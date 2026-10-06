/**
 * Bifrost - Lock-Free Shared Memory Library
 * 
 * A high-performance, lock-free shared memory library for Linux.
 * Designed for communication between system daemons (writers) and
 * desktop environment components (readers).
 * 
 * Features:
 * - Completely lock-free using atomic operations only
 * - No memcpy - direct pointer access
 * - Dynamic expansion of writers/readers
 * - Zero-copy data sharing
 * Copyright (C) 2026-2027 Hussein Jaafar
 * License: GNU Affero General Public License v3.0 (AGPL-3.0)
 */

#ifndef BIFROST_H
#define BIFROST_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Type Definitions
 * ============================================================================ */

/**
 * Opaque handle to a shared memory pool.
 * Created by the Director, used by writers and readers.
 */
typedef struct bifrost_pool bifrost_pool_t;

/**
 * Opaque handle for a writer participant.
 * Obtained by joining the pool as a writer.
 */
typedef struct bifrost_writer bifrost_writer_t;

/**
 * Opaque handle for a reader participant.
 * Obtained by joining the pool as a reader.
 */
typedef struct bifrost_reader bifrost_reader_t;

/**
 * Slot states - used internally but exposed for debugging.
 */
typedef enum {
    BIFROST_SLOT_FREE = 0,       /**< Slot is free and available */
    BIFROST_SLOT_ALLOCATING,     /**< Slot is being allocated */
    BIFROST_SLOT_WRITING,        /**< Writer is writing data */
    BIFROST_SLOT_READY,          /**< Data is ready for reading */
    BIFROST_SLOT_STALE           /**< Data is stale/outdated */
} bifrost_slot_state_t;

/**
 * Participant type.
 */
typedef enum {
    BIFROST_PARTICIPANT_NONE = 0,
    BIFROST_PARTICIPANT_WRITER,
    BIFROST_PARTICIPANT_READER
} bifrost_participant_type_t;

/**
 * Error codes.
 */
typedef enum {
    BIFROST_OK = 0,                  /**< Success */
    BIFROST_ERR_NOMEM = -1,          /**< Out of memory */
    BIFROST_ERR_EXISTS = -2,         /**< Pool/participant already exists */
    BIFROST_ERR_NOTFOUND = -3,       /**< Pool/participant not found */
    BIFROST_ERR_FULL = -4,           /**< Pool is full, needs expansion */
    BIFROST_ERR_INVALID = -5,        /**< Invalid argument */
    BIFROST_ERR_PERMISSION = -6,     /**< Permission denied */
    BIFROST_ERR_SYSTEM = -7,         /**< System error (check errno) */
    BIFROST_ERR_CORRUPTED = -8,      /**< Pool data corrupted */
    BIFROST_ERR_VERSION = -9,        /**< Version mismatch */
    BIFROST_ERR_BUSY = -10,          /**< Resource is busy */
    BIFROST_ERR_TIMEOUT = -11,       /**< Operation timed out */
    BIFROST_ERR_DISABLED = -12       /**< Feature disabled in this pool (e.g. commands) */
} bifrost_error_t;

/**
 * Pool configuration options.
 */
typedef struct {
    size_t initial_size;        /**< Initial pool size in bytes (default: 1MB) */
    size_t max_size;            /**< Maximum pool size (default: 256MB) */
    uint32_t max_participants;  /**< Max writers + readers (default: 64) */
    uint32_t max_slots_per_writer; /**< Max slots per writer (default: 16) */
    bool auto_expand;           /**< Auto-expand when full (default: true) */

    /* --- Command layer (see bifrost_command.h). The command region is allocated
     *     IN ADDITION to initial_size, so data capacity is unaffected. --- */
    uint32_t cmd_inbox_depth;   /**< Requests queued per writer, power of 2. 0 = commands disabled (default: 64) */
    uint32_t cmd_reply_depth;   /**< Replies queued per reader, power of 2 (default: 32) */
    uint32_t cmd_max_args;      /**< Max inline argument bytes per request (default: 192) */
    uint32_t cmd_max_reply;     /**< Max inline reply bytes per reply (default: 192) */
    uint32_t cmd_max_per_writer;/**< Catalog capacity per writer (default: 32) */
} bifrost_pool_config_t;

/**
 * Writer information - for enumeration.
 */
