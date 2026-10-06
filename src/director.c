/**
 * Bifrost - Director (Pool Creator/Manager)
 * 
 * Functions for creating and managing shared memory pools.
 */

#define _GNU_SOURCE
#include "bifrost_internal.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>

/* ============================================================================
 * Configuration
 * ============================================================================ */

void bifrost_config_init(bifrost_pool_config_t* config) {
    if (!config) return;
    
    config->initial_size = BIFROST_DEFAULT_INITIAL_SIZE;
    config->max_size = BIFROST_DEFAULT_MAX_SIZE;
    config->max_participants = BIFROST_DEFAULT_MAX_PARTICIPANTS;
    config->max_slots_per_writer = BIFROST_DEFAULT_MAX_SLOTS;
    config->auto_expand = true;
    
    config->cmd_inbox_depth = BIFROST_DEFAULT_CMD_INBOX_DEPTH;
    config->cmd_reply_depth = BIFROST_DEFAULT_CMD_REPLY_DEPTH;
    config->cmd_max_args = BIFROST_DEFAULT_CMD_MAX_ARGS;
    config->cmd_max_reply = BIFROST_DEFAULT_CMD_MAX_REPLY;
    config->cmd_max_per_writer = BIFROST_DEFAULT_CMD_MAX_PER_WRITER;
}

/* ============================================================================
 * Pool Creation
 * ============================================================================ */

/**
 * Computed shared-memory layout. Single source of truth used both for sizing
 * and for initialization.
 *
 * [header][participants][slots][catalogs][inboxes][reply boxes] | [data...]
 */
typedef struct {
    size_t participants_offset;
    size_t slots_offset;
    size_t data_start;
    size_t cmd_region_size;
    
    uint32_t inbox_depth, reply_depth;
    uint32_t max_args, max_reply, max_per_writer;
    uint32_t req_cell, rep_cell;
    size_t catalogs_offset, catalog_stride;
    size_t inboxes_offset, inbox_stride;
    size_t replyboxes_offset, replybox_stride;
} bifrost_layout_t;

static uint32_t round_pow2(uint32_t v) {
    if (v <= 1) return 1;
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
    return v + 1;
}

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void compute_layout(const bifrost_pool_config_t* config, bifrost_layout_t* L) {
    memset(L, 0, sizeof(*L));
    size_t n = config->max_participants;
    size_t off = BIFROST_ALIGN_UP(sizeof(bifrost_pool_header_t));
    
    L->participants_offset = off;
    off += BIFROST_ALIGN_UP(sizeof(bifrost_participant_t) * n);
    
    L->slots_offset = off;
    off += BIFROST_ALIGN_UP(sizeof(bifrost_slot_t) * n);
    
    size_t cmd_start = off;
    if (config->cmd_inbox_depth > 0) {
        L->inbox_depth    = round_pow2(clamp_u32(config->cmd_inbox_depth, 2, BIFROST_CMD_MAX_DEPTH));
        L->reply_depth    = round_pow2(clamp_u32(config->cmd_reply_depth ? config->cmd_reply_depth
                                                 : BIFROST_DEFAULT_CMD_REPLY_DEPTH, 2, BIFROST_CMD_MAX_DEPTH));
        L->max_args       = clamp_u32(config->cmd_max_args, 0, BIFROST_CMD_MAX_PAYLOAD);
        L->max_reply      = clamp_u32(config->cmd_max_reply, 0, BIFROST_CMD_MAX_PAYLOAD);
        L->max_per_writer = clamp_u32(config->cmd_max_per_writer ? config->cmd_max_per_writer
                                      : BIFROST_DEFAULT_CMD_MAX_PER_WRITER, 1, BIFROST_CMD_MAX_CATALOG);
        
        L->req_cell = (uint32_t)BIFROST_ALIGN_UP(8 + sizeof(bifrost_cmd_request_t) + L->max_args);
        L->rep_cell = (uint32_t)BIFROST_ALIGN_UP(8 + sizeof(bifrost_cmd_reply_t) + L->max_reply);
        
        L->catalog_stride  = BIFROST_ALIGN_UP(sizeof(bifrost_cmd_catalog_t) +
                                         sizeof(bifrost_cmd_desc_t) * L->max_per_writer);
        L->inbox_stride    = sizeof(bifrost_ring_t) + (size_t)L->req_cell * L->inbox_depth;
        L->replybox_stride = sizeof(bifrost_ring_t) + (size_t)L->rep_cell * L->reply_depth;
        
        L->catalogs_offset = off;
        off += L->catalog_stride * n;
        L->inboxes_offset = off;
        off += L->inbox_stride * n;
        L->replyboxes_offset = off;
        off += L->replybox_stride * n;
    }
    L->cmd_region_size = off - cmd_start;
    L->data_start = off;
}

