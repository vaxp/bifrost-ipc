/**
 * Bifrost - Command Layer Implementation
 *
 * Shared-memory structures (per participant index i):
 *
 *   catalog[i]  : seqlock-protected array of bifrost_cmd_desc_t (writer -> readers)
 *   inbox[i]    : bounded MPSC ring of requests  (readers -> writer i)
 *   replybox[i] : bounded MPSC ring of replies   (writers -> reader i)
 *
 * Rings follow Dmitry Vyukov's bounded queue protocol: every cell carries a
 * sequence number, so producers claim a cell with one CAS on enqueue_pos and
 * publish it with one release store; the single consumer needs no atomic RMW
 * at all. A consumer may sleep on a futex; producers only issue the wake
 * syscall when a consumer has announced that it is sleeping.
 *
 * Nothing in this file is reached from the data path (alloc/publish/get).
 */

#define _GNU_SOURCE
#include "bifrost_internal.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sched.h>
#include <time.h>
#include <limits.h>
#include <linux/futex.h>
#include <sys/syscall.h>

/* ============================================================================
 * Low-level helpers
 * ============================================================================ */

#define BIFROST_SPIN_ITERS        2048u           /* Busy-wait before sleeping */
#define BIFROST_CALL_SLICE_NS     (10ull * 1000 * 1000)  /* Liveness check period in bifrost_reader_call */
#define BIFROST_TICKET_SEQ_MASK   ((1ull << 40) - 1)

static inline void cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    atomic_signal_fence(memory_order_seq_cst);
#endif
}

static inline bifrost_error_t fail(bifrost_error_t e) {
    bifrost_set_error(e);
    return e;
}

static bool pid_alive(pid_t pid) {
    if (pid <= 0) return false;
    return kill(pid, 0) == 0 || errno == EPERM;
}

