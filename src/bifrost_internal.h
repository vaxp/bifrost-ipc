/**
 * Bifrost Internal Definitions
 * 
 * Internal data structures and constants.
 * Not part of the public API.
 */

#ifndef BIFROST_INTERNAL_H
#define BIFROST_INTERNAL_H

#include "bifrost.h"
#include <stdatomic.h>
#include <sys/types.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

#define BIFROST_MAGIC            "BIFROSTHYD"
#define BIFROST_MAGIC_SIZE       8
#define BIFROST_VERSION          2
#define BIFROST_NAME_MAX         64
#define BIFROST_SHM_PREFIX       "/bifrost_"
#define BIFROST_SHM_NAME_MAX     64

/* Default configuration values */
#define BIFROST_DEFAULT_INITIAL_SIZE     (1024 * 1024)       /* 1 MB */
#define BIFROST_DEFAULT_MAX_SIZE         (256 * 1024 * 1024) /* 256 MB */
#define BIFROST_DEFAULT_MAX_PARTICIPANTS 64
#define BIFROST_DEFAULT_MAX_SLOTS        16

/* Command layer defaults */
#define BIFROST_DEFAULT_CMD_INBOX_DEPTH  64
#define BIFROST_DEFAULT_CMD_REPLY_DEPTH  32
#define BIFROST_DEFAULT_CMD_MAX_ARGS     192
#define BIFROST_DEFAULT_CMD_MAX_REPLY    192
#define BIFROST_DEFAULT_CMD_MAX_PER_WRITER 32

/* Command layer hard limits */
#define BIFROST_CMD_MAX_DEPTH            65536
#define BIFROST_CMD_MAX_PAYLOAD          (1024 * 1024)   /* 1 MB inline per cell */
#define BIFROST_CMD_MAX_CATALOG          1024

/* Participant lifecycle (bifrost_participant_t.active) */
#define BIFROST_P_FREE       0u   /* Unused */
#define BIFROST_P_CLAIMED    1u   /* Reserved, name not yet visible */
#define BIFROST_P_NAMED      2u   /* Name visible, uniqueness being verified */
#define BIFROST_P_LIVE       3u   /* Fully joined */

/* Memory alignment */
#define BIFROST_ALIGN            64      /* Cache line alignment */
#define BIFROST_ALIGN_UP(x)      (((x) + BIFROST_ALIGN - 1) & ~(BIFROST_ALIGN - 1))

/* ============================================================================
 * Atomic Types (Lock-Free)
 * ============================================================================ */

/* Use C11 atomics with explicit memory ordering */
#define bifrost_atomic_load(ptr) \
    atomic_load_explicit((ptr), memory_order_acquire)

#define bifrost_atomic_store(ptr, val) \
    atomic_store_explicit((ptr), (val), memory_order_release)

#define bifrost_atomic_fetch_add(ptr, val) \
    atomic_fetch_add_explicit((ptr), (val), memory_order_acq_rel)

#define bifrost_atomic_compare_exchange(ptr, expected, desired) \
    atomic_compare_exchange_strong_explicit((ptr), (expected), (desired), \
        memory_order_acq_rel, memory_order_acquire)

#define bifrost_memory_barrier() \
    atomic_thread_fence(memory_order_seq_cst)

/* ============================================================================
 * Internal Structures
 * ============================================================================ */

/**
 * Data slot - holds one piece of data from a writer.
 * 
 * Layout in shared memory:
 * [bifrost_slot_t header][data bytes...]
 */
typedef struct {
    _Atomic uint32_t state;         /* bifrost_slot_state_t */
    _Atomic uint32_t command;       /* User-defined command */
    _Atomic uint64_t version;       /* Increments on each publish */
    _Atomic size_t   data_size;     /* Size of data */
    _Atomic uint64_t data_offset;   /* Offset from pool base to data */
    _Atomic uint64_t timestamp;     /* Last update timestamp (optional) */
    uint8_t          reserved[24];  /* Padding to cache line */
} bifrost_slot_t;

_Static_assert(sizeof(bifrost_slot_t) == 64, "bifrost_slot_t must be cache-line sized");