/**
 * Initialize pool header and internal structures.
 */
static void initialize_pool_header(bifrost_pool_t* pool, const bifrost_pool_config_t* config,
                                    const bifrost_layout_t* L, size_t total_size) {
    bifrost_pool_header_t* header = pool->header;
    
    /* Magic and version */
    memcpy(header->magic, BIFROST_MAGIC, BIFROST_MAGIC_SIZE);
    bifrost_atomic_store(&header->version, BIFROST_VERSION);
    bifrost_atomic_store(&header->flags, 0);
    
    /* Size info */
    bifrost_atomic_store(&header->total_size, total_size);
    
    /* Offsets */
    header->participants_offset = L->participants_offset;
    header->slots_offset = L->slots_offset;
    
    size_t data_start = L->data_start;
    bifrost_atomic_store(&header->data_start, data_start);
    bifrost_atomic_store(&header->data_end, data_start);
    bifrost_atomic_store(&header->header_size, data_start);
    
    /* Participant counts */
    bifrost_atomic_store(&header->max_participants, config->max_participants);
    bifrost_atomic_store(&header->writer_count, 0);
    bifrost_atomic_store(&header->reader_count, 0);
    bifrost_atomic_store(&header->next_writer_id, 1);  /* Start from 1 */
    bifrost_atomic_store(&header->next_reader_id, 1);
    
    /* Statistics */
    bifrost_atomic_store(&header->total_writes, 0);
    bifrost_atomic_store(&header->total_reads, 0);
    bifrost_atomic_store(&header->create_time, bifrost_get_timestamp());
    
    /* Configuration (not atomic - set once). The command region is not
     * counted against max_size so data capacity matches the config. */
    header->max_size = config->max_size + L->cmd_region_size;
    header->max_slots_per_writer = config->max_slots_per_writer;
    header->auto_expand = config->auto_expand ? 1 : 0;
    
    /* Command layer */
    header->cmd_inbox_depth    = L->inbox_depth;
    header->cmd_reply_depth    = L->reply_depth;
    header->cmd_max_args       = L->max_args;
    header->cmd_max_reply      = L->max_reply;
    header->cmd_max_per_writer = L->max_per_writer;
    header->cmd_req_cell_size  = L->req_cell;
    header->cmd_rep_cell_size  = L->rep_cell;
    header->catalogs_offset    = L->catalogs_offset;
    header->catalog_stride     = L->catalog_stride;
    header->inboxes_offset     = L->inboxes_offset;
    header->inbox_stride       = L->inbox_stride;
    header->replyboxes_offset  = L->replyboxes_offset;
    header->replybox_stride    = L->replybox_stride;
    
    /* Set pointers in pool handle */
    pool->participants = (bifrost_participant_t*)((char*)pool->base + header->participants_offset);
    pool->slots = (bifrost_slot_t*)((char*)pool->base + header->slots_offset);
    pool->data_area = (char*)pool->base + data_start;
    
    /* Initialize participant array */
    for (uint32_t i = 0; i < config->max_participants; i++) {
        bifrost_participant_t* p = &pool->participants[i];
        bifrost_atomic_store(&p->active, BIFROST_P_FREE);
        bifrost_atomic_store(&p->type, BIFROST_PARTICIPANT_NONE);
        bifrost_atomic_store(&p->pid, 0);
        memset(p->name, 0, BIFROST_NAME_MAX);
        bifrost_atomic_store(&p->slot_offset, 0);
        bifrost_atomic_store(&p->join_time, 0);
        bifrost_atomic_store(&p->generation, 0);
        bifrost_atomic_store(&p->uid, 0);
    }
    
    /* Initialize slot array */
    for (uint32_t i = 0; i < config->max_participants; i++) {
        bifrost_slot_t* s = &pool->slots[i];
        bifrost_atomic_store(&s->state, BIFROST_SLOT_FREE);
        bifrost_atomic_store(&s->command, 0);
        bifrost_atomic_store(&s->version, 0);
        bifrost_atomic_store(&s->data_size, 0);
        bifrost_atomic_store(&s->data_offset, 0);
        bifrost_atomic_store(&s->timestamp, 0);
    }
    
    /* Initialize command rings and catalogs */
    bifrost_cmd_init_region(pool);
}