static void futex_wake_all(_Atomic uint32_t* addr) {
    /* Shared (not PRIVATE) futex: waiters live in other processes */
    syscall(SYS_futex, (void*)addr, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
}

static void futex_wait(_Atomic uint32_t* addr, uint32_t expected, const struct timespec* ts) {
    syscall(SYS_futex, (void*)addr, FUTEX_WAIT, expected, ts, NULL, 0);
}

/* ============================================================================
 * Region accessors
 * ============================================================================ */

#define RING_CELLS(r) ((char*)(r) + sizeof(bifrost_ring_t))

static inline bifrost_ring_t* inbox_of(bifrost_pool_t* pool, uint32_t idx) {
    bifrost_pool_header_t* h = pool->header;
    return (bifrost_ring_t*)((char*)pool->base + h->inboxes_offset + (size_t)idx * h->inbox_stride);
}

static inline bifrost_ring_t* replybox_of(bifrost_pool_t* pool, uint32_t idx) {
    bifrost_pool_header_t* h = pool->header;
    return (bifrost_ring_t*)((char*)pool->base + h->replyboxes_offset + (size_t)idx * h->replybox_stride);
}

static inline bifrost_cmd_catalog_t* catalog_of(bifrost_pool_t* pool, uint32_t idx) {
    bifrost_pool_header_t* h = pool->header;
    return (bifrost_cmd_catalog_t*)((char*)pool->base + h->catalogs_offset + (size_t)idx * h->catalog_stride);
}

static inline bifrost_cmd_desc_t* catalog_entries(bifrost_cmd_catalog_t* c) {
    return (bifrost_cmd_desc_t*)((char*)c + sizeof(bifrost_cmd_catalog_t));
}

static inline bifrost_ring_cell_t* ring_cell(bifrost_ring_t* r, uint32_t mask, uint32_t cell_size,
                                        uint64_t pos) {
    return (bifrost_ring_cell_t*)(RING_CELLS(r) + (size_t)(pos & mask) * cell_size);
}

/* ============================================================================
 * Bounded MPSC ring (Vyukov)
 * ============================================================================ */

static void ring_init(bifrost_ring_t* r, uint32_t depth, uint32_t cell_size) {
    atomic_store_explicit(&r->enqueue_pos, 0, memory_order_relaxed);
    atomic_store_explicit(&r->waiters, 0, memory_order_relaxed);
    atomic_store_explicit(&r->futex_word, 0, memory_order_relaxed);
    atomic_store_explicit(&r->dequeue_pos, 0, memory_order_relaxed);
    atomic_store_explicit(&r->dropped, 0, memory_order_relaxed);
    for (uint32_t j = 0; j < depth; j++) {
        bifrost_ring_cell_t* c = (bifrost_ring_cell_t*)(RING_CELLS(r) + (size_t)j * cell_size);
        atomic_store_explicit(&c->seq, j, memory_order_relaxed);
    }
}

/** Producer: claim a cell. Returns NULL if the ring is full (never blocks). */
static bifrost_ring_cell_t* ring_reserve(bifrost_ring_t* r, uint32_t mask, uint32_t cell_size,
                                    uint64_t* pos_out) {
    uint64_t pos = atomic_load_explicit(&r->enqueue_pos, memory_order_relaxed);
    for (;;) {
        bifrost_ring_cell_t* c = ring_cell(r, mask, cell_size, pos);
        uint64_t seq = atomic_load_explicit(&c->seq, memory_order_acquire);
        int64_t dif = (int64_t)(seq - pos);
        if (dif == 0) {
            if (atomic_compare_exchange_weak_explicit(&r->enqueue_pos, &pos, pos + 1,
                                                      memory_order_relaxed,
                                                      memory_order_relaxed)) {
                *pos_out = pos;
                return c;
            }
            /* CAS failure reloaded pos */
        } else if (dif < 0) {
            return NULL;    /* Full */
        } else {
            pos = atomic_load_explicit(&r->enqueue_pos, memory_order_relaxed);
        }
    }
}

/** Producer: publish a claimed cell and wake the consumer if it sleeps. */
static void ring_commit(bifrost_ring_t* r, bifrost_ring_cell_t* c, uint64_t pos) {
    atomic_store_explicit(&c->seq, pos + 1, memory_order_release);
    /* Dekker handshake with ring_wait(): publish-then-check vs announce-then-check */
    atomic_thread_fence(memory_order_seq_cst);
    if (atomic_load_explicit(&r->waiters, memory_order_relaxed)) {
        atomic_fetch_add_explicit(&r->futex_word, 1, memory_order_release);
        futex_wake_all(&r->futex_word);
    }
}

/** Consumer: return the cell at pos if it is ready, else NULL. */
static inline bifrost_ring_cell_t* ring_peek(bifrost_ring_t* r, uint32_t mask, uint32_t cell_size,
                                        uint64_t pos) {
    bifrost_ring_cell_t* c = ring_cell(r, mask, cell_size, pos);
    uint64_t seq = atomic_load_explicit(&c->seq, memory_order_acquire);
    return (seq == pos + 1) ? c : NULL;
}

/** Consumer: hand the cell back to producers. */
static inline void ring_release(bifrost_ring_t* r, bifrost_ring_cell_t* c, uint32_t mask, uint64_t pos) {
    atomic_store_explicit(&c->seq, pos + (uint64_t)mask + 1, memory_order_release);
    atomic_store_explicit(&r->dequeue_pos, pos + 1, memory_order_relaxed);
}

/** Consumer: wait until the cell at pos is ready (spin, then futex). */
static bifrost_error_t ring_wait(bifrost_ring_t* r, uint32_t mask, uint32_t cell_size,
                            uint64_t pos, uint64_t timeout_ns) {
    if (ring_peek(r, mask, cell_size, pos)) return BIFROST_OK;
    if (timeout_ns == 0) return BIFROST_ERR_TIMEOUT;
    
    const bool forever = (timeout_ns == UINT64_MAX);
    const uint64_t start = bifrost_get_timestamp();
    
    for (uint32_t i = 1; i <= BIFROST_SPIN_ITERS; i++) {
        cpu_relax();
        if (ring_peek(r, mask, cell_size, pos)) return BIFROST_OK;
        if (!forever && (i & 63u) == 0 && bifrost_get_timestamp() - start >= timeout_ns) {
            return BIFROST_ERR_TIMEOUT;
        }
    }
    
    for (;;) {
        uint32_t v = atomic_load_explicit(&r->futex_word, memory_order_acquire);
        atomic_store_explicit(&r->waiters, 1, memory_order_seq_cst);
        atomic_thread_fence(memory_order_seq_cst);
        
        if (ring_peek(r, mask, cell_size, pos)) {
            atomic_store_explicit(&r->waiters, 0, memory_order_relaxed);
            return BIFROST_OK;
        }
        
        struct timespec ts, *tsp = NULL;
        if (!forever) {
            uint64_t elapsed = bifrost_get_timestamp() - start;
            if (elapsed >= timeout_ns) {
                atomic_store_explicit(&r->waiters, 0, memory_order_relaxed);
                return BIFROST_ERR_TIMEOUT;
            }
            uint64_t rem = timeout_ns - elapsed;
            ts.tv_sec = (time_t)(rem / 1000000000ull);
            ts.tv_nsec = (long)(rem % 1000000000ull);
            tsp = &ts;
        }
        
        futex_wait(&r->futex_word, v, tsp);
        atomic_store_explicit(&r->waiters, 0, memory_order_relaxed);
        
        if (ring_peek(r, mask, cell_size, pos)) return BIFROST_OK;
    }
}

/* ============================================================================
 * Catalog seqlock
 * ============================================================================ */

static void cat_write_begin(bifrost_cmd_catalog_t* c) {
    uint32_t s = atomic_load_explicit(&c->seq, memory_order_relaxed);
    atomic_store_explicit(&c->seq, s + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
}

static void cat_write_end(bifrost_cmd_catalog_t* c) {
    uint32_t s = atomic_load_explicit(&c->seq, memory_order_relaxed);
    atomic_store_explicit(&c->seq, s + 1, memory_order_release);
}

/** Wait for an even sequence. Fails if a writer died mid-update. */
static bool cat_read_begin(bifrost_cmd_catalog_t* c, uint32_t* seq_out) {
    for (uint32_t spins = 0; spins < (1u << 20); spins++) {
        uint32_t s = atomic_load_explicit(&c->seq, memory_order_acquire);
        if ((s & 1u) == 0) {
            *seq_out = s;
            return true;
        }
        if ((spins & 255u) == 255u) sched_yield(); else cpu_relax();
    }
    return false;
}

static bool cat_read_changed(bifrost_cmd_catalog_t* c, uint32_t s) {
    atomic_thread_fence(memory_order_acquire);
    return atomic_load_explicit(&c->seq, memory_order_relaxed) != s;
}

/* ============================================================================
 * Region initialization (Director)
 * ============================================================================ */

void bifrost_cmd_init_region(bifrost_pool_t* pool) {
    if (!bifrost_cmd_enabled(pool)) return;
    
    bifrost_pool_header_t* h = pool->header;
    uint32_t n = bifrost_atomic_load(&h->max_participants);
    
    for (uint32_t i = 0; i < n; i++) {
        bifrost_cmd_catalog_t* cat = catalog_of(pool, i);
        atomic_store_explicit(&cat->seq, 0, memory_order_relaxed);
        atomic_store_explicit(&cat->count, 0, memory_order_relaxed);
        
        ring_init(inbox_of(pool, i), h->cmd_inbox_depth, h->cmd_req_cell_size);
        ring_init(replybox_of(pool, i), h->cmd_reply_depth, h->cmd_rep_cell_size);
    }
}

/* ============================================================================
 * Participant resolution
 * ============================================================================ */

static bifrost_participant_t* live_participant(bifrost_pool_t* pool, uint32_t id, uint32_t gen,
                                          bifrost_participant_type_t type, uint32_t* idx_out) {
    uint32_t idx = id - 1;  /* id == 0 wraps */
    if (idx >= bifrost_atomic_load(&pool->header->max_participants)) return NULL;
    
    bifrost_participant_t* p = &pool->participants[idx];
    if (bifrost_atomic_load(&p->active) != BIFROST_P_LIVE ||
        bifrost_atomic_load(&p->type) != (uint32_t)type ||
        bifrost_atomic_load(&p->generation) != gen) {
        return NULL;
    }
    if (idx_out) *idx_out = idx;
    return p;
}

/* ============================================================================
 * Writer side
 * ============================================================================ */

static void writer_free_locals(bifrost_writer_t* w) {
    free(w->cat_local);     w->cat_local = NULL;
    free(w->handlers);      w->handlers = NULL;
    free(w->handler_ctx);   w->handler_ctx = NULL;
    free(w->reply_scratch); w->reply_scratch = NULL;
}

int bifrost_cmd_writer_attach(bifrost_writer_t* w) {
    bifrost_pool_t* pool = w->pool;
    bifrost_pool_header_t* h = pool->header;
    
    w->uid = (uint32_t)getuid();
    w->inbox = NULL;
    w->inflight = NULL;
    w->inflight_idx = UINT32_MAX;
    
    if (!bifrost_cmd_enabled(pool)) return 0;
    
    uint32_t idx = w->participant_idx;
    w->cat_cap = h->cmd_max_per_writer;
    w->cat_count = 0;
    w->cat_local = calloc(w->cat_cap, sizeof(bifrost_cmd_desc_t));
    w->handlers = calloc(w->cat_cap, sizeof(bifrost_cmd_handler_fn));
    w->handler_ctx = calloc(w->cat_cap, sizeof(void*));
    w->reply_scratch = malloc(h->cmd_max_reply ? h->cmd_max_reply : 1);
    if (!w->cat_local || !w->handlers || !w->handler_ctx || !w->reply_scratch) {
        writer_free_locals(w);
        return -1;
    }
    
    w->inbox = inbox_of(pool, idx);
    w->inbox_cells = RING_CELLS(w->inbox);
    w->inbox_mask = h->cmd_inbox_depth - 1;
    w->req_cell_size = h->cmd_req_cell_size;
    /* Resume where the previous owner of this slot stopped. Leftover requests
     * target the previous generation and get rejected with NOTFOUND. */
    w->deq_pos = atomic_load_explicit(&w->inbox->dequeue_pos, memory_order_acquire);
    atomic_store_explicit(&w->inbox->dropped, 0, memory_order_relaxed);
    
    /* Start with an empty catalog (do not inherit a previous incarnation's) */
    w->catalog = catalog_of(pool, idx);
    w->cat_shared = catalog_entries(w->catalog);
    cat_write_begin(w->catalog);
    atomic_store_explicit(&w->catalog->count, 0, memory_order_relaxed);
    cat_write_end(w->catalog);
    
    return 0;
}

static uint32_t cat_find_id(const bifrost_writer_t* w, uint32_t cmd_id) {
    for (uint32_t i = 0; i < w->cat_count; i++) {
        if (w->cat_local[i].id == cmd_id) return i;
    }
    return UINT32_MAX;
}

/** Push a reply into the sender's reply box. Never blocks. */
static bifrost_error_t send_reply(bifrost_writer_t* w, const bifrost_cmd_request_t* req, int32_t status,
                             const void* data, uint32_t len) {
    bifrost_pool_t* pool = w->pool;
    bifrost_pool_header_t* h = pool->header;
    uint32_t ridx;
    
    if (!live_participant(pool, req->sender_id, req->sender_gen, BIFROST_PARTICIPANT_READER, &ridx)) {
        atomic_fetch_add_explicit(&w->inbox->dropped, 1, memory_order_relaxed);
        return BIFROST_ERR_NOTFOUND;
    }
    
    bifrost_ring_t* rb = replybox_of(pool, ridx);
    uint64_t pos;
    bifrost_ring_cell_t* c = ring_reserve(rb, h->cmd_reply_depth - 1, h->cmd_rep_cell_size, &pos);
    if (!c) {
        atomic_fetch_add_explicit(&w->inbox->dropped, 1, memory_order_relaxed);
        return BIFROST_ERR_FULL;
    }
    
    bifrost_cmd_reply_t* rep = (bifrost_cmd_reply_t*)BIFROST_CELL_PAYLOAD(c);
    rep->ticket = req->ticket;
    rep->cmd_id = req->cmd_id;
    rep->status = status;
    rep->writer_id = w->id;
    rep->writer_gen = w->gen;
    rep->target_gen = req->sender_gen;
    rep->len = len;
    memset(rep->reserved, 0, sizeof(rep->reserved));
    if (len) memcpy(rep->data, data, len);
    
    ring_commit(rb, c, pos);
    return BIFROST_OK;
}

/** Reply (if wanted) then release the in-flight request cell. */
static bifrost_error_t complete_inflight(bifrost_writer_t* w, int32_t status,
                                    const void* data, uint32_t len) {
    bifrost_ring_cell_t* c = w->inflight;
    const bifrost_cmd_request_t* req = (const bifrost_cmd_request_t*)BIFROST_CELL_PAYLOAD(c);
    bifrost_error_t rc = BIFROST_OK;
    
    if (!w->inflight_noreply) {
        if (len > w->pool->header->cmd_max_reply || (len && !data)) {
            status = BIFROST_ERR_INVALID;
            data = NULL;
            len = 0;
            rc = BIFROST_ERR_INVALID;
        }
        /* Reply first: `data` may point into the request cell (e.g. echo) */
        bifrost_error_t r2 = send_reply(w, req, status, data, len);
        if (rc == BIFROST_OK) rc = r2;
    }
    
    ring_release(w->inbox, c, w->inbox_mask, w->deq_pos);
    w->deq_pos++;
    w->inflight = NULL;
    w->inflight_idx = UINT32_MAX;
    return rc;
}

void bifrost_cmd_writer_detach(bifrost_writer_t* w) {
    if (w->inbox) {
        if (w->inflight) {
            complete_inflight(w, BIFROST_ERR_NOTFOUND, NULL, 0);
        }
        
        /* Hide the catalog */
        cat_write_begin(w->catalog);
        atomic_store_explicit(&w->catalog->count, 0, memory_order_relaxed);
        cat_write_end(w->catalog);
        w->cat_count = 0;
        
        /* Fail every queued request fast instead of letting callers time out */
        bifrost_ring_cell_t* c;
        while ((c = ring_peek(w->inbox, w->inbox_mask, w->req_cell_size, w->deq_pos))) {
            const bifrost_cmd_request_t* req = (const bifrost_cmd_request_t*)BIFROST_CELL_PAYLOAD(c);
            w->inflight = c;
            w->inflight_noreply = (req->flags & BIFROST_SEND_F_NO_REPLY) != 0;
            complete_inflight(w, BIFROST_ERR_NOTFOUND, NULL, 0);
        }
        w->inbox = NULL;
    }
    writer_free_locals(w);
}

bifrost_error_t bifrost_writer_register_command(bifrost_writer_t* w, const bifrost_cmd_desc_t* desc) {
    if (!w || !desc) return fail(BIFROST_ERR_INVALID);
    if (!w->inbox) return fail(BIFROST_ERR_DISABLED);
    
    bifrost_pool_header_t* h = w->pool->header;
    
    if (desc->id == 0 || desc->name[0] == '\0' ||
        memchr(desc->name, '\0', BIFROST_CMD_NAME_MAX) == NULL ||
        desc->arg_min > desc->arg_max ||
        desc->arg_max > h->cmd_max_args ||
        desc->reply_max > h->cmd_max_reply) {
        return fail(BIFROST_ERR_INVALID);
    }
    
    for (uint32_t i = 0; i < w->cat_count; i++) {
        if (w->cat_local[i].id == desc->id ||
            strncmp(w->cat_local[i].name, desc->name, BIFROST_CMD_NAME_MAX) == 0) {
            return fail(BIFROST_ERR_EXISTS);
        }
    }
    
    if (w->cat_count >= w->cat_cap) return fail(BIFROST_ERR_FULL);
    
    /* Normalize: zero padding, terminated strings, reserved = 0 */
    bifrost_cmd_desc_t e;
    memset(&e, 0, sizeof(e));
    e.id = desc->id;
    e.flags = desc->flags;
    e.arg_min = desc->arg_min;
    e.arg_max = desc->arg_max;
    e.reply_max = desc->reply_max;
    memcpy(e.name, desc->name, strnlen(desc->name, BIFROST_CMD_NAME_MAX - 1));
    memcpy(e.desc, desc->desc, strnlen(desc->desc, BIFROST_CMD_DESC_MAX - 1));
    
    uint32_t n = w->cat_count;
    w->cat_local[n] = e;
    w->handlers[n] = NULL;
    w->handler_ctx[n] = NULL;
    
    cat_write_begin(w->catalog);
    memcpy(&w->cat_shared[n], &e, sizeof(e));
    atomic_store_explicit(&w->catalog->count, n + 1, memory_order_relaxed);
    cat_write_end(w->catalog);
    
    w->cat_count = n + 1;
    return BIFROST_OK;
}

bifrost_error_t bifrost_writer_unregister_command(bifrost_writer_t* w, uint32_t cmd_id) {
    if (!w) return fail(BIFROST_ERR_INVALID);
    if (!w->inbox) return fail(BIFROST_ERR_DISABLED);
    
    uint32_t i = cat_find_id(w, cmd_id);
    if (i == UINT32_MAX) return fail(BIFROST_ERR_NOTFOUND);
    
    uint32_t last = w->cat_count - 1;
    w->cat_local[i] = w->cat_local[last];
    w->handlers[i] = w->handlers[last];
    w->handler_ctx[i] = w->handler_ctx[last];
    
    cat_write_begin(w->catalog);
    memcpy(&w->cat_shared[i], &w->cat_shared[last], sizeof(bifrost_cmd_desc_t));
    atomic_store_explicit(&w->catalog->count, last, memory_order_relaxed);
    cat_write_end(w->catalog);
    
    w->cat_count = last;
    
    if (w->inflight_idx == i) w->inflight_idx = UINT32_MAX;
    else if (w->inflight_idx == last) w->inflight_idx = i;
    return BIFROST_OK;
}

const bifrost_cmd_request_t* bifrost_writer_poll_command(bifrost_writer_t* w) {
    if (!w) { bifrost_set_error(BIFROST_ERR_INVALID); return NULL; }
    if (!w->inbox) { bifrost_set_error(BIFROST_ERR_DISABLED); return NULL; }
    if (w->inflight) { bifrost_set_error(BIFROST_ERR_BUSY); return NULL; }
    
    const uint32_t max_args = w->pool->header->cmd_max_args;
    
    for (;;) {
        bifrost_ring_cell_t* c = ring_peek(w->inbox, w->inbox_mask, w->req_cell_size, w->deq_pos);
        if (!c) return NULL;   /* Empty: one acquire load */
        
        const bifrost_cmd_request_t* req = (const bifrost_cmd_request_t*)BIFROST_CELL_PAYLOAD(c);
        int32_t st = BIFROST_OK;
        uint32_t i = UINT32_MAX;
        
        if (req->target_gen != w->gen) {
            st = BIFROST_ERR_NOTFOUND;           /* Meant for a previous incarnation */
        } else if (req->arg_len > max_args) {
            st = BIFROST_ERR_INVALID;
        } else if ((i = cat_find_id(w, req->cmd_id)) == UINT32_MAX) {
            st = BIFROST_ERR_NOTFOUND;           /* Not in our catalog */
        } else {
            const bifrost_cmd_desc_t* d = &w->cat_local[i];
            if (req->arg_len < d->arg_min || req->arg_len > d->arg_max) {
                st = BIFROST_ERR_INVALID;
            } else if ((d->flags & BIFROST_CMD_F_SAME_UID) && req->sender_uid != w->uid) {
                st = BIFROST_ERR_PERMISSION;
            } else if (w->filter) {
                st = w->filter(req, d, w->filter_ctx);
            }
        }
        
        w->inflight = c;
        w->inflight_idx = i;
        w->inflight_noreply = (req->flags & BIFROST_SEND_F_NO_REPLY) != 0;
        
        if (st == BIFROST_OK) return req;
        
        /* Rejected: answer automatically and keep looking */
        complete_inflight(w, st, NULL, 0);
    }
}

bifrost_error_t bifrost_writer_command_done(bifrost_writer_t* w, const bifrost_cmd_request_t* req,
                                  int32_t status, const void* reply, uint32_t reply_len) {
    if (!w || !req || !w->inflight ||
        req != (const bifrost_cmd_request_t*)BIFROST_CELL_PAYLOAD(w->inflight)) {
        return fail(BIFROST_ERR_INVALID);
    }
    bifrost_error_t rc = complete_inflight(w, status, reply, reply_len);
    if (rc != BIFROST_OK) bifrost_set_error(rc);
    return rc;
}

bifrost_error_t bifrost_writer_wait_command(bifrost_writer_t* w, uint64_t timeout_ns) {
    if (!w) return fail(BIFROST_ERR_INVALID);
    if (!w->inbox) return fail(BIFROST_ERR_DISABLED);
    if (w->inflight) return BIFROST_OK;
    
    bifrost_error_t rc = ring_wait(w->inbox, w->inbox_mask, w->req_cell_size, w->deq_pos, timeout_ns);
    if (rc != BIFROST_OK) bifrost_set_error(rc);
    return rc;
}

bifrost_error_t bifrost_writer_on_command(bifrost_writer_t* w, uint32_t cmd_id,
                                bifrost_cmd_handler_fn fn, void* ctx) {
    if (!w) return fail(BIFROST_ERR_INVALID);
    if (!w->inbox) return fail(BIFROST_ERR_DISABLED);
    
    uint32_t i = cat_find_id(w, cmd_id);
    if (i == UINT32_MAX) return fail(BIFROST_ERR_NOTFOUND);
    
    w->handlers[i] = fn;
    w->handler_ctx[i] = fn ? ctx : NULL;
    return BIFROST_OK;
}

void bifrost_writer_set_command_filter(bifrost_writer_t* w, bifrost_cmd_filter_fn fn, void* ctx) {
    if (!w) return;
    w->filter = fn;
    w->filter_ctx = fn ? ctx : NULL;
}

uint32_t bifrost_writer_dispatch(bifrost_writer_t* w, uint32_t max_cmds) {
    if (!w || !w->inbox) return 0;
    
    const uint32_t cap = w->pool->header->cmd_max_reply;
    uint32_t n = 0;
    
    while (n < max_cmds) {
        const bifrost_cmd_request_t* req = bifrost_writer_poll_command(w);
        if (!req) break;
        
        uint32_t i = w->inflight_idx;
        bifrost_cmd_handler_fn fn = (i != UINT32_MAX) ? w->handlers[i] : NULL;
        
        if (!fn) {
            complete_inflight(w, BIFROST_ERR_NOTFOUND, NULL, 0);
        } else {
            uint32_t len = cap;
            int32_t st = fn(req, w->reply_scratch, &len, w->handler_ctx[i]);
            if (len > cap) { st = BIFROST_ERR_INVALID; len = 0; }
            complete_inflight(w, st, w->reply_scratch, len);
        }
        n++;
    }
    return n;
}

bifrost_error_t bifrost_writer_get_sender_name(bifrost_writer_t* w, const bifrost_cmd_request_t* req,
                                     char* out, size_t out_size) {
    if (!w || !req || !out || out_size == 0) return fail(BIFROST_ERR_INVALID);
    
    bifrost_participant_t* p = live_participant(w->pool, req->sender_id, req->sender_gen,
                                           BIFROST_PARTICIPANT_READER, NULL);
    if (!p) {
        out[0] = '\0';
        return fail(BIFROST_ERR_NOTFOUND);
    }
    size_t n = strnlen(p->name, BIFROST_NAME_MAX - 1);
    if (n >= out_size) n = out_size - 1;
    memcpy(out, p->name, n);
    out[n] = '\0';
    return BIFROST_OK;
}

uint64_t bifrost_writer_replies_dropped(bifrost_writer_t* w) {
    if (!w || !w->inbox) return 0;
    return atomic_load_explicit(&w->inbox->dropped, memory_order_relaxed);
}

/* ============================================================================
 * Reader side
 * ============================================================================ */

int bifrost_cmd_reader_attach(bifrost_reader_t* r) {
    bifrost_pool_t* pool = r->pool;
    bifrost_pool_header_t* h = pool->header;
    
    r->replybox = NULL;
    r->inflight = NULL;
    r->next_ticket = 1;
    
    if (!bifrost_cmd_enabled(pool)) return 0;
    
    r->stash = malloc((size_t)h->cmd_reply_depth * h->cmd_rep_cell_size);
    r->pending = calloc(bifrost_atomic_load(&h->max_participants), sizeof(uint32_t));
    if (!r->stash || !r->pending) {
        free(r->stash); r->stash = NULL;
        free(r->pending); r->pending = NULL;
        return -1;
    }
    r->pending_total = 0;
    r->stash_head = r->stash_count = 0;
    r->stash_inflight = false;
    
    r->replybox = replybox_of(pool, r->participant_idx);
    r->reply_cells = RING_CELLS(r->replybox);
    r->reply_mask = h->cmd_reply_depth - 1;
    r->rep_cell_size = h->cmd_rep_cell_size;
    r->deq_pos = atomic_load_explicit(&r->replybox->dequeue_pos, memory_order_acquire);
    return 0;
}

void bifrost_cmd_reader_detach(bifrost_reader_t* r) {
    if (r->replybox && r->inflight) {
        ring_release(r->replybox, r->inflight, r->reply_mask, r->deq_pos);
        r->deq_pos++;
        r->inflight = NULL;
    }
    r->replybox = NULL;
    free(r->stash);
    r->stash = NULL;
    free(r->pending);
    r->pending = NULL;
}

/** A reply addressed to us left the ring: give the credit back. */
static inline void credit_return(bifrost_reader_t* r, const bifrost_cmd_reply_t* rep) {
    uint32_t idx = rep->writer_id - 1;
    if (idx < bifrost_atomic_load(&r->pool->header->max_participants) && r->pending[idx]) {
        r->pending[idx]--;
        r->pending_total--;
    }
}

/** The writer at idx is gone for good: its outstanding replies will never come. */
static inline void credit_forfeit(bifrost_reader_t* r, uint32_t writer_id) {
    uint32_t idx = writer_id - 1;
    if (idx < bifrost_atomic_load(&r->pool->header->max_participants)) {
        r->pending_total -= r->pending[idx];
        r->pending[idx] = 0;
    }
}

static inline bifrost_cmd_reply_t* stash_at(bifrost_reader_t* r, uint32_t slot) {
    return (bifrost_cmd_reply_t*)(r->stash + (size_t)slot * r->rep_cell_size + 8);
}

/** Copy a reply out of the ring into the local stash (drops oldest if full). */
static void stash_push(bifrost_reader_t* r, const bifrost_cmd_reply_t* rep) {
    uint32_t depth = r->reply_mask + 1;
    if (r->stash_count == depth) {
        r->stash_head = (r->stash_head + 1) & r->reply_mask;
        r->stash_count--;
    }
    uint32_t slot = (r->stash_head + r->stash_count) & r->reply_mask;
    size_t n = sizeof(bifrost_cmd_reply_t) + rep->len;
    if (n > r->rep_cell_size - 8) n = r->rep_cell_size - 8;
    (void)depth;
    memcpy(stash_at(r, slot), rep, n);
    r->stash_count++;
}

bifrost_writer_ref_t bifrost_reader_resolve_writer(bifrost_reader_t* r, const char* writer_name) {
    bifrost_writer_ref_t ref = {0, 0};
    if (!r || !writer_name) {
        bifrost_set_error(BIFROST_ERR_INVALID);
        return ref;
    }
    
    bifrost_pool_t* pool = r->pool;
    uint32_t max = bifrost_atomic_load(&pool->header->max_participants);
    for (uint32_t i = 0; i < max; i++) {
        bifrost_participant_t* p = &pool->participants[i];
        if (bifrost_atomic_load(&p->active) == BIFROST_P_LIVE &&
            bifrost_atomic_load(&p->type) == BIFROST_PARTICIPANT_WRITER &&
            strcmp(p->name, writer_name) == 0) {
            uint32_t gen = bifrost_atomic_load(&p->generation);
            if (bifrost_atomic_load(&p->active) == BIFROST_P_LIVE) {
                ref.id = i + 1;
                ref.gen = gen;
            }
            break;
        }
    }
    if (ref.id == 0) bifrost_set_error(BIFROST_ERR_NOTFOUND);
    return ref;
}

bool bifrost_reader_writer_alive(bifrost_reader_t* r, bifrost_writer_ref_t wr) {
    if (!r) return false;
    bifrost_participant_t* p = live_participant(r->pool, wr.id, wr.gen, BIFROST_PARTICIPANT_WRITER, NULL);
    if (p && pid_alive(bifrost_atomic_load(&p->pid))) return true;
    
    /* Dead or gone. Unless the slot already hosts a new writer (which will
     * answer our leftovers with NOTFOUND), reclaim the credits it owes us. */
    uint32_t idx = wr.id - 1;
    if (r->pending && idx < bifrost_atomic_load(&r->pool->header->max_participants)) {
        bifrost_participant_t* q = &r->pool->participants[idx];
        bool reused = bifrost_atomic_load(&q->active) == BIFROST_P_LIVE &&
                      bifrost_atomic_load(&q->generation) != wr.gen;
        if (!reused) credit_forfeit(r, wr.id);
    }
    return false;
}

uint32_t bifrost_reader_list_commands(bifrost_reader_t* r, bifrost_writer_ref_t wr,
                                 bifrost_cmd_info_t* out, uint32_t max_count) {
    if (!r || !out || max_count == 0) { bifrost_set_error(BIFROST_ERR_INVALID); return 0; }
    if (!bifrost_cmd_enabled(r->pool)) { bifrost_set_error(BIFROST_ERR_DISABLED); return 0; }
    
    bifrost_pool_t* pool = r->pool;
    uint32_t idx;
    if (!live_participant(pool, wr.id, wr.gen, BIFROST_PARTICIPANT_WRITER, &idx)) {
        bifrost_set_error(BIFROST_ERR_NOTFOUND);
        return 0;
    }
    
    bifrost_cmd_catalog_t* cat = catalog_of(pool, idx);
    bifrost_cmd_desc_t* entries = catalog_entries(cat);
    uint32_t cap = pool->header->cmd_max_per_writer;
    
    for (int attempt = 0; attempt < 10000; attempt++) {
        uint32_t s;
        if (!cat_read_begin(cat, &s)) break;
        
        uint32_t n = atomic_load_explicit(&cat->count, memory_order_relaxed);
        if (n > cap) n = cap;
        if (n > max_count) n = max_count;
        memcpy(out, entries, (size_t)n * sizeof(bifrost_cmd_desc_t));
        
        if (cat_read_changed(cat, s)) continue;
        
        /* The writer may have been replaced while we copied */
        if (!live_participant(pool, wr.id, wr.gen, BIFROST_PARTICIPANT_WRITER, NULL)) {
            bifrost_set_error(BIFROST_ERR_NOTFOUND);
            return 0;
        }
        return n;
    }
    bifrost_set_error(BIFROST_ERR_BUSY);
    return 0;
}

uint32_t bifrost_reader_find_command(bifrost_reader_t* r, bifrost_writer_ref_t wr,
                                const char* cmd_name, bifrost_cmd_info_t* info_out) {
    if (!r || !cmd_name) { bifrost_set_error(BIFROST_ERR_INVALID); return 0; }
    if (!bifrost_cmd_enabled(r->pool)) { bifrost_set_error(BIFROST_ERR_DISABLED); return 0; }
    
    bifrost_pool_t* pool = r->pool;
    uint32_t idx;
    if (!live_participant(pool, wr.id, wr.gen, BIFROST_PARTICIPANT_WRITER, &idx)) {
        bifrost_set_error(BIFROST_ERR_NOTFOUND);
        return 0;
    }
    
    bifrost_cmd_catalog_t* cat = catalog_of(pool, idx);
    bifrost_cmd_desc_t* entries = catalog_entries(cat);
    uint32_t cap = pool->header->cmd_max_per_writer;
    
    for (int attempt = 0; attempt < 10000; attempt++) {
        uint32_t s;
        if (!cat_read_begin(cat, &s)) break;
        
        uint32_t n = atomic_load_explicit(&cat->count, memory_order_relaxed);
        if (n > cap) n = cap;
        
        bifrost_cmd_desc_t found;
        uint32_t id = 0;
        for (uint32_t j = 0; j < n; j++) {
            if (strncmp(entries[j].name, cmd_name, BIFROST_CMD_NAME_MAX) == 0) {
                memcpy(&found, &entries[j], sizeof(found));
                id = found.id;
                break;
            }
        }
        
        if (cat_read_changed(cat, s)) continue;
        
        if (!live_participant(pool, wr.id, wr.gen, BIFROST_PARTICIPANT_WRITER, NULL)) {
            bifrost_set_error(BIFROST_ERR_NOTFOUND);
            return 0;
        }
        if (id == 0) {
            bifrost_set_error(BIFROST_ERR_NOTFOUND);
            return 0;
        }
        if (info_out) *info_out = found;
        return id;
    }
    bifrost_set_error(BIFROST_ERR_BUSY);
    return 0;
}

bifrost_error_t bifrost_reader_send_command(bifrost_reader_t* r, bifrost_writer_ref_t wr,
                                  uint32_t cmd_id, const void* args, uint32_t arg_len,
                                  uint32_t flags, uint64_t* ticket_out) {
    if (!r) return fail(BIFROST_ERR_INVALID);
    if (!r->replybox) return fail(BIFROST_ERR_DISABLED);
    
    bifrost_pool_t* pool = r->pool;
    bifrost_pool_header_t* h = pool->header;
    
    if (cmd_id == 0 || arg_len > h->cmd_max_args || (arg_len && !args)) {
        return fail(BIFROST_ERR_INVALID);
    }
    
    uint32_t idx;
    if (!live_participant(pool, wr.id, wr.gen, BIFROST_PARTICIPANT_WRITER, &idx)) {
        return fail(BIFROST_ERR_NOTFOUND);
    }
    
    /* Flow control: never ask for more replies than our reply box can hold */
    const bool want_reply = (flags & BIFROST_SEND_F_NO_REPLY) == 0;
    if (want_reply && r->pending_total + r->stash_count > r->reply_mask) {
        return fail(BIFROST_ERR_FULL);
    }
    
    bifrost_ring_t* ib = inbox_of(pool, idx);
    uint64_t pos;
    bifrost_ring_cell_t* c = ring_reserve(ib, h->cmd_inbox_depth - 1, h->cmd_req_cell_size, &pos);
    if (!c) return fail(BIFROST_ERR_FULL);
    
    uint64_t ticket = ((uint64_t)r->id << 40) | (r->next_ticket++ & BIFROST_TICKET_SEQ_MASK);
    bifrost_participant_t* self = &pool->participants[r->participant_idx];
    
    bifrost_cmd_request_t* req = (bifrost_cmd_request_t*)BIFROST_CELL_PAYLOAD(c);
    req->ticket = ticket;
    req->cmd_id = cmd_id;
    req->flags = flags;
    req->sender_id = r->id;
    req->sender_gen = r->gen;
    req->target_gen = wr.gen;
    req->arg_len = arg_len;
    req->sender_pid = (int32_t)bifrost_atomic_load(&self->pid);   /* cached, no syscall */
    req->sender_uid = r->uid;
    if (arg_len) memcpy(req->args, args, arg_len);
    
    ring_commit(ib, c, pos);
    
    if (want_reply) {
        r->pending[idx]++;
        r->pending_total++;
    }
    if (ticket_out) *ticket_out = ticket;
    return BIFROST_OK;
}

const bifrost_cmd_reply_t* bifrost_reader_poll_reply(bifrost_reader_t* r) {
    if (!r) { bifrost_set_error(BIFROST_ERR_INVALID); return NULL; }
    if (!r->replybox) { bifrost_set_error(BIFROST_ERR_DISABLED); return NULL; }
    if (r->inflight || r->stash_inflight) { bifrost_set_error(BIFROST_ERR_BUSY); return NULL; }
    
    /* Replies set aside by bifrost_reader_call() come first (arrival order) */
    if (r->stash_count) {
        r->stash_inflight = true;
        return stash_at(r, r->stash_head);
    }
    
    for (;;) {
        bifrost_ring_cell_t* c = ring_peek(r->replybox, r->reply_mask, r->rep_cell_size, r->deq_pos);
        if (!c) return NULL;
        
        const bifrost_cmd_reply_t* rep = (const bifrost_cmd_reply_t*)BIFROST_CELL_PAYLOAD(c);
        if (rep->target_gen != r->gen) {
            /* Addressed to a previous incarnation of this reader slot */
            ring_release(r->replybox, c, r->reply_mask, r->deq_pos);
            r->deq_pos++;
            continue;
        }
        r->inflight = c;
        credit_return(r, rep);
        return rep;
    }
}

void bifrost_reader_reply_done(bifrost_reader_t* r, const bifrost_cmd_reply_t* rep) {
    if (!r || !rep) return;
    
    if (r->stash_inflight && rep == stash_at(r, r->stash_head)) {
        r->stash_head = (r->stash_head + 1) & r->reply_mask;
        r->stash_count--;
        r->stash_inflight = false;
        return;
    }
    if (r->inflight && rep == (const bifrost_cmd_reply_t*)BIFROST_CELL_PAYLOAD(r->inflight)) {
        ring_release(r->replybox, r->inflight, r->reply_mask, r->deq_pos);
        r->deq_pos++;
        r->inflight = NULL;
    }
}

bifrost_error_t bifrost_reader_wait_reply(bifrost_reader_t* r, uint64_t timeout_ns) {
    if (!r) return fail(BIFROST_ERR_INVALID);
    if (!r->replybox) return fail(BIFROST_ERR_DISABLED);
    if (r->stash_count || r->inflight) return BIFROST_OK;
    
    bifrost_error_t rc = ring_wait(r->replybox, r->reply_mask, r->rep_cell_size, r->deq_pos, timeout_ns);
    if (rc != BIFROST_OK) bifrost_set_error(rc);
    return rc;
}

bifrost_error_t bifrost_reader_call(bifrost_reader_t* r, bifrost_writer_ref_t wr,
                          uint32_t cmd_id, const void* args, uint32_t arg_len,
                          void* reply_buf, uint32_t* reply_len,
                          int32_t* status_out, uint64_t timeout_ns) {
    if (!r) return fail(BIFROST_ERR_INVALID);
    if (!r->replybox) return fail(BIFROST_ERR_DISABLED);
    if (r->inflight || r->stash_inflight) return fail(BIFROST_ERR_BUSY);
    
    uint32_t cap = (reply_buf && reply_len) ? *reply_len : 0;
    
    uint64_t ticket;
    bifrost_error_t rc = bifrost_reader_send_command(r, wr, cmd_id, args, arg_len, 0, &ticket);
    if (rc != BIFROST_OK) return rc;
    
    const bool forever = (timeout_ns == UINT64_MAX);
    const uint64_t start = bifrost_get_timestamp();
    
    for (;;) {
        bifrost_ring_cell_t* c;
        while ((c = ring_peek(r->replybox, r->reply_mask, r->rep_cell_size, r->deq_pos))) {
            const bifrost_cmd_reply_t* rep = (const bifrost_cmd_reply_t*)BIFROST_CELL_PAYLOAD(c);
            
            if (rep->target_gen == r->gen) {
                credit_return(r, rep);
                if (rep->ticket == ticket) {
                    if (status_out) *status_out = rep->status;
                    if (reply_len) {
                        uint32_t n = rep->len < cap ? rep->len : cap;
                        if (n) memcpy(reply_buf, rep->data, n);
                        *reply_len = rep->len;  /* > capacity means truncated */
                    }
                    ring_release(r->replybox, c, r->reply_mask, r->deq_pos);
                    r->deq_pos++;
                    return BIFROST_OK;
                }
                stash_push(r, rep);     /* Unrelated: keep for poll_reply() */
            }
            ring_release(r->replybox, c, r->reply_mask, r->deq_pos);
            r->deq_pos++;
        }
        
        uint64_t elapsed = bifrost_get_timestamp() - start;
        if (!forever && elapsed >= timeout_ns) return fail(BIFROST_ERR_TIMEOUT);
        
        uint64_t slice = forever ? BIFROST_CALL_SLICE_NS : timeout_ns - elapsed;
        if (slice > BIFROST_CALL_SLICE_NS) slice = BIFROST_CALL_SLICE_NS;
        if (ring_wait(r->replybox, r->reply_mask, r->rep_cell_size, r->deq_pos, slice) == BIFROST_OK) {
            continue;
        }
        
        /* A whole slice without a reply: writer crashed (kill -9) or left?
         * Fail fast instead of waiting out the full timeout. */
        if (!bifrost_reader_writer_alive(r, wr) &&
            !ring_peek(r->replybox, r->reply_mask, r->rep_cell_size, r->deq_pos)) {
            return fail(BIFROST_ERR_NOTFOUND);
        }
    }
}
