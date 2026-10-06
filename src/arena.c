/**
 * Bifrost - Arena Allocator
 * 
 * Simple bump allocator for the data area.
 * Lock-free using atomic fetch-add.
 */

#define _GNU_SOURCE
#include "bifrost_internal.h"
#include <string.h>

/* ============================================================================
 * Arena Allocation
 * ============================================================================ */

/**
 * Allocate memory from the pool's data arena.
 * 
 * This is completely lock-free using atomic fetch-add.
 * The allocation is permanent (no free) - the arena resets when the pool restarts.
 * 
 * @param pool Pool handle
 * @param size Size to allocate (will be aligned)
 * @return Pointer to allocated memory, NULL if full
 */
void* bifrost_arena_alloc(bifrost_pool_t* pool, size_t size) {
    if (!pool || size == 0) {
        bifrost_set_error(BIFROST_ERR_INVALID);
        return NULL;
    }
    
    /* Align size to cache line boundary */
    size_t aligned_size = BIFROST_ALIGN_UP(size);
    
    bifrost_pool_header_t* header = pool->header;
    
    /* Atomically reserve space */
    size_t old_end = bifrost_atomic_fetch_add(&header->data_end, aligned_size);
    size_t new_end = old_end + aligned_size;
    
    /* Check if we exceeded pool size */
    size_t total_size = bifrost_atomic_load(&header->total_size);
    if (new_end > total_size) {
        /* Allocation failed - try to expand if auto-expand is enabled */
        if (header->auto_expand && new_end <= header->max_size) {
            /* TODO: Implement auto-expansion 
             * For now, just fail */
            bifrost_set_error(BIFROST_ERR_FULL);
            /* Rollback the allocation */
            bifrost_atomic_fetch_add(&header->data_end, -(ptrdiff_t)aligned_size);
            return NULL;
        }
        
        bifrost_set_error(BIFROST_ERR_FULL);
        /* Rollback the allocation */
        bifrost_atomic_fetch_add(&header->data_end, -(ptrdiff_t)aligned_size);
        return NULL;
    }
    
    /* Calculate pointer */
    void* ptr = (char*)pool->base + old_end;
    
    return ptr;
}

/**
 * Get available space in the arena.
 * 
 * @param pool Pool handle
 * @return Available bytes
 */
size_t bifrost_arena_available(bifrost_pool_t* pool) {
    if (!pool) {
        return 0;
    }
    
    bifrost_pool_header_t* header = pool->header;
    size_t total = bifrost_atomic_load(&header->total_size);
    size_t used = bifrost_atomic_load(&header->data_end);
    
    return (used < total) ? (total - used) : 0;
}

/**
 * Reset arena to initial state (only for Director).
 * 
 * @warning This invalidates all existing allocations!
 * @param pool Pool handle
 */
void bifrost_arena_reset(bifrost_pool_t* pool) {
    if (!pool || !pool->is_creator) {
        return;
    }
    
    bifrost_pool_header_t* header = pool->header;
    size_t data_start = bifrost_atomic_load(&header->data_start);
    
    bifrost_atomic_store(&header->data_end, data_start);
}
