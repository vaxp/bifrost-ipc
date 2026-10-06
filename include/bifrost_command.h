/**
 * Bifrost - Command Layer
 *
 * Lets a writer advertise a catalog of commands it understands, lets readers
 * browse that catalog and submit requests ("incoming order boxes"), and lets
 * the writer send back a full reply.
 *
 *   Writer                                   Reader
 *   ------                                   ------
 *   bifrost_writer_register_command()  ──catalog──▶ bifrost_reader_list_commands()
 *                                              bifrost_reader_find_command()
 *   bifrost_writer_poll_command()      ◀──inbox───  bifrost_reader_send_command()
 *   bifrost_writer_command_done()      ──replies──▶ bifrost_reader_poll_reply()
 *                                              bifrost_reader_call()  (send + wait)
 *
 * Design guarantees:
 *  - The data path (bifrost_writer_alloc / publish / bifrost_reader_get) is untouched.
 *  - Lock-free: per-writer inbox and per-reader reply box are bounded MPSC
 *    rings living in their own cache lines of the shared pool.
 *  - Zero-copy: polled requests/replies point directly into shared memory.
 *  - Never blocks the writer: polling is non-blocking (~1 atomic load when
 *    empty); a full reply box drops the reply instead of stalling.
 *  - No syscalls in the fast path. Optional futex sleep is only woken when a
 *    peer is actually sleeping.
 *  - Sizes (queue depth, max argument/reply bytes, catalog capacity) are set
 *    per pool by the Director through bifrost_pool_config_t.
 *
 * Threading: a bifrost_writer_t / bifrost_reader_t handle must be used by one thread at
 * a time. Use one handle per thread if you need parallel senders.
 *
 * Copyright (C) 2026-2027 Hussein Jaafar
 * License: GNU Affero General Public License v3.0 (AGPL-3.0)
 */

#ifndef BIFROST_COMMAND_H
#define BIFROST_COMMAND_H

#include "bifrost.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

#define BIFROST_CMD_NAME_MAX     32      /**< Including terminating NUL */
#define BIFROST_CMD_DESC_MAX     72      /**< Including terminating NUL */

/** Command descriptor flags (catalog). */
#define BIFROST_CMD_F_SAME_UID   0x0002u /**< Only senders with the writer's UID (cooperative check) */

/** Per-request flags (bifrost_reader_send_command). A reply is always sent unless
 *  the sender asks otherwise, so a caller waiting for a reply never hangs. */
#define BIFROST_SEND_F_NO_REPLY  0x0001u /**< Fire-and-forget: do not send a reply */

/* ============================================================================
 * Types
 * ============================================================================ */

/**
 * Stable reference to a writer. `gen` guards against ABA: if the writer leaves
 * and another one takes its slot, sends using the old reference fail with
 * BIFROST_ERR_NOTFOUND instead of reaching the wrong writer.
 */
typedef struct {
    uint32_t id;    /**< Writer ID (0 = invalid) */
    uint32_t gen;   /**< Writer generation */
} bifrost_writer_ref_t;

/**
 * Command descriptor - published by the writer, read by readers.
 * Fixed 128 bytes so it lives in shared memory as-is.
 */
typedef struct {
    uint32_t id;                    /**< Writer-defined command ID (non-zero) */
    uint32_t flags;                 /**< BIFROST_CMD_F_* */
    uint32_t arg_min;               /**< Minimum argument bytes */
    uint32_t arg_max;               /**< Maximum argument bytes (<= pool cmd_max_args) */
    uint32_t reply_max;             /**< Maximum reply bytes (informational, <= pool cmd_max_reply) */
    uint32_t reserved;              /**< Reserved for future permission model (must be 0) */
    char     name[BIFROST_CMD_NAME_MAX]; /**< Unique name within the writer, e.g. "set_brightness" */
    char     desc[BIFROST_CMD_DESC_MAX]; /**< Human readable description */
} bifrost_cmd_desc_t;

/** What readers get when browsing a catalog (identical layout). */
typedef bifrost_cmd_desc_t bifrost_cmd_info_t;

/**
 * Incoming request, as seen by the writer. Points directly into shared memory
 * (zero-copy) and is valid until bifrost_writer_command_done() is called.
 */