/**
 * Participant record - writer or reader registration.
 */
typedef struct {
    _Atomic uint32_t active;        /* BIFROST_P_* lifecycle state */
    _Atomic uint32_t type;          /* bifrost_participant_type_t */
    _Atomic pid_t    pid;           /* Process ID */
    char             name[BIFROST_NAME_MAX]; /* Participant name */
    _Atomic uint64_t slot_offset;   /* Offset to bifrost_slot_t (for writers) */
    _Atomic uint64_t join_time;     /* When participant joined */
    _Atomic uint32_t generation;    /* Bumped on every join (ABA guard) */
    _Atomic uint32_t uid;           /* User ID of the process */
    uint8_t          reserved[8];   /* Padding */
} bifrost_participant_t;

_Static_assert(sizeof(bifrost_participant_t) % 8 == 0, "bifrost_participant_t must be 8-byte aligned");

/* ============================================================================
 * Command Layer Structures (shared memory)
 * ============================================================================ */

_Static_assert(sizeof(bifrost_cmd_desc_t) == 128, "bifrost_cmd_desc_t must be 128 bytes");
_Static_assert(sizeof(bifrost_cmd_request_t) == 40, "bifrost_cmd_request_t layout");
_Static_assert(sizeof(bifrost_cmd_reply_t) == 40, "bifrost_cmd_reply_t layout");

/**
 * Per-writer catalog, protected by a seqlock (single writer, many readers).
 * [bifrost_cmd_catalog_t][bifrost_cmd_desc_t x cmd_max_per_writer]
 */
typedef struct {
    _Atomic uint32_t seq;           /* Odd while being modified */
    _Atomic uint32_t count;         /* Valid entries */
    uint8_t          pad[56];
} bifrost_cmd_catalog_t;

_Static_assert(sizeof(bifrost_cmd_catalog_t) == 64, "catalog header must be one cache line");

/**
 * Ring cell header: [seq][payload struct][inline bytes...]
 * seq follows Dmitry Vyukov's bounded MPMC queue protocol.
 */
typedef struct {
    _Atomic uint64_t seq;
} bifrost_ring_cell_t;

#define BIFROST_CELL_PAYLOAD(cell)   ((void*)((char*)(cell) + 8))
#define BIFROST_CELL_FROM_PAYLOAD(p) ((bifrost_ring_cell_t*)((char*)(p) - 8))

/**
 * Bounded MPSC ring header (used for writer inboxes and reader reply boxes).
 * Producers and the consumer touch separate cache lines.
 * [bifrost_ring_t (128 B)][cell x depth]
 */
typedef struct {
    /* Cache line 0: producers (+ wake-up handshake, read by producers on send) */
    _Atomic uint64_t enqueue_pos;
    _Atomic uint32_t waiters;       /* Consumer is (about to be) asleep */
    _Atomic uint32_t futex_word;    /* Bumped by producers to wake the consumer */
    uint8_t          pad0[48];
    /* Cache line 1: consumer only */
    _Atomic uint64_t dequeue_pos;   /* Persisted by consumer (survives re-join) */
    _Atomic uint64_t dropped;       /* Replies dropped by this participant (writer side) */
    uint8_t          pad1[48];
} bifrost_ring_t;

_Static_assert(sizeof(bifrost_ring_t) == 128, "bifrost_ring_t must be two cache lines");

/**
 * Pool header - at the start of shared memory.
 * 
 * Memory layout:
 * [bifrost_pool_header_t]
 * [bifrost_participant_t array]
 * [bifrost_slot_t array] (one per max writer)
 * [data area - bump allocated]
 */
