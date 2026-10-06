# Bifrost C API Reference

Complete reference for all types, structures, and functions in `bifrost.h` and `bifrost_command.h`.

---

## 📋 Error Codes (`bifrost_error_t`)

```c
typedef enum {
    BIFROST_OK              =  0,  /**< Success */
    BIFROST_ERR_NOMEM       = -1,  /**< Out of memory */
    BIFROST_ERR_EXISTS      = -2,  /**< Pool/participant already exists */
    BIFROST_ERR_NOTFOUND    = -3,  /**< Pool/participant/command not found */
    BIFROST_ERR_FULL        = -4,  /**< Pool or command inbox is full */
    BIFROST_ERR_INVALID     = -5,  /**< Invalid argument or payload size */
    BIFROST_ERR_PERMISSION  = -6,  /**< Permission denied / filter rejected */
    BIFROST_ERR_SYSTEM      = -7,  /**< Underlying OS system error */
    BIFROST_ERR_CORRUPTED   = -8,  /**< Shared memory segment corrupted */
    BIFROST_ERR_VERSION     = -9,  /**< Protocol or ABI version mismatch */
    BIFROST_ERR_BUSY        = -10, /**< Resource busy (e.g. unreleased request) */
    BIFROST_ERR_TIMEOUT     = -11, /**< Wait or call operation timed out */
    BIFROST_ERR_DISABLED    = -12  /**< Feature disabled in this pool */
} bifrost_error_t;
```

---

## 🏊 Pool & Director Management

### `bifrost_config_init`
```c
void bifrost_config_init(bifrost_pool_config_t* config);
```
Initializes a `bifrost_pool_config_t` structure with sensible production defaults.

### `bifrost_create_pool`
```c
bifrost_pool_t* bifrost_create_pool(const char* name, const bifrost_pool_config_t* config);
```
Creates a new POSIX shared memory pool at `/dev/shm/bifrost_<name>`.
*   `name`: Pool name (maximum 56 characters, without leading slash).
*   `config`: Pool configuration, or `NULL` to use defaults.
*   **Returns**: Opaque pool handle on success, `NULL` on failure.

### `bifrost_open_pool`
```c
bifrost_pool_t* bifrost_open_pool(const char* name);
```
Attaches to an existing shared memory pool.

### `bifrost_close_pool`
```c
void bifrost_close_pool(bifrost_pool_t* pool);
```
Unmaps the pool from the current process without removing it from `/dev/shm`.

### `bifrost_destroy_pool`
```c
void bifrost_destroy_pool(bifrost_pool_t* pool);
```
Unlinks `/dev/shm/bifrost_<name>` and unmaps it. Should only be called by the Director when shutting down.

### `bifrost_get_pool_stats`
```c
bifrost_error_t bifrost_get_pool_stats(bifrost_pool_t* pool, bifrost_pool_stats_t* stats);
```
Fills a `bifrost_pool_stats_t` structure with live usage metrics (used bytes, free bytes, active participants, read/write counters).

### `bifrost_expand_pool`
```c
bifrost_error_t bifrost_expand_pool(bifrost_pool_t* pool, size_t new_size);
```
Expands the physical capacity of the shared memory pool.

### `bifrost_pool_exists`
```c
bool bifrost_pool_exists(const char* name);
```
Returns `true` if `/dev/shm/bifrost_<name>` currently exists.

---

## ✍️ Writer (Data Producer) API

### `bifrost_join_as_writer`
```c
bifrost_writer_t* bifrost_join_as_writer(const char* pool_name, const char* writer_name);
```
Registers the calling process as an active writer in the designated pool.
*   `writer_name`: Unique identifier string (maximum 63 characters).
*   **Returns**: Writer handle, or `NULL` on error.

### `bifrost_writer_alloc`
```c
void* bifrost_writer_alloc(bifrost_writer_t* writer, size_t size);
```
Allocates a zero-copy data slot inside the pool arena.
*   **Returns**: Direct pointer into shared memory. Valid until the next alloc or leave.

### `bifrost_writer_publish`
```c
void bifrost_writer_publish(bifrost_writer_t* writer);
```
Commits data modifications using release memory ordering and increments the sequence counter.

### `bifrost_writer_set_command`
```c
void bifrost_writer_set_command(bifrost_writer_t* writer, uint32_t command);
```
Publishes an integer opcode/command tag alongside data for readers to inspect.

### `bifrost_writer_get_id`
```c
uint32_t bifrost_writer_get_id(bifrost_writer_t* writer);
```
Returns the unique participant ID assigned to this writer slot.

### `bifrost_writer_get_version`
```c
uint64_t bifrost_writer_get_version(bifrost_writer_t* writer);
```
Returns the current publish sequence counter for this writer.

### `bifrost_writer_leave`
```c
void bifrost_writer_leave(bifrost_writer_t* writer);
```
Releases the writer's slots, clears registration, and closes the writer handle.

---

## 📖 Reader (Data Consumer) API

### `bifrost_join_as_reader`
```c
bifrost_reader_t* bifrost_join_as_reader(const char* pool_name, const char* reader_name);
```
Registers the calling process as a reader in the shared pool.

### `bifrost_reader_get`
```c
void* bifrost_reader_get(bifrost_reader_t* reader, const char* writer_name, size_t* size_out);
```
Resolves and returns a direct pointer to the current data published by `writer_name`.
*   Zero-copy: no memory copy is performed.

### `bifrost_reader_get_by_id`
```c
void* bifrost_reader_get_by_id(bifrost_reader_t* reader, uint32_t writer_id, size_t* size_out);
```
Fast $O(1)$ direct pointer lookup bypassing string matching.

