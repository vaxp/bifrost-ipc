/**
 * Bifrost - Utility Functions
 */

#define _GNU_SOURCE
#include "bifrost_internal.h"
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <sched.h>

/* ============================================================================
 * Thread-Local Error Storage
 * ============================================================================ */

static _Thread_local bifrost_error_t tls_last_error = BIFROST_OK;

void bifrost_set_error(bifrost_error_t error) {
    tls_last_error = error;
}

bifrost_error_t bifrost_get_last_error(void) {
    return tls_last_error;
}

const char* bifrost_error_string(bifrost_error_t error) {
    switch (error) {
        case BIFROST_OK:             return "Success";
        case BIFROST_ERR_NOMEM:      return "Out of memory";
        case BIFROST_ERR_EXISTS:     return "Already exists";
        case BIFROST_ERR_NOTFOUND:   return "Not found";
        case BIFROST_ERR_FULL:       return "Pool is full";
        case BIFROST_ERR_INVALID:    return "Invalid argument";
        case BIFROST_ERR_PERMISSION: return "Permission denied";
        case BIFROST_ERR_SYSTEM:     return "System error";
        case BIFROST_ERR_CORRUPTED:  return "Data corrupted";
        case BIFROST_ERR_VERSION:    return "Version mismatch";
        case BIFROST_ERR_BUSY:       return "Resource busy";
        case BIFROST_ERR_TIMEOUT:    return "Timed out";
        case BIFROST_ERR_DISABLED:   return "Feature disabled in this pool";
        default:                return "Unknown error";
    }
}

/* ============================================================================
 * Time Utilities
 * ============================================================================ */

uint64_t bifrost_get_timestamp(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ============================================================================
 * Name Validation and Conversion
 * ============================================================================ */

bool bifrost_validate_name(const char* name) {
    if (!name || !*name) {
        return false;
    }
    
    size_t len = strlen(name);
    if (len >= BIFROST_NAME_MAX - 1) {
        return false;
    }
    
    /* Name should not start with / */
    if (name[0] == '/') {
        return false;
    }
    
    /* Check for valid characters (alphanumeric, underscore, dash) */
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '_' || c == '-')) {
            return false;
        }
    }
    
    return true;
}

void bifrost_make_shm_name(const char* name, char* out, size_t out_size) {
    snprintf(out, out_size, "%s%s", BIFROST_SHM_PREFIX, name);
}

/* ============================================================================
 * Shared Memory Operations
 * ============================================================================ */

int bifrost_shm_create(const char* name, size_t size) {
    /* Create shared memory object */
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (fd < 0) {
        if (errno == EEXIST) {
            bifrost_set_error(BIFROST_ERR_EXISTS);
        } else if (errno == EACCES) {
            bifrost_set_error(BIFROST_ERR_PERMISSION);
        } else {
            bifrost_set_error(BIFROST_ERR_SYSTEM);
        }
        return -1;
    }
    
    /* Set size */
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        shm_unlink(name);
        bifrost_set_error(BIFROST_ERR_SYSTEM);
        return -1;
    }
    
    return fd;
}

int bifrost_shm_open(const char* name) {
    int fd = shm_open(name, O_RDWR, 0);
    if (fd < 0) {
        if (errno == ENOENT) {
            bifrost_set_error(BIFROST_ERR_NOTFOUND);
        } else if (errno == EACCES) {
            bifrost_set_error(BIFROST_ERR_PERMISSION);
        } else {
            bifrost_set_error(BIFROST_ERR_SYSTEM);
        }
        return -1;
    }
    return fd;
}

void* bifrost_shm_map(int fd, size_t size) {
    void* ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        bifrost_set_error(BIFROST_ERR_SYSTEM);
        return NULL;
    }
    return ptr;
}

int bifrost_shm_unmap(void* addr, size_t size) {
    if (munmap(addr, size) < 0) {
        bifrost_set_error(BIFROST_ERR_SYSTEM);
        return -1;
    }
    return 0;
}

int bifrost_shm_resize(int fd, void** addr, size_t old_size, size_t new_size) {
    /* First extend the file */
    if (ftruncate(fd, (off_t)new_size) < 0) {
        bifrost_set_error(BIFROST_ERR_SYSTEM);
        return -1;
    }
    
    /* Try to remap in place using mremap */
    void* new_addr = mremap(*addr, old_size, new_size, MREMAP_MAYMOVE);
    if (new_addr == MAP_FAILED) {
        bifrost_set_error(BIFROST_ERR_SYSTEM);
        return -1;
    }
    
    *addr = new_addr;
    return 0;
}

int bifrost_shm_unlink(const char* name) {
    if (shm_unlink(name) < 0) {
        if (errno == ENOENT) {
            bifrost_set_error(BIFROST_ERR_NOTFOUND);
        } else {
            bifrost_set_error(BIFROST_ERR_SYSTEM);
        }
        return -1;
    }
    return 0;
}

/* ============================================================================
 * Pool Utilities
 * ============================================================================ */

bifrost_participant_t* bifrost_find_participant_by_name(bifrost_pool_t* pool, const char* name,
                                               bifrost_participant_type_t type) {
    uint32_t max = bifrost_atomic_load(&pool->header->max_participants);
    
    for (uint32_t i = 0; i < max; i++) {
        bifrost_participant_t* p = &pool->participants[i];
        
        if (bifrost_atomic_load(&p->active) == BIFROST_P_LIVE &&
            bifrost_atomic_load(&p->type) == (uint32_t)type &&
            strcmp(p->name, name) == 0) {
            return p;
        }
    }
    
    return NULL;
}