bifrost_pool_t* bifrost_create_pool(const char* name, const bifrost_pool_config_t* config) {
    /* Validate name */
    if (!bifrost_validate_name(name)) {
        bifrost_set_error(BIFROST_ERR_INVALID);
        return NULL;
    }
    
    /* Use default config if not provided */
    bifrost_pool_config_t default_config;
    if (!config) {
        bifrost_config_init(&default_config);
        config = &default_config;
    }
    
    /* Validate participant count */
    if (config->max_participants == 0) {
        bifrost_set_error(BIFROST_ERR_INVALID);
        return NULL;
    }
    
    /* Calculate sizes: data capacity = initial_size, command region on top */
    bifrost_layout_t layout;
    compute_layout(config, &layout);
    size_t header_size = layout.data_start - layout.cmd_region_size;
    size_t total_size = config->initial_size;
    if (total_size < header_size + 4096) {
        total_size = header_size + 4096;  /* At least 4K for data */
    }
    total_size += layout.cmd_region_size;
    total_size = BIFROST_ALIGN_UP(total_size);
    
    /* Allocate pool handle */
    bifrost_pool_t* pool = calloc(1, sizeof(bifrost_pool_t));
    if (!pool) {
        bifrost_set_error(BIFROST_ERR_NOMEM);
        return NULL;
    }
    
    /* Store names */
    strncpy(pool->name, name, BIFROST_SHM_NAME_MAX - 1);
    bifrost_make_shm_name(name, pool->shm_name, BIFROST_SHM_NAME_MAX);
    
    /* Create shared memory */
    pool->shm_fd = bifrost_shm_create(pool->shm_name, total_size);
    if (pool->shm_fd < 0) {
        free(pool);
        return NULL;
    }
    
    /* Map shared memory */
    pool->base = bifrost_shm_map(pool->shm_fd, total_size);
    if (!pool->base) {
        close(pool->shm_fd);
        shm_unlink(pool->shm_name);
        free(pool);
        return NULL;
    }
    
    pool->mapped_size = total_size;
    pool->is_creator = true;
    pool->header = (bifrost_pool_header_t*)pool->base;
    
    /* Initialize the pool */
    initialize_pool_header(pool, config, &layout, total_size);
    
    /* Memory barrier to ensure all writes are visible */
    bifrost_memory_barrier();
    
    return pool;
}

/* ============================================================================
 * Pool Opening (for participants)
 * ============================================================================ */

bifrost_pool_t* bifrost_open_pool(const char* name) {
    if (!bifrost_validate_name(name)) {
        bifrost_set_error(BIFROST_ERR_INVALID);
        return NULL;
    }
    
    /* Allocate pool handle */
    bifrost_pool_t* pool = calloc(1, sizeof(bifrost_pool_t));
    if (!pool) {
        bifrost_set_error(BIFROST_ERR_NOMEM);
        return NULL;
    }
    
    strncpy(pool->name, name, BIFROST_SHM_NAME_MAX - 1);
    bifrost_make_shm_name(name, pool->shm_name, BIFROST_SHM_NAME_MAX);
    
    /* Open shared memory */
    pool->shm_fd = bifrost_shm_open(pool->shm_name);
    if (pool->shm_fd < 0) {
        free(pool);
        return NULL;
    }
    
    /* Get current size from stat */
    struct stat st;
    if (fstat(pool->shm_fd, &st) < 0) {
        close(pool->shm_fd);
        free(pool);
        bifrost_set_error(BIFROST_ERR_SYSTEM);
        return NULL;
    }
    
    size_t size = (size_t)st.st_size;
    
    /* Map shared memory */
    pool->base = bifrost_shm_map(pool->shm_fd, size);
    if (!pool->base) {
        close(pool->shm_fd);
        free(pool);
        return NULL;
    }
    
    pool->mapped_size = size;
    pool->is_creator = false;
    pool->header = (bifrost_pool_header_t*)pool->base;
    
    /* Validate magic */
    if (memcmp(pool->header->magic, BIFROST_MAGIC, BIFROST_MAGIC_SIZE) != 0) {
        bifrost_shm_unmap(pool->base, size);
        close(pool->shm_fd);
        free(pool);
        bifrost_set_error(BIFROST_ERR_CORRUPTED);
        return NULL;
    }
    
    /* Check version */
    if (bifrost_atomic_load(&pool->header->version) != BIFROST_VERSION) {
        bifrost_shm_unmap(pool->base, size);
        close(pool->shm_fd);
        free(pool);
        bifrost_set_error(BIFROST_ERR_VERSION);
        return NULL;
    }
    
    /* Set up pointers */
    pool->participants = (bifrost_participant_t*)((char*)pool->base + 
                                              pool->header->participants_offset);
    pool->slots = (bifrost_slot_t*)((char*)pool->base + pool->header->slots_offset);
    pool->data_area = (char*)pool->base + bifrost_atomic_load(&pool->header->data_start);
    
    return pool;
}