typedef struct {
    /* Magic and version */
    char             magic[BIFROST_MAGIC_SIZE];  /* "BIFROSTHYD" */
    _Atomic uint32_t version;               /* Structure version */
    _Atomic uint32_t flags;                 /* Pool flags */
    
    /* Size management */
    _Atomic size_t   total_size;            /* Total mapped size */
    _Atomic size_t   header_size;           /* Size of header + tables */
    _Atomic size_t   data_start;            /* Offset where data area starts */
    _Atomic size_t   data_end;              /* Current end of used data */
    
    /* Participant management */
    _Atomic uint32_t max_participants;      /* Max slots for participants */
    _Atomic uint32_t writer_count;          /* Active writer count */
    _Atomic uint32_t reader_count;          /* Active reader count */
    _Atomic uint32_t next_writer_id;        /* Next writer ID to assign */
    _Atomic uint32_t next_reader_id;        /* Next reader ID to assign */
    
    /* Statistics */
    _Atomic uint64_t total_writes;          /* Total publish count */
    _Atomic uint64_t total_reads;           /* Total read count */
    _Atomic uint64_t create_time;           /* Pool creation time */
    
    /* Configuration */
    size_t           max_size;              /* Maximum allowed size */
    uint32_t         max_slots_per_writer;  /* Max slots per writer */
    uint32_t         auto_expand;           /* Auto-expand flag */
    
    /* Offsets to arrays (from start of header) */
    uint64_t         participants_offset;   /* Offset to participant array */
    uint64_t         slots_offset;          /* Offset to slot array */
    
    /* Command layer (all zero when disabled) */
    uint32_t         cmd_inbox_depth;       /* Power of 2, 0 = disabled */
    uint32_t         cmd_reply_depth;       /* Power of 2 */
    uint32_t         cmd_max_args;
    uint32_t         cmd_max_reply;
    uint32_t         cmd_max_per_writer;
    uint32_t         cmd_req_cell_size;     /* Bytes per request cell */
    uint32_t         cmd_rep_cell_size;     /* Bytes per reply cell */
    uint32_t         cmd_pad;
    uint64_t         catalogs_offset;       /* [catalog x max_participants] */
    uint64_t         catalog_stride;
    uint64_t         inboxes_offset;        /* [inbox ring x max_participants] */
    uint64_t         inbox_stride;
    uint64_t         replyboxes_offset;     /* [reply ring x max_participants] */
    uint64_t         replybox_stride;
    
    /* Reserved for future use */
    uint8_t          reserved[64];
} bifrost_pool_header_t;

/**
 * Local pool handle (not in shared memory).
 */
struct bifrost_pool {
    char        name[BIFROST_SHM_NAME_MAX];  /* Pool name */
    char        shm_name[BIFROST_SHM_NAME_MAX]; /* Full shm name with prefix */
    int         shm_fd;                 /* Shared memory file descriptor */
    void*       base;                   /* Base address of mapping */
    size_t      mapped_size;            /* Current mapped size */
    bool        is_creator;             /* Did we create this pool */
    
    /* Quick access pointers (derived from base) */
    bifrost_pool_header_t* header;           /* Pool header */
    bifrost_participant_t* participants;     /* Participant array */
    bifrost_slot_t*        slots;            /* Slot array */
    void*             data_area;        /* Start of data area */
};

/**
 * Local writer handle.
 */
struct bifrost_writer {
    bifrost_pool_t*        pool;             /* Pool we belong to */
    uint32_t          id;               /* Our writer ID (participant_idx + 1) */
    uint32_t          participant_idx;  /* Index in participant array */
    bifrost_slot_t*        slot;             /* Our data slot */
    void*             current_alloc;    /* Current allocation pointer */
    size_t            current_size;     /* Current allocation size */
    char              name[BIFROST_NAME_MAX];
    uint32_t          gen;              /* Our generation */
    
    /* Command layer (inbox consumer side) */
    bifrost_ring_t*        inbox;            /* NULL when commands disabled */
    char*             inbox_cells;
    uint32_t          inbox_mask;
    uint32_t          req_cell_size;
    uint64_t          deq_pos;          /* Local copy of dequeue position */
    bifrost_ring_cell_t*   inflight;         /* Request handed to the app */
    uint32_t          inflight_idx;     /* Catalog index of the in-flight command */
    bool              inflight_noreply; /* Reply suppressed for the in-flight request */
    uint32_t          uid;              /* Our user ID (BIFROST_CMD_F_SAME_UID) */
    
