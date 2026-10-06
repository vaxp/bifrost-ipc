# Bifrost Comprehensive User Guide

This guide covers all operational aspects of Bifrost: shared memory architecture, zero-copy data streaming, the lock-free Command & Control (RPC) layer, and high-performance network bridging.

---

## 🏗️ Core Architecture & Concepts

```
┌─────────────────────────────────────────────────────────────┐
│                 Director (Pool Creator)                     │
│               bifrost_create_pool("my_pool")                │
└──────────────────────────────┬──────────────────────────────┘
                               │ creates /dev/shm/bifrost_my_pool
                               ▼
┌─────────────────────────────────────────────────────────────┐
│                     Shared Memory Pool                      │
│ ┌───────────────┬────────────────────────┬────────────────┐ │
│ │  Pool Header  │  Participants & Slots  │   Data Arena   │ │
│ │ (Stats, Magic)│  (ABA Gen, Descriptors)│(Bump Allocator)│ │
│ ├───────────────┴────────────────────────┴────────────────┤ │
│ │           Command Region (Ring Inboxes & Replies)       │ │
│ └─────────────────────────────────────────────────────────┘ │
└──────────────────────────────┬──────────────────────────────┘
         ▲                     │                     ▲
         │ Direct SHM Pointers │ Direct SHM Pointers │
┌────────┴────────┐            │            ┌────────┴────────┐
│  Writer Daemon  │◀───────────┴───────────▶│   Reader (UI)   │
│  (Data Server)  │      Command Requests   │  (Data Client)  │
└─────────────────┘      & Lock-Free Replies└─────────────────┘
```

1. **Pool**: A POSIX shared memory object (`/dev/shm/bifrost_NAME`). It holds participant descriptors, slot metadata, the bump-allocated data arena, and command queue rings.
2. **Director**: The process that creates and formats the pool via `bifrost_create_pool()`.
3. **Writer**: A process that publishes data to its dedicated slots and/or registers an RPC command catalog.
4. **Reader**: A consumer process that discovers writers, reads published data with zero memcpy overhead, and executes commands.
5. **Command Region**: High-speed, bounded MPSC queues for bidirectional request/reply messaging without mutexes.

---

## 📝 Step-by-Step Implementation

### Phase 1: Creating and Configuring the Pool

The pool is typically initialized by a supervisor or daemon at system startup.

```c
#include <stdio.h>
#include <bifrost.h>

int main(void) {
    bifrost_pool_config_t config;
    bifrost_config_init(&config);

    // Data arena settings
    config.initial_size = 16 * 1024 * 1024;    // 16 MB initial capacity
    config.max_size = 256 * 1024 * 1024;       // 256 MB max expansion
    config.max_participants = 64;              // Max concurrent writers + readers
    config.max_slots_per_writer = 16;          // Max slots per writer
    config.auto_expand = true;

    // Command layer settings
    config.cmd_inbox_depth = 64;               // Requests queued per writer (power of 2)
    config.cmd_reply_depth = 32;               // Replies queued per reader (power of 2)
    config.cmd_max_args = 256;                 // Max argument payload in bytes
    config.cmd_max_reply = 256;                // Max reply payload in bytes
    config.cmd_max_per_writer = 32;            // Command catalog capacity per writer

    bifrost_pool_t* pool = bifrost_create_pool("production_pool", &config);
    if (!pool) {
        // Pool might already exist from a previous run
        pool = bifrost_open_pool("production_pool");
        if (!pool) {
            fprintf(stderr, "Failed to initialize shared pool\n");
            return 1;
        }
    }

    printf("Bifrost pool initialized successfully.\n");
    // Keep alive or close when shutting down:
    // bifrost_destroy_pool(pool);
    return 0;
}
```

---

### Phase 2: The Writer (Publishing Data)

Writers allocate memory directly within the shared memory pool and update it in-place:

```c
#include <bifrost.h>
#include <unistd.h>

typedef struct {
    double cpu_usage;
    uint64_t ram_bytes_used;
    uint32_t active_tasks;
} SystemMetrics;

int main(void) {
    // 1. Join pool
    bifrost_writer_t* writer = bifrost_join_as_writer("production_pool", "sysmon_service");
    if (!writer) return 1;

    // 2. Allocate ONCE (Best practice: avoid re-allocating in hot loops)
    SystemMetrics* metrics = (SystemMetrics*)bifrost_writer_alloc(writer, sizeof(SystemMetrics));

    while (running) {
        // 3. Mutate data directly in shared memory
        metrics->cpu_usage = read_cpu();
        metrics->ram_bytes_used = read_ram();
        metrics->active_tasks = read_tasks();

        // 4. Publish update (Atomically bumps the sequence version)
        bifrost_writer_publish(writer);

        usleep(50000); // 50ms interval
    }

    // 5. Clean exit
    bifrost_writer_leave(writer);
    return 0;
}
```