typedef struct {
    uint64_t ticket;        /**< Correlates the reply with the request */
    uint32_t cmd_id;        /**< Requested command */
    uint32_t flags;         /**< BIFROST_SEND_F_* */
    uint32_t sender_id;     /**< Reader participant ID */
    uint32_t sender_gen;    /**< Reader generation */
    uint32_t target_gen;    /**< Writer generation the sender targeted */
    uint32_t arg_len;       /**< Bytes in args[] */
    int32_t  sender_pid;    /**< Sender process ID */
    uint32_t sender_uid;    /**< Sender user ID (cooperative, not authenticated) */
    uint8_t  args[];        /**< Inline arguments */
} bifrost_cmd_request_t;

/**
 * Reply, as seen by the reader. Valid until bifrost_reader_reply_done().
 */
typedef struct {
    uint64_t ticket;        /**< Ticket of the originating request */
    uint32_t cmd_id;        /**< Command that was executed */
    int32_t  status;        /**< BIFROST_OK, a bifrost_error_t, or a writer-defined code */
    uint32_t writer_id;     /**< Writer that replied */
    uint32_t writer_gen;    /**< Writer generation */
    uint32_t target_gen;    /**< Reader generation (internal routing) */
    uint32_t len;           /**< Bytes in data[] */
    uint8_t  reserved[8];
    uint8_t  data[];        /**< Inline reply payload */
} bifrost_cmd_reply_t;

/**
 * Optional handler for bifrost_writer_dispatch().
 *
 * @param req        Incoming request
 * @param reply      Buffer for the reply payload (capacity *reply_len)
 * @param reply_len  In: capacity. Out: bytes written.
 * @param ctx        User context given to bifrost_writer_on_command()
 * @return           Status sent back to the reader (BIFROST_OK on success)
 */
typedef int32_t (*bifrost_cmd_handler_fn)(const bifrost_cmd_request_t* req,
                                     void* reply, uint32_t* reply_len,
                                     void* ctx);

/**
 * Optional access-control hook, run by the writer before a request is handed
 * to the application. Return BIFROST_OK to accept, or an error (typically
 * BIFROST_ERR_PERMISSION) which is replied automatically.
 */
typedef int32_t (*bifrost_cmd_filter_fn)(const bifrost_cmd_request_t* req,
                                    const bifrost_cmd_desc_t* desc, void* ctx);

/* ============================================================================
 * Writer API
 * ============================================================================ */

/** Register a command in this writer's catalog. Typically at startup. */
bifrost_error_t bifrost_writer_register_command(bifrost_writer_t* writer, const bifrost_cmd_desc_t* desc);

/** Remove a command from the catalog. */
bifrost_error_t bifrost_writer_unregister_command(bifrost_writer_t* writer, uint32_t cmd_id);

/**
 * Fetch the next request (non-blocking, zero-copy).
 *
 * Invalid requests (unknown command, bad size, permission filter, stale
 * target) are answered automatically and never returned.
 *
 * @return Request pointer, or NULL if the inbox is empty. Only one request may
 *         be outstanding: call bifrost_writer_command_done() before polling again
 *         (otherwise NULL is returned and the error is BIFROST_ERR_BUSY).
 */
const bifrost_cmd_request_t* bifrost_writer_poll_command(bifrost_writer_t* writer);

/**
 * Complete the outstanding request and send the reply (unless the sender used
 * BIFROST_SEND_F_NO_REPLY). Never blocks: if the reader's reply box is full the
 * reply is dropped. `reply` may point into req->args (echo is safe).
 *
 * @return BIFROST_OK, BIFROST_ERR_FULL (reply dropped), BIFROST_ERR_NOTFOUND (reader gone),
 *         BIFROST_ERR_INVALID (reply too large / no outstanding request).
 *         In all cases the request slot is released.
 */
bifrost_error_t bifrost_writer_command_done(bifrost_writer_t* writer, const bifrost_cmd_request_t* req,
                                  int32_t status, const void* reply, uint32_t reply_len);

/**
 * Sleep until a request is available (spin briefly, then futex).
 * @param timeout_ns 0 = check once, UINT64_MAX = wait forever
 * @return BIFROST_OK if a request is available, BIFROST_ERR_TIMEOUT otherwise
 */
bifrost_error_t bifrost_writer_wait_command(bifrost_writer_t* writer, uint64_t timeout_ns);

/** Bind a local handler for bifrost_writer_dispatch(). fn = NULL removes it. */
bifrost_error_t bifrost_writer_on_command(bifrost_writer_t* writer, uint32_t cmd_id,
                                bifrost_cmd_handler_fn fn, void* ctx);