    /* Catalog (shared copy + local mirror for O(1)-ish validation) */
    bifrost_cmd_catalog_t* catalog;
    bifrost_cmd_desc_t*    cat_shared;       /* Entries in shared memory */
    bifrost_cmd_desc_t*    cat_local;        /* Local mirror */
    uint32_t          cat_count;
    uint32_t          cat_cap;
    
    /* Dispatch / access control (process-local, never in SHM) */
    bifrost_cmd_handler_fn* handlers;        /* Parallel to cat_local */
    void**            handler_ctx;
    bifrost_cmd_filter_fn  filter;
    void*             filter_ctx;
    void*             reply_scratch;    /* cmd_max_reply bytes for dispatch */
};

/**
 * Local reader handle.
 */
struct bifrost_reader {
    bifrost_pool_t*        pool;             /* Pool we belong to */
    uint32_t          id;               /* Our reader ID */
    uint32_t          participant_idx;  /* Index in participant array */
    char              name[BIFROST_NAME_MAX];
    uint32_t          gen;              /* Our generation */
    uint32_t          uid;
    
    /* Command layer (reply box consumer side) */
    bifrost_ring_t*        replybox;         /* NULL when commands disabled */
    char*             reply_cells;
    uint32_t          reply_mask;
    uint32_t          rep_cell_size;
    uint64_t          deq_pos;
    bifrost_ring_cell_t*   inflight;         /* Reply handed to the app (SHM) */
    uint64_t          next_ticket;
    
    /* Flow control: replies we are owed, per writer index. Sends that expect
     * a reply are refused once pending_total + stash_count reaches the reply
     * box depth, so a writer never has to drop a reply for lack of space. */
    uint32_t*         pending;          /* [max_participants] */
    uint32_t          pending_total;
    
    /* Local stash for replies set aside by bifrost_reader_call() */
    char*             stash;            /* reply_depth * rep_cell_size */
    uint32_t          stash_head;
    uint32_t          stash_count;
    bool              stash_inflight;
};

/* ============================================================================
 * Internal Functions
 * ============================================================================ */

/* Thread-local error storage */
void bifrost_set_error(bifrost_error_t error);

/* Time utilities */
uint64_t bifrost_get_timestamp(void);

/* Shared memory utilities */
int bifrost_shm_create(const char* name, size_t size);
int bifrost_shm_open(const char* name);
void* bifrost_shm_map(int fd, size_t size);
int bifrost_shm_unmap(void* addr, size_t size);
int bifrost_shm_resize(int fd, void** addr, size_t old_size, size_t new_size);
int bifrost_shm_unlink(const char* name);

/* Pool internal functions */
bifrost_participant_t* bifrost_find_participant_by_name(bifrost_pool_t* pool, const char* name, 
                                               bifrost_participant_type_t type);
bifrost_participant_t* bifrost_find_free_participant(bifrost_pool_t* pool);
bifrost_slot_t* bifrost_get_writer_slot(bifrost_pool_t* pool, uint32_t participant_idx);

/* Atomic join/leave protocol (utils.c) */
bifrost_participant_t* bifrost_participant_join(bifrost_pool_t* pool, const char* name,
                                      bifrost_participant_type_t type);
void bifrost_participant_publish(bifrost_participant_t* p);
void bifrost_participant_abort(bifrost_participant_t* p);
void bifrost_participant_release(bifrost_participant_t* p);

/* Command layer (command.c) */
void bifrost_cmd_init_region(bifrost_pool_t* pool);
int  bifrost_cmd_writer_attach(bifrost_writer_t* writer);
void bifrost_cmd_writer_detach(bifrost_writer_t* writer);
int  bifrost_cmd_reader_attach(bifrost_reader_t* reader);
void bifrost_cmd_reader_detach(bifrost_reader_t* reader);

static inline bool bifrost_cmd_enabled(const bifrost_pool_t* pool) {
    return pool->header->cmd_inbox_depth != 0;
}

/* Arena allocator */
void* bifrost_arena_alloc(bifrost_pool_t* pool, size_t size);
size_t bifrost_arena_available(bifrost_pool_t* pool);

/* Name validation */
bool bifrost_validate_name(const char* name);
void bifrost_make_shm_name(const char* name, char* out, size_t out_size);

#endif /* BIFROST_INTERNAL_H */
