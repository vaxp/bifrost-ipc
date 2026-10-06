# Bifrost

**Lock-Free Shared Memory Library for Linux**

A high-performance, completely lock-free shared memory library designed for communication between system daemons (writers) and desktop environment components (readers).

## Features

- 🔓 **Completely Lock-Free**: Uses atomic operations only - no mutexes or spinlocks
- 🚀 **Zero-Copy**: Direct pointer access - no memcpy operations
- 📈 **Dynamic Scaling**: Supports dynamic number of writers and readers
- 🎛️ **Command Layer**: Lock-free, zero-copy RPC-style request/reply system
- 🔧 **Simple API**: Easy-to-use interfaces for writers and readers
- 🐧 **Linux Native**: Built on POSIX shared memory (shm_open, mmap)

## 📚 Documentation

**[العربية (Arabic)](docs/ar/01_getting_started.md)** | **[English](docs/01_getting_started.md)**

Detailed documentation is available in the [`docs/`](docs/) directory:

*   [**01 - Getting Started**](docs/01_getting_started.md): Installation & First Steps. ([العربية](docs/ar/01_getting_started.md))
*   [**02 - User Guide**](docs/02_user_guide.md): Complete usage guide. ([العربية](docs/ar/02_user_guide.md))
*   [**03 - API Reference**](docs/03_api_reference.md): Full API documentation. ([العربية](docs/ar/03_api_reference.md))
*   [**04 - Internals**](docs/04_internals.md): Performance architecture. ([العربية](docs/ar/04_internals.md))
*   [**Network Tuning & AF_XDP**](docs/NETWORK_TUNING.md): Distributed networking guide. ([العربية](docs/ar/NETWORK_TUNING.md))

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Director (Pool Creator)                   │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                    Shared Memory Pool                        │
│  ┌─────────────┬─────────────┬─────────────────────────┐   │
│  │   Header    │ Participants │      Data Arena         │   │
│  │             │   & Slots    │   (Bump Allocator)      │   │
│  └─────────────┴─────────────┴─────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
        ▲                                         ▲
        │                                         │
   ┌────┴────┐                               ┌────┴────┐
   │ Writers │                               │ Readers │
   │(Daemons)│                               │  (DE)   │
   └─────────┘                               └─────────┘
```

## Quick Start

### Building

```bash
sudo apt update
sudo apt install libbpf-dev libxdp-dev -y

meson setup build --prefix=/usr --buildtype=release
meson compile -C build
meson install -C build

```

### Basic Usage

**1. Create a pool (Director):**

```c
#include <bifrost.h>

bifrost_pool_t* pool = bifrost_create_pool("my_pool", NULL);
// Pool is now ready for writers and readers
// ... later ...
bifrost_destroy_pool(pool);
```

**2. Write data (Daemon):**

```c
#include <bifrost.h>

// Join as writer
bifrost_writer_t* w = bifrost_join_as_writer("my_pool", "battery_daemon");

// Allocate space - returns direct pointer
typedef struct { int level; char status[32]; } battery_t;
battery_t* data = bifrost_writer_alloc(w, sizeof(battery_t));

// Write directly - NO memcpy!
data->level = 85;
strcpy(data->status, "Charging");

// Set command and publish
bifrost_writer_set_command(w, CMD_BATTERY_UPDATE);
bifrost_writer_publish(w);

// Cleanup
bifrost_writer_leave(w);
```

**3. Read data (Desktop Environment):**

```c
#include <bifrost.h>

// Join as reader
bifrost_reader_t* r = bifrost_join_as_reader("my_pool", "status_bar");

// Get command from writer
uint32_t cmd = bifrost_reader_get_command(r, "battery_daemon");

if (cmd == CMD_BATTERY_UPDATE) {
    // Read directly - NO memcpy!
    size_t size;
    battery_t* data = bifrost_reader_get(r, "battery_daemon", &size);
    
    if (data) {
        printf("Battery: %d%% - %s\n", data->level, data->status);
    }
}

// Cleanup
bifrost_reader_leave(r);
```

## Running Examples
```bash
# Terminal 1 - Start the pool director
./build/examples/pool_director

# Terminal 2 - Start a daemon writer
./build/examples/daemon_writer

# Terminal 3 - Start a DE reader
./build/examples/de_reader
```

## Running Tests & Benchmarks

```bash
# Run automated test suite
meson test -C build

# Run benchmarks
./build/tests/bifrost_ultra_bench
```

## API Reference

### Director Functions

| Function | Description |
|----------|-------------|
| `bifrost_create_pool()` | Create a new shared memory pool |
| `bifrost_destroy_pool()` | Destroy a pool and free resources |
| `bifrost_open_pool()` | Open an existing pool |
| `bifrost_get_pool_stats()` | Get pool statistics |

### Writer Functions

| Function | Description |
|----------|-------------|
| `bifrost_join_as_writer()` | Join pool as a writer |
| `bifrost_writer_alloc()` | Allocate space for data (returns direct pointer) |
| `bifrost_writer_publish()` | Publish data to readers |
| `bifrost_writer_set_command()` | Set command/operation type |
| `bifrost_writer_leave()` | Leave the pool |

### Reader Functions

| Function | Description |
|----------|-------------|
| `bifrost_join_as_reader()` | Join pool as a reader |
| `bifrost_reader_get()` | Get data from writer (returns direct pointer) |
| `bifrost_reader_get_command()` | Get command from writer |
| `bifrost_reader_get_version()` | Get data version |
| `bifrost_reader_list_writers()` | List all active writers |
| `bifrost_reader_leave()` | Leave the pool |

### Command Layer API (RPC)

Include `<bifrost_command.h>` to use the advanced Command & Control layer:

| Function | Description |
|----------|-------------|
| `bifrost_writer_register_command()` | Register a command in the catalog |
| `bifrost_reader_list_commands()` | List available commands for a writer |
| `bifrost_reader_call()` | Execute a synchronous RPC call |
| `bifrost_reader_send_command()` | Send an asynchronous command |
| `bifrost_writer_poll_command()` | Poll for incoming commands |
| `bifrost_writer_command_done()` | Complete request and send reply |

## Lock-Free Design

The library achieves lock-free operation through:

1. **Atomic Operations**: All shared state uses C11 atomics with appropriate memory ordering
2. **Bump Allocator**: Data allocation uses atomic fetch-add
3. **State Machine**: Slots use atomic state transitions (FREE → WRITING → READY)
4. **Version Numbers**: Readers can detect data changes without locks

```c
// Writer side
atomic_fetch_add(&data_end, size);      // Allocate
atomic_store(&slot->state, WRITING);     // Mark writing
// ... write data ...
atomic_thread_fence(memory_order_release);
atomic_store(&slot->state, READY);       // Publish

// Reader side
if (atomic_load(&slot->state) == READY) {
    atomic_thread_fence(memory_order_acquire);
    // ... read data directly ...
}
```

## License

GNU Affero General Public License v3.0 (AGPL-3.0) - see the [LICENSE](LICENSE) file for details.