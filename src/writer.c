/**
 * Bifrost - Writer Implementation
 * 
 * Lock-free writer interface for system daemons.
 */

#define _GNU_SOURCE
#include "bifrost_internal.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ============================================================================
 * Writer Join/Leave
 * ============================================================================ */

bifrost_writer_t* bifrost_join_as_writer(const char* pool_name, const char* writer_name) {
    /* Validate inputs */
    if (!bifrost_validate_name(pool_name) || !bifrost_validate_name(writer_name)) {
        bifrost_set_error(BIFROST_ERR_INVALID);
        return NULL;
    }
    
    /* Open the pool */
    bifrost_pool_t* pool = bifrost_open_pool(pool_name);
    if (!pool) {
        return NULL;
    }
    
    /* Atomically claim a participant entry with a unique name */
    bifrost_participant_t* participant = bifrost_participant_join(pool, writer_name,
                                                        BIFROST_PARTICIPANT_WRITER);
    if (!participant) {
        bifrost_close_pool(pool);
        return NULL;
    }
    
    /* Calculate participant index */
    uint32_t participant_idx = (uint32_t)(participant - pool->participants);
    
    /* Get a slot for this writer */
    bifrost_slot_t* slot = bifrost_get_writer_slot(pool, participant_idx);
    bifrost_writer_t* writer = slot ? calloc(1, sizeof(bifrost_writer_t)) : NULL;
    if (!writer) {
        bifrost_participant_release(participant);
        bifrost_close_pool(pool);
        bifrost_set_error(slot ? BIFROST_ERR_NOMEM : BIFROST_ERR_FULL);
        return NULL;
    }
    
    /* Finish participant record (still invisible to scanners until LIVE) */
    bifrost_atomic_store(&participant->slot_offset, 
                    (uint64_t)((char*)slot - (char*)pool->base));
    bifrost_atomic_store(&participant->join_time, bifrost_get_timestamp());
    
    /* Initialize slot */
    bifrost_atomic_store(&slot->state, BIFROST_SLOT_FREE);
    bifrost_atomic_store(&slot->command, 0);
    bifrost_atomic_store(&slot->version, 0);
    bifrost_atomic_store(&slot->data_size, 0);
    bifrost_atomic_store(&slot->data_offset, 0);
    
    /* Set up writer handle. The public ID is the participant index + 1 so it
     * is identical everywhere (get_id / find_writer / list_writers / get_by_id). */
    writer->pool = pool;
    writer->id = participant_idx + 1;
    writer->participant_idx = participant_idx;
    writer->slot = slot;
    writer->current_alloc = NULL;
    writer->current_size = 0;
    writer->gen = bifrost_atomic_load(&participant->generation);
    strncpy(writer->name, writer_name, BIFROST_NAME_MAX - 1);
    
    /* Attach command inbox + catalog (no-op if commands are disabled) */
    if (bifrost_cmd_writer_attach(writer) != 0) {
        bifrost_participant_release(participant);
        bifrost_close_pool(pool);
        free(writer);
        bifrost_set_error(BIFROST_ERR_NOMEM);
        return NULL;
    }
    
    /* Increment writer count */
    bifrost_atomic_fetch_add(&pool->header->writer_count, 1);
    
    /* Become visible to readers (includes a full barrier) */
    bifrost_participant_publish(participant);
    
    return writer;
}

void bifrost_writer_leave(bifrost_writer_t* writer) {
    if (!writer || !writer->pool) {
        return;
    }
    
    bifrost_pool_t* pool = writer->pool;
    bifrost_participant_t* participant = &pool->participants[writer->participant_idx];
    
    /* Clear catalog, answer queued requests with BIFROST_ERR_NOTFOUND */
    bifrost_cmd_writer_detach(writer);
    
    /* Mark slot as stale */
    if (writer->slot) {
        bifrost_atomic_store(&writer->slot->state, BIFROST_SLOT_STALE);
    }
    
    /* Decrement writer count */
    bifrost_atomic_fetch_add(&pool->header->writer_count, -1);
    
    /* Mark participant as inactive (hide -> clear -> free, free is last) */
    bifrost_participant_release(participant);
    
    bifrost_memory_barrier();
    
    /* Close pool and free handle */
    bifrost_close_pool(pool);
    free(writer);
}

/* ============================================================================
 * Writer Operations
 * ============================================================================ */

void* bifrost_writer_alloc(bifrost_writer_t* writer, size_t size) {
    if (!writer || size == 0) {
        bifrost_set_error(BIFROST_ERR_INVALID);
        return NULL;
    }
    
    bifrost_slot_t* slot = writer->slot;
    
    /* Mark slot as allocating */
    bifrost_atomic_store(&slot->state, BIFROST_SLOT_ALLOCATING);
    
    /* Allocate from arena */
    void* ptr = bifrost_arena_alloc(writer->pool, size);
    if (!ptr) {
        bifrost_atomic_store(&slot->state, BIFROST_SLOT_FREE);
        return NULL;
    }
    
    /* Update slot with new allocation info */
    bifrost_atomic_store(&slot->data_offset, 
                    (uint64_t)((char*)ptr - (char*)writer->pool->base));
    bifrost_atomic_store(&slot->data_size, size);
    
    /* Mark as writing */
    bifrost_atomic_store(&slot->state, BIFROST_SLOT_WRITING);
    
    /* Store in writer handle */
    writer->current_alloc = ptr;
    writer->current_size = size;
    
    return ptr;
}

void bifrost_writer_publish(bifrost_writer_t* writer) {
    if (!writer || !writer->slot) {
        return;
    }
    
    bifrost_slot_t* slot = writer->slot;
    
    /* Increment version */
    bifrost_atomic_fetch_add(&slot->version, 1);
    
    /* Update timestamp */
    bifrost_atomic_store(&slot->timestamp, bifrost_get_timestamp());
    
    /* Memory barrier to ensure all data writes are visible first */
    bifrost_memory_barrier();
    
    /* Now mark as ready - readers can see data after this */
    bifrost_atomic_store(&slot->state, BIFROST_SLOT_READY);
    
    /* Increment total writes counter */
    bifrost_atomic_fetch_add(&writer->pool->header->total_writes, 1);
    
    /* Clear current allocation in handle (no longer modifiable) */
    writer->current_alloc = NULL;
    writer->current_size = 0;
}

void bifrost_writer_set_command(bifrost_writer_t* writer, uint32_t command) {
    if (!writer || !writer->slot) {
        return;
    }
    
    bifrost_atomic_store(&writer->slot->command, command);
}

uint32_t bifrost_writer_get_id(bifrost_writer_t* writer) {
    if (!writer) {
        return 0;
    }
    return writer->id;
}

uint64_t bifrost_writer_get_version(bifrost_writer_t* writer) {
    if (!writer || !writer->slot) {
        return 0;
    }
    return bifrost_atomic_load(&writer->slot->version);
}