typedef struct {
    uint32_t id;                /**< Writer ID (same value as bifrost_writer_get_id) */
    char name[64];              /**< Writer name */
    bool active;                /**< Is writer active */
    uint32_t current_command;   /**< Current published command */
    uint64_t version;           /**< Data version counter */
    uint32_t generation;        /**< Incarnation of this writer slot (ABA guard) */
} bifrost_writer_info_t;

/**
 * Pool statistics.
 */
typedef struct {
    size_t total_size;          /**< Total pool size */
    size_t used_size;           /**< Used size */
    size_t free_size;           /**< Free size */
    uint32_t writer_count;      /**< Number of active writers */
    uint32_t reader_count;      /**< Number of active readers */
    uint64_t total_writes;      /**< Total write operations */
    uint64_t total_reads;       /**< Total read operations */
} bifrost_pool_stats_t;

/* ============================================================================
 * Director API - Pool Creation and Management
 * ============================================================================ */

/**
 * Initialize default pool configuration.
 * 
 * @param config Pointer to configuration structure to initialize
 */
void bifrost_config_init(bifrost_pool_config_t* config);

/**
 * Create a new shared memory pool.
 * 
 * This function creates and initializes a new shared memory pool.
 * The caller becomes the "Director" of this pool.
 * 
 * @param name   Pool name (will be prefixed with /bifrost_ for shm)
 * @param config Pool configuration (NULL for defaults)
 * @return       Pool handle on success, NULL on failure
 * 
 * @note The pool name should not start with '/'. Max 56 characters.
 */
bifrost_pool_t* bifrost_create_pool(const char* name, const bifrost_pool_config_t* config);

/**
 * Open an existing shared memory pool.
 * 
 * @param name Pool name
 * @return     Pool handle on success, NULL on failure
 */
bifrost_pool_t* bifrost_open_pool(const char* name);

/**
 * Destroy a shared memory pool.
 * 
 * This removes the pool and all associated resources.
 * All participants should have left before calling this.
 * 
 * @param pool Pool handle
 */
void bifrost_destroy_pool(bifrost_pool_t* pool);

/**
 * Close a pool handle without destroying the pool.
 * 
 * @param pool Pool handle
 */
void bifrost_close_pool(bifrost_pool_t* pool);

/**
 * Get pool statistics.
 * 
 * @param pool  Pool handle
 * @param stats Output statistics structure
 * @return      BIFROST_OK on success
 */
bifrost_error_t bifrost_get_pool_stats(bifrost_pool_t* pool, bifrost_pool_stats_t* stats);

/**
 * Expand pool size.
 * 
 * @param pool     Pool handle
 * @param new_size New size in bytes
 * @return         BIFROST_OK on success
 * 
 * @warning This may change the base address. Participants should
 *          re-acquire their pointers after expansion.
 */
bifrost_error_t bifrost_expand_pool(bifrost_pool_t* pool, size_t new_size);

/* ============================================================================
 * Writer API - Data Writing
 * ============================================================================ */

/**
 * Join a pool as a writer.
 * 
 * The writer can then allocate slots and publish data.
 * 
 * @param pool_name   Name of the pool to join
 * @param writer_name Unique name for this writer (max 63 chars)
 * @return            Writer handle on success, NULL on failure
 * 
 * @note Writers typically represent system daemons providing data.
 */
bifrost_writer_t* bifrost_join_as_writer(const char* pool_name, const char* writer_name);

/**
 * Leave the pool as a writer.
 * 
 * This frees all slots owned by this writer.
 * 
 * @param writer Writer handle
 */
void bifrost_writer_leave(bifrost_writer_t* writer);

/**
 * Allocate space for data in the writer's slot.
 * 
 * This returns a direct pointer to shared memory.
 * Write your data directly to this pointer.
 * 
 * @param writer Writer handle
 * @param size   Size of data to allocate
 * @return       Direct pointer to allocated space, NULL on failure
 * 
 * @note The returned pointer is valid until the next alloc call
 *       or until the writer leaves.
 * @warning DO NOT use memcpy to this pointer from the same pool!
 */
void* bifrost_writer_alloc(bifrost_writer_t* writer, size_t size);

/**
 * Publish the current data.
 * 
 * This marks the data as ready for readers. After this call,
 * readers can see the data. The writer should not modify the
 * data after publishing until the next alloc.
 * 
 * @param writer Writer handle
 * 
 * @note This uses a memory barrier to ensure visibility.
 */
void bifrost_writer_publish(bifrost_writer_t* writer);

/**
 * Set the command/operation type for readers.
 * 
 * Commands are defined by the writer to indicate what type
 * of data is available or what action readers should take.
 * 
 * @param writer  Writer handle
 * @param command Command value (user-defined)
 */