/** Install an access-control filter (NULL removes it). */
void bifrost_writer_set_command_filter(bifrost_writer_t* writer, bifrost_cmd_filter_fn fn, void* ctx);

/**
 * Poll and run up to max_cmds requests through bound handlers.
 * Requests without a handler are answered with BIFROST_ERR_NOTFOUND.
 * @return Number of requests processed
 */
uint32_t bifrost_writer_dispatch(bifrost_writer_t* writer, uint32_t max_cmds);

/** Copy the sender's participant name (for logging / access control). */
bifrost_error_t bifrost_writer_get_sender_name(bifrost_writer_t* writer, const bifrost_cmd_request_t* req,
                                     char* out, size_t out_size);

/** Replies dropped because a reader's reply box was full or the reader left. */
uint64_t bifrost_writer_replies_dropped(bifrost_writer_t* writer);

/* ============================================================================
 * Reader API
 * ============================================================================ */

/** Resolve a writer by name into a stable reference ({0,0} if not found). */
bifrost_writer_ref_t bifrost_reader_resolve_writer(bifrost_reader_t* reader, const char* writer_name);

/** Check that the referenced writer is still the same live process. */
bool bifrost_reader_writer_alive(bifrost_reader_t* reader, bifrost_writer_ref_t writer);

/**
 * Copy the writer's catalog (consistent snapshot, lock-free).
 * @return Number of entries copied
 */
uint32_t bifrost_reader_list_commands(bifrost_reader_t* reader, bifrost_writer_ref_t writer,
                                 bifrost_cmd_info_t* out, uint32_t max_count);

/** Find a command by name. Optionally copies its descriptor. @return ID or 0. */
uint32_t bifrost_reader_find_command(bifrost_reader_t* reader, bifrost_writer_ref_t writer,
                                const char* cmd_name, bifrost_cmd_info_t* info_out);

/**
 * Submit a request to the writer's inbox (non-blocking).
 * @param ticket_out Optional, receives the ticket to match the reply
 * @return BIFROST_OK, BIFROST_ERR_FULL (inbox full, retry later), BIFROST_ERR_NOTFOUND
 *         (writer gone), BIFROST_ERR_INVALID (args too large), BIFROST_ERR_DISABLED
 */
bifrost_error_t bifrost_reader_send_command(bifrost_reader_t* reader, bifrost_writer_ref_t writer,
                                  uint32_t cmd_id, const void* args, uint32_t arg_len,
                                  uint32_t flags, uint64_t* ticket_out);

/**
 * Fetch the next reply (non-blocking, zero-copy). Call bifrost_reader_reply_done()
 * before polling again.
 */
const bifrost_cmd_reply_t* bifrost_reader_poll_reply(bifrost_reader_t* reader);

/** Release the reply returned by bifrost_reader_poll_reply(). */
void bifrost_reader_reply_done(bifrost_reader_t* reader, const bifrost_cmd_reply_t* reply);

/**
 * Sleep until a reply is available (spin briefly, then futex).
 * @return BIFROST_OK or BIFROST_ERR_TIMEOUT
 */
bifrost_error_t bifrost_reader_wait_reply(bifrost_reader_t* reader, uint64_t timeout_ns);

/**
 * Synchronous call: send, then wait for the matching reply. Unrelated replies
 * that arrive meanwhile are kept and returned later by bifrost_reader_poll_reply().
 *
 * @param reply_buf     Optional buffer for the reply payload
 * @param reply_len     In: buffer capacity. Out: full reply length (if it is
 *                      larger than the capacity, the copy was truncated)
 * @param status_out    Optional, receives the writer's status
 * @param timeout_ns    Total time budget (UINT64_MAX = forever)
 * @return BIFROST_OK if a reply was received (check *status_out), otherwise the
 *         send error, BIFROST_ERR_TIMEOUT, or BIFROST_ERR_NOTFOUND if the writer
 *         left/crashed while waiting, or BIFROST_ERR_BUSY if a polled reply has
 *         not been released with bifrost_reader_reply_done()
 */
bifrost_error_t bifrost_reader_call(bifrost_reader_t* reader, bifrost_writer_ref_t writer,
                          uint32_t cmd_id, const void* args, uint32_t arg_len,
                          void* reply_buf, uint32_t* reply_len,
                          int32_t* status_out, uint64_t timeout_ns);

#ifdef __cplusplus
}
#endif

#endif /* BIFROST_COMMAND_H */