/* ============================================================================
 * Pool Destruction
 * ============================================================================ */

void bifrost_destroy_pool(bifrost_pool_t* pool) {
    if (!pool) return;
    
    /* Only creator can destroy */
    if (pool->is_creator) {
        /* Unlink shared memory (remove from filesystem) */
        shm_unlink(pool->shm_name);
    }
    
    /* Unmap and close */
    if (pool->base) {
        bifrost_shm_unmap(pool->base, pool->mapped_size);
    }
    
    if (pool->shm_fd >= 0) {
        close(pool->shm_fd);
    }
    
    free(pool);
}

void bifrost_close_pool(bifrost_pool_t* pool) {
    if (!pool) return;
    
    /* Just unmap and close, don't unlink */
    if (pool->base) {
        bifrost_shm_unmap(pool->base, pool->mapped_size);
    }
    
    if (pool->shm_fd >= 0) {
        close(pool->shm_fd);
    }
    
    free(pool);
}

/* ============================================================================
 * Pool Statistics
 * ============================================================================ */

bifrost_error_t bifrost_get_pool_stats(bifrost_pool_t* pool, bifrost_pool_stats_t* stats) {
    if (!pool || !stats) {
        bifrost_set_error(BIFROST_ERR_INVALID);
        return BIFROST_ERR_INVALID;
    }
    
    bifrost_pool_header_t* header = pool->header;
    
    stats->total_size = bifrost_atomic_load(&header->total_size);
    size_t data_start = bifrost_atomic_load(&header->data_start);
    size_t data_end = bifrost_atomic_load(&header->data_end);
    
    stats->used_size = data_end - data_start;
    stats->free_size = stats->total_size - data_end;
    stats->writer_count = bifrost_atomic_load(&header->writer_count);
    stats->reader_count = bifrost_atomic_load(&header->reader_count);
    stats->total_writes = bifrost_atomic_load(&header->total_writes);
    stats->total_reads = bifrost_atomic_load(&header->total_reads);
    
    return BIFROST_OK;
}

/* ============================================================================
 * Pool Expansion
 * ============================================================================ */

bifrost_error_t bifrost_expand_pool(bifrost_pool_t* pool, size_t new_size) {
    if (!pool) {
        bifrost_set_error(BIFROST_ERR_INVALID);
        return BIFROST_ERR_INVALID;
    }
    
    bifrost_pool_header_t* header = pool->header;
    size_t current_size = bifrost_atomic_load(&header->total_size);
    
    if (new_size <= current_size) {
        return BIFROST_OK;  /* Already big enough */
    }
    
    if (new_size > header->max_size) {
        bifrost_set_error(BIFROST_ERR_INVALID);
        return BIFROST_ERR_INVALID;
    }
    
    new_size = BIFROST_ALIGN_UP(new_size);
    
    /* Resize using mremap */
    if (bifrost_shm_resize(pool->shm_fd, &pool->base, current_size, new_size) < 0) {
        return bifrost_get_last_error();
    }
    
    /* Update pointers (base may have changed) */
    pool->mapped_size = new_size;
    pool->header = (bifrost_pool_header_t*)pool->base;
    pool->participants = (bifrost_participant_t*)((char*)pool->base + 
                                              pool->header->participants_offset);
    pool->slots = (bifrost_slot_t*)((char*)pool->base + pool->header->slots_offset);
    pool->data_area = (char*)pool->base + bifrost_atomic_load(&pool->header->data_start);
    
    /* Update size in header */
    bifrost_atomic_store(&pool->header->total_size, new_size);
    
    bifrost_memory_barrier();
    
    return BIFROST_OK;
}
