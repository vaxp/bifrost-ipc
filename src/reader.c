/**
 * Bifrost - Reader Implementation
 * 
 * Lock-free reader interface for desktop environment components.
 */

#define _GNU_SOURCE
#include "bifrost_internal.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ============================================================================
 * Reader Join/Leave
 * ============================================================================ */

bifrost_reader_t* bifrost_join_as_reader(const char* pool_name, const char* reader_name) {
    /* Validate inputs */
    if (!bifrost_validate_name(pool_name) || !bifrost_validate_name(reader_name)) {
        bifrost_set_error(BIFROST_ERR_INVALID);
        return NULL;
    }
    
    /* Open the pool */
    bifrost_pool_t* pool = bifrost_open_pool(pool_name);
    if (!pool) {
        return NULL;
    }
    
    /* Atomically claim a participant entry with a unique name */
    bifrost_participant_t* participant = bifrost_participant_join(pool, reader_name,
                                                        BIFROST_PARTICIPANT_READER);
    if (!participant) {
        bifrost_close_pool(pool);
        return NULL;
    }
    
    /* Calculate participant index */
    uint32_t participant_idx = (uint32_t)(participant - pool->participants);
    
    /* Allocate reader handle */
    bifrost_reader_t* reader = calloc(1, sizeof(bifrost_reader_t));
    if (!reader) {
        bifrost_participant_release(participant);
        bifrost_close_pool(pool);
        bifrost_set_error(BIFROST_ERR_NOMEM);
        return NULL;
    }
    
    /* Finish participant record */
    bifrost_atomic_store(&participant->slot_offset, 0);  /* Readers don't have slots */
    bifrost_atomic_store(&participant->join_time, bifrost_get_timestamp());
    
    /* Set up reader handle (ID = participant index + 1, like writers) */
    reader->pool = pool;
    reader->id = participant_idx + 1;
    reader->participant_idx = participant_idx;
    reader->gen = bifrost_atomic_load(&participant->generation);
    reader->uid = (uint32_t)getuid();
    strncpy(reader->name, reader_name, BIFROST_NAME_MAX - 1);
    
    /* Attach reply box (no-op if commands are disabled) */
    if (bifrost_cmd_reader_attach(reader) != 0) {
        bifrost_participant_release(participant);
        bifrost_close_pool(pool);
        free(reader);
        bifrost_set_error(BIFROST_ERR_NOMEM);
        return NULL;
    }
    
    /* Increment reader count */
    bifrost_atomic_fetch_add(&pool->header->reader_count, 1);
    
    /* Become visible (includes a full barrier) */
    bifrost_participant_publish(participant);
    
    return reader;
}

void bifrost_reader_leave(bifrost_reader_t* reader) {
    if (!reader || !reader->pool) {
        return;
    }
    
    bifrost_pool_t* pool = reader->pool;
    bifrost_participant_t* participant = &pool->participants[reader->participant_idx];
    
    bifrost_cmd_reader_detach(reader);
    
    /* Decrement reader count */
    bifrost_atomic_fetch_add(&pool->header->reader_count, -1);
    
    /* Mark participant as inactive (hide -> clear -> free, free is last) */
    bifrost_participant_release(participant);
    
    bifrost_memory_barrier();
    
    /* Close pool and free handle */
    bifrost_close_pool(pool);
    free(reader);
}

/* ============================================================================
 * Internal: Find Writer
 * ============================================================================ */

/**
 * Find a writer's participant index by name.
 * Returns index, or UINT32_MAX if not found.
 */
static uint32_t find_writer_index(bifrost_pool_t* pool, const char* writer_name) {
    uint32_t max = bifrost_atomic_load(&pool->header->max_participants);
    
    for (uint32_t i = 0; i < max; i++) {
        bifrost_participant_t* p = &pool->participants[i];
        
        if (bifrost_atomic_load(&p->active) == BIFROST_P_LIVE &&
            bifrost_atomic_load(&p->type) == BIFROST_PARTICIPANT_WRITER &&
            strcmp(p->name, writer_name) == 0) {
            return i;
        }
    }
    
    return UINT32_MAX;
}

/**
 * Map a public writer ID (participant index + 1) to its index.
 * O(1). Returns UINT32_MAX if the ID does not denote a live writer.
 */
static inline uint32_t writer_index_from_id(bifrost_pool_t* pool, uint32_t writer_id) {
    uint32_t idx = writer_id - 1;   /* writer_id == 0 wraps to UINT32_MAX */
    if (idx >= bifrost_atomic_load(&pool->header->max_participants)) {
        return UINT32_MAX;
    }
    bifrost_participant_t* p = &pool->participants[idx];
    if (bifrost_atomic_load(&p->active) != BIFROST_P_LIVE ||
        bifrost_atomic_load(&p->type) != BIFROST_PARTICIPANT_WRITER) {
        return UINT32_MAX;
    }
    return idx;
}

/* ============================================================================
 * Reader Operations
 * ============================================================================ */