bifrost_participant_t* bifrost_find_free_participant(bifrost_pool_t* pool) {
    uint32_t max = bifrost_atomic_load(&pool->header->max_participants);
    
    for (uint32_t i = 0; i < max; i++) {
        bifrost_participant_t* p = &pool->participants[i];
        uint32_t expected = BIFROST_P_FREE;
        
        /* Try to claim this slot atomically */
        if (bifrost_atomic_compare_exchange(&p->active, &expected, BIFROST_P_CLAIMED)) {
            return p;
        }
    }
    
    return NULL;
}

bifrost_slot_t* bifrost_get_writer_slot(bifrost_pool_t* pool, uint32_t participant_idx) {
    if (participant_idx >= bifrost_atomic_load(&pool->header->max_participants)) {
        return NULL;
    }
    return &pool->slots[participant_idx];
}

/* ============================================================================
 * Atomic Join Protocol
 *
 *   FREE --CAS--> CLAIMED --name written--> NAMED --unique--> LIVE
 *
 * Uniqueness: after publishing our name (NAMED, seq_cst) we scan every other
 * participant of the same type and name:
 *   - LIVE                 -> name taken, abort.
 *   - NAMED, lower index   -> it wins, abort.
 *   - NAMED, higher index  -> wait until it resolves (it will either see us
 *                             and abort, or go LIVE and we abort).
 * We only ever wait on higher indices, so there is no wait cycle. Dead
 * processes stuck in NAMED are detected with kill(pid, 0).
 * ============================================================================ */

static bool pid_alive(pid_t pid) {
    if (pid <= 0) return false;
    return kill(pid, 0) == 0 || errno == EPERM;
}

void bifrost_participant_abort(bifrost_participant_t* p) {
    memset(p->name, 0, BIFROST_NAME_MAX);
    bifrost_atomic_store(&p->type, BIFROST_PARTICIPANT_NONE);
    bifrost_atomic_store(&p->pid, 0);
    bifrost_atomic_store(&p->active, BIFROST_P_FREE);
}

void bifrost_participant_release(bifrost_participant_t* p) {
    /* Hide first so scanners stop matching, then clear, then free (last). */
    bifrost_atomic_store(&p->active, BIFROST_P_CLAIMED);
    bifrost_participant_abort(p);
}

void bifrost_participant_publish(bifrost_participant_t* p) {
    bifrost_memory_barrier();
    bifrost_atomic_store(&p->active, BIFROST_P_LIVE);
}

bifrost_participant_t* bifrost_participant_join(bifrost_pool_t* pool, const char* name,
                                      bifrost_participant_type_t type) {
    /* Fast reject */
    if (bifrost_find_participant_by_name(pool, name, type)) {
        bifrost_set_error(BIFROST_ERR_EXISTS);
        return NULL;
    }
    
    bifrost_participant_t* p = bifrost_find_free_participant(pool);
    if (!p) {
        bifrost_set_error(BIFROST_ERR_FULL);
        return NULL;
    }
    
    uint32_t self = (uint32_t)(p - pool->participants);
    
    bifrost_atomic_store(&p->type, (uint32_t)type);
    bifrost_atomic_store(&p->pid, getpid());
    bifrost_atomic_store(&p->uid, (uint32_t)getuid());
    strncpy(p->name, name, BIFROST_NAME_MAX - 1);
    p->name[BIFROST_NAME_MAX - 1] = '\0';
    atomic_store_explicit(&p->active, BIFROST_P_NAMED, memory_order_seq_cst);
    
    uint32_t max = bifrost_atomic_load(&pool->header->max_participants);
    for (uint32_t i = 0; i < max; i++) {
        if (i == self) continue;
        bifrost_participant_t* q = &pool->participants[i];
        
        uint32_t spins = 0;
        for (;;) {
            uint32_t st = atomic_load_explicit(&q->active, memory_order_seq_cst);
            if (st < BIFROST_P_NAMED ||
                bifrost_atomic_load(&q->type) != (uint32_t)type ||
                strncmp(q->name, name, BIFROST_NAME_MAX) != 0) {
                break;  /* No conflict */
            }
            if (st == BIFROST_P_LIVE || i < self) {
                if (st == BIFROST_P_NAMED && !pid_alive(bifrost_atomic_load(&q->pid))) {
                    break;  /* Stale entry from a dead process */
                }
                bifrost_participant_abort(p);
                bifrost_set_error(BIFROST_ERR_EXISTS);
                return NULL;
            }
            /* Higher index still verifying: wait for it to resolve */
            if ((++spins & 1023u) == 0 && !pid_alive(bifrost_atomic_load(&q->pid))) {
                break;  /* Stale entry from a dead process */
            }
            if (spins > (1u << 22)) {
                bifrost_participant_abort(p);
                bifrost_set_error(BIFROST_ERR_BUSY);
                return NULL;
            }
            sched_yield();
        }
    }
    
    /* New incarnation of this slot (never 0) */
    uint32_t gen = bifrost_atomic_fetch_add(&p->generation, 1) + 1;
    if (gen == 0) {
        bifrost_atomic_fetch_add(&p->generation, 1);
    }
    
    return p;   /* Still NAMED: caller finishes setup then calls publish() */
}

/* ============================================================================
 * Misc
 * ============================================================================ */

bool bifrost_pool_exists(const char* name) {
    char shm_name[BIFROST_SHM_NAME_MAX];
    bifrost_make_shm_name(name, shm_name, sizeof(shm_name));
    
    int fd = shm_open(shm_name, O_RDONLY, 0);
    if (fd >= 0) {
        close(fd);
        return true;
    }
    return false;
}

const char* bifrost_version(void) {
    return "2.0.0";
}