void bifrost_writer_set_command(bifrost_writer_t* writer, uint32_t command);

/**
 * Get the writer's unique ID.
 * 
 * This is the same ID returned by bifrost_reader_find_writer() and
 * bifrost_reader_list_writers(), and accepted by bifrost_reader_get_by_id().
 * 
 * @param writer Writer handle
 * @return       Writer ID (never 0)
 */
uint32_t bifrost_writer_get_id(bifrost_writer_t* writer);

/**
 * Get current data version.
 * 
 * Version increments with each publish.
 * 
 * @param writer Writer handle
 * @return       Current version
 */
uint64_t bifrost_writer_get_version(bifrost_writer_t* writer);

/* ============================================================================
 * Reader API - Data Reading
 * ============================================================================ */

/**
 * Join a pool as a reader.
 * 
 * @param pool_name   Name of the pool to join
 * @param reader_name Unique name for this reader (max 63 chars)
 * @return            Reader handle on success, NULL on failure
 * 
 * @note Readers typically represent desktop environment components.
 */
bifrost_reader_t* bifrost_join_as_reader(const char* pool_name, const char* reader_name);

/**
 * Leave the pool as a reader.
 * 
 * @param reader Reader handle
 */
void bifrost_reader_leave(bifrost_reader_t* reader);

/**
 * Get data from a writer by name.
 * 
 * Returns a direct pointer to the writer's data.
 * This is lock-free and does not copy data.
 * 
 * @param reader      Reader handle
 * @param writer_name Name of the writer
 * @param size_out    Optional output for data size
 * @return            Direct pointer to data, NULL if not ready
 * 
 * @warning The pointer is only valid while reading. Do not store it.
 * @warning Check the version if you need to know if data changed.
 */
void* bifrost_reader_get(bifrost_reader_t* reader, const char* writer_name, size_t* size_out);

/**
 * Get data from a writer by ID.
 * 
 * Faster than by-name lookup.
 * 
 * @param reader   Reader handle
 * @param writer_id Writer ID
 * @param size_out Optional output for data size
 * @return         Direct pointer to data, NULL if not ready
 */
void* bifrost_reader_get_by_id(bifrost_reader_t* reader, uint32_t writer_id, size_t* size_out);

/**
 * Get the command from a writer.
 * 
 * @param reader      Reader handle
 * @param writer_name Writer name
 * @return            Command value, 0 if writer not found
 */
uint32_t bifrost_reader_get_command(bifrost_reader_t* reader, const char* writer_name);

/**
 * Get command by writer ID.
 * 
 * @param reader    Reader handle
 * @param writer_id Writer ID
 * @return          Command value
 */
uint32_t bifrost_reader_get_command_by_id(bifrost_reader_t* reader, uint32_t writer_id);

/**
 * Get data version from a writer.
 * 
 * Use this to check if data has been updated since last read.
 * 
 * @param reader      Reader handle
 * @param writer_name Writer name
 * @return            Version number, 0 if not found
 */
uint64_t bifrost_reader_get_version(bifrost_reader_t* reader, const char* writer_name);

/**
 * Find a writer by name and get its ID.
 * 
 * @param reader      Reader handle
 * @param writer_name Writer name to find
 * @return            Writer ID, or 0 if not found
 */
uint32_t bifrost_reader_find_writer(bifrost_reader_t* reader, const char* writer_name);

/**
 * List all active writers.
 * 
 * @param reader    Reader handle
 * @param info_out  Array to fill with writer info
 * @param max_count Maximum entries in info_out
 * @return          Number of writers found
 */
uint32_t bifrost_reader_list_writers(bifrost_reader_t* reader, bifrost_writer_info_t* info_out, 
                                 uint32_t max_count);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/**
 * Get the last error code.
 * 
 * @return Last error code
 */
bifrost_error_t bifrost_get_last_error(void);

/**
 * Get error message for an error code.
 * 
 * @param error Error code
 * @return      Error message string
 */
const char* bifrost_error_string(bifrost_error_t error);

/**
 * Check if a pool exists.
 * 
 * @param name Pool name
 * @return     true if pool exists
 */
bool bifrost_pool_exists(const char* name);

/**
 * Get library version.
 * 
 * @return Version string
 */
const char* bifrost_version(void);

#ifdef __cplusplus
}
#endif

/* Command layer API (catalog / inbox / replies) */
#include "bifrost_command.h"

#endif /* BIFROST_H */