---

### Phase 3: The Reader (Zero-Copy Polling)

Readers can read either by name or by fast cached ID:

```c
#include <stdio.h>
#include <unistd.h>
#include <bifrost.h>

int main(void) {
    bifrost_reader_t* reader = bifrost_join_as_reader("production_pool", "status_widget");
    if (!reader) return 1;

    // Cache the writer ID for fast O(1) reads without string comparison
    uint32_t writer_id = 0;
    while (!writer_id && running) {
        writer_id = bifrost_reader_find_writer(reader, "sysmon_service");
        if (!writer_id) usleep(10000);
    }

    uint64_t last_version = 0;

    while (running) {
        // Fast ID-based data retrieval
        size_t size = 0;
        const SystemMetrics* metrics = (const SystemMetrics*)bifrost_reader_get_by_id(
            reader, writer_id, &size
        );

        if (metrics) {
            uint64_t ver = bifrost_reader_get_version(reader, "sysmon_service");
            if (ver > last_version) {
                printf("CPU: %.1f%% | RAM: %lu MB\n", 
                       metrics->cpu_usage, metrics->ram_bytes_used / (1024 * 1024));
                last_version = ver;
            }
        }
        usleep(50000);
    }

    bifrost_reader_leave(reader);
    return 0;
}
```

---

### Phase 4: Command & Control Layer (Lock-Free RPC)

The Command Layer lets Readers invoke functions on Writers with payload arguments and receive structured replies.

#### Method A: Manual Request Polling & Done

```c
#include <bifrost.h>
#include <bifrost_command.h>

#define CMD_SET_POWER_MODE 10

// 1. Register command in writer's catalog
bifrost_cmd_desc_t desc = {
    .id = CMD_SET_POWER_MODE,
    .name = "set_power_mode",
    .desc = "Sets profile: 0=low, 1=balanced, 2=performance",
    .arg_min = sizeof(int32_t),
    .arg_max = sizeof(int32_t),
    .reply_max = sizeof(int32_t)
};
bifrost_writer_register_command(writer, &desc);

// 2. Poll in event loop
const bifrost_cmd_request_t* req = bifrost_writer_poll_command(writer);
if (req) {
    if (req->cmd_id == CMD_SET_POWER_MODE) {
        int32_t new_mode = *(const int32_t*)req->args;
        int32_t result = apply_power_mode(new_mode);

        // Reply directly back to caller
        bifrost_writer_command_done(writer, req, BIFROST_OK, &result, sizeof(result));
    }
}
```

#### Method B: Automated Dispatcher Handler

```c
static int32_t handle_power_mode(const bifrost_cmd_request_t* req, 
                                 void* reply, uint32_t* reply_len, void* ctx) {
    int32_t mode = *(const int32_t*)req->args;
    *(int32_t*)reply = apply_power_mode(mode);
    *reply_len = sizeof(int32_t);
    return BIFROST_OK;
}

// Bind handler
bifrost_writer_on_command(writer, CMD_SET_POWER_MODE, handle_power_mode, NULL);

// In event loop, dispatch pending commands (up to 8 per iteration):
bifrost_writer_dispatch(writer, 8);
```

#### Calling from the Reader (Synchronous RPC)

```c
bifrost_writer_ref_t target = bifrost_reader_resolve_writer(reader, "sysmon_service");
if (target.id != 0) {
    int32_t desired_mode = 2; // Performance
    int32_t reply_val = 0;
    uint32_t reply_len = sizeof(reply_val);
    int32_t status = 0;

    // Send and wait with 100ms timeout
    bifrost_error_t err = bifrost_reader_call(
        reader, target,
        CMD_SET_POWER_MODE,
        &desired_mode, sizeof(desired_mode),
        &reply_val, &reply_len,
        &status,
        100000000ULL // 100ms in nanoseconds
    );

    if (err == BIFROST_OK && status == BIFROST_OK) {
        printf("Power mode set successfully. Response: %d\n", reply_val);
    }
}
```

---

## ⚡ Best Practices & Threading Rules

1. **Thread Concurrency**:
   - A single `bifrost_writer_t` or `bifrost_reader_t` handle must be accessed by only **one thread** at a time.
   - For multi-threaded applications, create a separate writer/reader handle per worker thread.
2. **Allocation in Hot Paths**:
   - Always call `bifrost_writer_alloc()` once at initialization whenever possible.
   - Avoid calling alloc repeatedly in high-frequency loops.
3. **ABA Guarding**:
   - Always use `bifrost_reader_resolve_writer()` to get a `bifrost_writer_ref_t`.
   - The generation field ensures that if a writer crashes and restarts in the same slot, old requests fail safely instead of corrupting the new writer.