### `bifrost_reader_get_version`
```c
uint64_t bifrost_reader_get_version(bifrost_reader_t* reader, const char* writer_name);
```
Returns the atomic sequence counter for the specified writer.

### `bifrost_reader_get_command` / `bifrost_reader_get_command_by_id`
```c
uint32_t bifrost_reader_get_command(bifrost_reader_t* reader, const char* writer_name);
uint32_t bifrost_reader_get_command_by_id(bifrost_reader_t* reader, uint32_t writer_id);
```
Returns the integer command/opcode tag set by the writer.

### `bifrost_reader_find_writer`
```c
uint32_t bifrost_reader_find_writer(bifrost_reader_t* reader, const char* writer_name);
```
Finds a writer by name and returns its integer ID (or 0 if not found).

### `bifrost_reader_list_writers`
```c
uint32_t bifrost_reader_list_writers(bifrost_reader_t* reader, bifrost_writer_info_t* info_out, uint32_t max_count);
```
Fills `info_out` with snapshots of all active writers in the pool.

### `bifrost_reader_leave`
```c
void bifrost_reader_leave(bifrost_reader_t* reader);
```
Unregisters the reader and frees its handle.

---

## 🎛️ Command Layer API (`bifrost_command.h`)

### Writer RPC Functions

*   `bifrost_error_t bifrost_writer_register_command(bifrost_writer_t* writer, const bifrost_cmd_desc_t* desc);`
    Registers a command schema in the writer's catalog.
*   `bifrost_error_t bifrost_writer_unregister_command(bifrost_writer_t* writer, uint32_t cmd_id);`
    Removes a command from the catalog.
*   `const bifrost_cmd_request_t* bifrost_writer_poll_command(bifrost_writer_t* writer);`
    Polls the next incoming request non-blockingly (zero-copy pointer directly into shared memory).
*   `bifrost_error_t bifrost_writer_command_done(bifrost_writer_t* writer, const bifrost_cmd_request_t* req, int32_t status, const void* reply, uint32_t reply_len);`
    Completes the outstanding request and sends the response payload back to the reader.
*   `bifrost_error_t bifrost_writer_wait_command(bifrost_writer_t* writer, uint64_t timeout_ns);`
    Futex-based low-power sleep waiting for incoming requests.
*   `bifrost_error_t bifrost_writer_on_command(bifrost_writer_t* writer, uint32_t cmd_id, bifrost_cmd_handler_fn fn, void* ctx);`
    Registers an automated callback function for a specific command ID.
*   `void bifrost_writer_set_command_filter(bifrost_writer_t* writer, bifrost_cmd_filter_fn fn, void* ctx);`
    Installs an access-control hook evaluated before passing requests to handlers.
*   `uint32_t bifrost_writer_dispatch(bifrost_writer_t* writer, uint32_t max_cmds);`
    Processes up to `max_cmds` pending requests using registered callbacks.
*   `bifrost_error_t bifrost_writer_get_sender_name(bifrost_writer_t* writer, const bifrost_cmd_request_t* req, char* out, size_t out_size);`
    Retrieves the participant name of the request sender.
*   `uint64_t bifrost_writer_replies_dropped(bifrost_writer_t* writer);`
    Returns count of replies dropped due to full client reply boxes or dead readers.

### Reader RPC Functions

*   `bifrost_writer_ref_t bifrost_reader_resolve_writer(bifrost_reader_t* reader, const char* writer_name);`
    Resolves writer name to an ABA-safe reference (`id` + `gen`).
*   `bool bifrost_reader_writer_alive(bifrost_reader_t* reader, bifrost_writer_ref_t writer);`
    Verifies that the target writer process is still alive and has not been recycled.
*   `uint32_t bifrost_reader_list_commands(bifrost_reader_t* reader, bifrost_writer_ref_t writer, bifrost_cmd_info_t* out, uint32_t max_count);`
    Retrieves a consistent snapshot of the writer's command catalog.
*   `uint32_t bifrost_reader_find_command(bifrost_reader_t* reader, bifrost_writer_ref_t writer, const char* cmd_name, bifrost_cmd_info_t* info_out);`
    Finds a command ID by name within the writer's catalog.
*   `bifrost_error_t bifrost_reader_send_command(bifrost_reader_t* reader, bifrost_writer_ref_t writer, uint32_t cmd_id, const void* args, uint32_t arg_len, uint32_t flags, uint64_t* ticket_out);`
    Enqueues an asynchronous request into the writer's inbox.
*   `const bifrost_cmd_reply_t* bifrost_reader_poll_reply(bifrost_reader_t* reader);`
    Polls for an incoming reply.
*   `void bifrost_reader_reply_done(bifrost_reader_t* reader, const bifrost_cmd_reply_t* reply);`
    Releases the reply slot returned by `bifrost_reader_poll_reply()`.
*   `bifrost_error_t bifrost_reader_wait_reply(bifrost_reader_t* reader, uint64_t timeout_ns);`
    Sleeps using futex until a reply is placed into the reader's reply box.
*   `bifrost_error_t bifrost_reader_call(bifrost_reader_t* reader, bifrost_writer_ref_t writer, uint32_t cmd_id, const void* args, uint32_t arg_len, void* reply_buf, uint32_t* reply_len, int32_t* status_out, uint64_t timeout_ns);`
    High-level synchronous RPC: sends a request and waits for matching reply.

---

## 🛠️ Utility Functions

*   `bifrost_error_t bifrost_get_last_error(void);`
    Returns the thread-local error code for the most recent failed operation.
*   `const char* bifrost_error_string(bifrost_error_t error);`
    Returns human-readable text for any error code.
*   `const char* bifrost_version(void);`
    Returns library version string (e.g. `"0.1.0"`).