void* bifrost_reader_get(bifrost_reader_t* reader, const char* writer_name, size_t* size_out) {
    if (!reader || !writer_name) {
        bifrost_set_error(BIFROST_ERR_INVALID);
        return NULL;
    }
    
    bifrost_pool_t* pool = reader->pool;
    
    /* Find the writer */
    uint32_t writer_idx = find_writer_index(pool, writer_name);
    if (writer_idx == UINT32_MAX) {
        bifrost_set_error(BIFROST_ERR_NOTFOUND);
        return NULL;
    }
    
    /* Get the writer's slot */
    bifrost_slot_t* slot = &pool->slots[writer_idx];
    
    /* Check if data is ready (lock-free read) */
    uint32_t state = bifrost_atomic_load(&slot->state);
    if (state != BIFROST_SLOT_READY) {
        /* Data not ready - return NULL without error (normal case) */
        return NULL;
    }
    
    /* Read data offset and size atomically */
    uint64_t data_offset = bifrost_atomic_load(&slot->data_offset);
    size_t data_size = bifrost_atomic_load(&slot->data_size);
    
    if (data_offset == 0 || data_size == 0) {
        return NULL;
    }
    
    /* Calculate pointer - direct access, no memcpy */
    void* data = (char*)pool->base + data_offset;
    
    /* Return size if requested */
    if (size_out) {
        *size_out = data_size;
    }
    
    /* Increment read counter */
    bifrost_atomic_fetch_add(&pool->header->total_reads, 1);
    
    /* Memory barrier to ensure we read consistent data */
    bifrost_memory_barrier();
    
    return data;
}

void* bifrost_reader_get_by_id(bifrost_reader_t* reader, uint32_t writer_id, size_t* size_out) {
    if (!reader || writer_id == 0) {
        bifrost_set_error(BIFROST_ERR_INVALID);
        return NULL;
    }
    
    bifrost_pool_t* pool = reader->pool;
    
    /* O(1): the ID encodes the slot index. (Previously this returned the
     * first READY writer regardless of the requested ID.) */
    uint32_t idx = writer_index_from_id(pool, writer_id);
    if (idx == UINT32_MAX) {
        bifrost_set_error(BIFROST_ERR_NOTFOUND);
        return NULL;
    }
    
    bifrost_slot_t* slot = &pool->slots[idx];
    
    if (bifrost_atomic_load(&slot->state) != BIFROST_SLOT_READY) {
        return NULL;
    }
    
    uint64_t offset = bifrost_atomic_load(&slot->data_offset);
    size_t size = bifrost_atomic_load(&slot->data_size);
    
    if (offset == 0 || size == 0) {
        return NULL;
    }
    
    if (size_out) *size_out = size;
    bifrost_atomic_fetch_add(&pool->header->total_reads, 1);
    bifrost_memory_barrier();
    return (char*)pool->base + offset;
}

uint32_t bifrost_reader_get_command(bifrost_reader_t* reader, const char* writer_name) {
    if (!reader || !writer_name) {
        return 0;
    }
    
    bifrost_pool_t* pool = reader->pool;
    
    uint32_t writer_idx = find_writer_index(pool, writer_name);
    if (writer_idx == UINT32_MAX) {
        return 0;
    }
    
    bifrost_slot_t* slot = &pool->slots[writer_idx];
    return bifrost_atomic_load(&slot->command);
}

uint32_t bifrost_reader_get_command_by_id(bifrost_reader_t* reader, uint32_t writer_id) {
    if (!reader || writer_id == 0) {
        return 0;
    }
    
    uint32_t idx = writer_index_from_id(reader->pool, writer_id);
    if (idx == UINT32_MAX) {
        return 0;
    }
    
    return bifrost_atomic_load(&reader->pool->slots[idx].command);
}

uint64_t bifrost_reader_get_version(bifrost_reader_t* reader, const char* writer_name) {
    if (!reader || !writer_name) {
        return 0;
    }
    
    bifrost_pool_t* pool = reader->pool;
    
    uint32_t writer_idx = find_writer_index(pool, writer_name);
    if (writer_idx == UINT32_MAX) {
        return 0;
    }
    
    bifrost_slot_t* slot = &pool->slots[writer_idx];
    return bifrost_atomic_load(&slot->version);
}

uint32_t bifrost_reader_find_writer(bifrost_reader_t* reader, const char* writer_name) {
    if (!reader || !writer_name) {
        return 0;
    }
    
    bifrost_pool_t* pool = reader->pool;
    uint32_t idx = find_writer_index(pool, writer_name);
    
    if (idx != UINT32_MAX) {
        /* ID = idx + 1 (0 means not found); same as bifrost_writer_get_id() */
        return idx + 1;
    }
    
    return 0;
}

uint32_t bifrost_reader_list_writers(bifrost_reader_t* reader, bifrost_writer_info_t* info_out,
                                 uint32_t max_count) {
    if (!reader || !info_out || max_count == 0) {
        return 0;
    }
    
    bifrost_pool_t* pool = reader->pool;
    uint32_t max = bifrost_atomic_load(&pool->header->max_participants);
    uint32_t count = 0;
    
    for (uint32_t i = 0; i < max && count < max_count; i++) {
        bifrost_participant_t* p = &pool->participants[i];
        
        if (bifrost_atomic_load(&p->active) == BIFROST_P_LIVE &&
            bifrost_atomic_load(&p->type) == BIFROST_PARTICIPANT_WRITER) {
            
            bifrost_writer_info_t* info = &info_out[count];
            
            info->id = i + 1;
            strncpy(info->name, p->name, 63);
            info->name[63] = '\0';
            info->active = true;
            info->generation = bifrost_atomic_load(&p->generation);
            
            bifrost_slot_t* slot = &pool->slots[i];
            info->current_command = bifrost_atomic_load(&slot->command);
            info->version = bifrost_atomic_load(&slot->version);
            
            count++;
        }
    }
    
    return count;
}
