# Getting Started with Bifrost

Bifrost is a high-performance, lock-free Inter-Process Communication (IPC) library based on C11 atomics and POSIX shared memory (`/dev/shm`). It is engineered for microsecond-level latency, zero-copy data streaming, and non-blocking RPC commands between Linux processes.

---

## 📦 Requirements & Prerequisites

*   **Operating System**: Linux (Kernel 3.17+ for full memfd/futex support, 5.4+ recommended for AF_XDP)
*   **Compiler**: GCC 4.9+ or Clang 3.6+ (C11 support required)
*   **Build System**: [Meson](https://mesonbuild.com/) (>= 0.55) & [Ninja](https://ninja-build.org/)
*   **Dependencies**:
    *   `librt` & `pthreads` (standard POSIX libraries on Linux)
    *   `libbpf-dev` & `libxdp-dev` *(optional, for AF_XDP kernel bypass networking)*

### Installing Dependencies

On **Ubuntu / Debian**:
```bash
sudo apt update
sudo apt install -y build-essential meson ninja-build libbpf-dev libxdp-dev
```

On **Fedora / RHEL / CentOS**:
```bash
sudo dnf install -y gcc meson ninja-build libbpf-devel libxdp-devel
```

On **Arch Linux**:
```bash
sudo pacman -S --needed base-devel meson ninja libbpf
```

---

## 🛠️ Building & Installation

Bifrost uses **Meson** and **Ninja** as its standard build system.

### 1. Configure & Build

```bash
# Setup the build directory (Release mode with native optimizations)
meson setup build --prefix=/usr --buildtype=release

# Compile the library, examples, and tests
ninja -C build
```

### 2. Run Tests & Benchmarks

```bash
# Run test suite
meson test -C build

# Or run the ultra-benchmark suite
./build/tests/bifrost_ultra_bench
```

### 3. Install System-Wide (Optional)

```bash
sudo ninja -C build install
sudo ldconfig
```

This installs:
- Headers to `/usr/include/bifrost.h` and `/usr/include/bifrost_command.h`
- Shared library `/usr/lib/libbifrost.so`
- Static library `/usr/lib/libbifrost_static.a`

---

## 🔗 Linking in Your Project

### Option A: Using Meson (Recommended)
If your project uses Meson, use `dependency('bifrost')` or `cc.find_library('bifrost')`:

```meson
project('my_project', 'c', default_options : ['c_std=gnu11'])

bifrost_dep = dependency('bifrost', required : true)
# Or if linking manually:
# cc = meson.get_compiler('c')
# bifrost_dep = cc.find_library('bifrost', required : true)
# rt_dep = cc.find_library('rt', required : true)

executable('my_app', 'main.c', dependencies : [bifrost_dep])
```

### Option B: Using CMake
If your project uses CMake, locate Bifrost using `find_library` or `pkg-config`:

```cmake
cmake_minimum_required(VERSION 3.12)
project(my_project C)

set(CMAKE_C_STANDARD 11)

find_library(BIFROST_LIB bifrost REQUIRED)
find_package(Threads REQUIRED)

add_executable(my_app main.c)
target_link_libraries(my_app PRIVATE ${BIFROST_LIB} Threads::Threads rt)
```

### Option C: Using GCC / Clang Directly

```bash
gcc -std=c11 -O3 main.c -o my_app -lbifrost -lpthread -lrt
```

---

## 🚀 Quick Start Examples

### Example 1: Shared Memory Producer (Writer)
A background daemon publishing continuous sensor/telemetry data without any locks or memory copies:

```c
#include <stdio.h>
#include <unistd.h>
#include <bifrost.h>

typedef struct {
    double cpu_temp;
    uint64_t frame_index;
} TelemetryData;

int main(void) {
    // 1. Initialize configuration and create the shared pool
    bifrost_pool_config_t config;
    bifrost_config_init(&config);
    config.initial_size = 4 * 1024 * 1024; // 4 MB pool

    bifrost_pool_t* pool = bifrost_create_pool("system_telemetry", &config);
    if (!pool) {
        // Pool might already exist, open it
        pool = bifrost_open_pool("system_telemetry");
    }

    // 2. Register as a writer
    bifrost_writer_t* writer = bifrost_join_as_writer("system_telemetry", "hwmon_daemon");

    // 3. Allocate shared memory slot (Direct zero-copy pointer)
    TelemetryData* data = (TelemetryData*)bifrost_writer_alloc(writer, sizeof(TelemetryData));

    // 4. Update data and publish
    for (int i = 0; i < 100; i++) {
        data->cpu_temp = 42.5 + (i * 0.1);
        data->frame_index = i;

        // Atomic publish: readers see this immediately
        bifrost_writer_publish(writer);
        usleep(10000); // 10ms
    }

    // 5. Cleanup
    bifrost_writer_leave(writer);
    bifrost_close_pool(pool);
    return 0;
}
```

### Example 2: Shared Memory Consumer (Reader)
A UI or consumer process reading the telemetry directly from shared memory:

```c
#include <stdio.h>
#include <unistd.h>
#include <bifrost.h>

typedef struct {
    double cpu_temp;
    uint64_t frame_index;
} TelemetryData;

int main(void) {
    // 1. Join pool as a reader
    bifrost_reader_t* reader = bifrost_join_as_reader("system_telemetry", "status_ui");
    if (!reader) {
        fprintf(stderr, "Failed to connect to pool!\n");
        return 1;
    }

    uint64_t last_ver = 0;

    // 2. Poll for updates
    for (int i = 0; i < 50; i++) {
        uint64_t ver = bifrost_reader_get_version(reader, "hwmon_daemon");
        if (ver > last_ver) {
            size_t size = 0;
            // Direct zero-copy pointer access (NO memcpy!)
            const TelemetryData* data = (const TelemetryData*)bifrost_reader_get(reader, "hwmon_daemon", &size);
            if (data) {
                printf("[UI] Frame #%lu | CPU Temp: %.1f C\n", data->frame_index, data->cpu_temp);
                last_ver = ver;
            }
        }
        usleep(10000);
    }

    // 3. Cleanup
    bifrost_reader_leave(reader);
    return 0;
}
```

### Example 3: Command & Control Layer (Lock-Free RPC)
Bifrost includes `<bifrost_command.h>` for non-blocking RPC requests and responses:

**Writer (Server):**
```c
#include <bifrost.h>
#include <bifrost_command.h>

// Register command in catalog
bifrost_cmd_desc_t desc = {
    .id = 101,
    .name = "ping",
    .desc = "Echo ping request",
    .arg_min = 0,
    .arg_max = 64,
    .reply_max = 64
};
bifrost_writer_register_command(writer, &desc);

// In event loop:
const bifrost_cmd_request_t* req = bifrost_writer_poll_command(writer);
if (req) {
    // Process and reply directly
    const char reply_msg[] = "pong";
    bifrost_writer_command_done(writer, req, BIFROST_OK, reply_msg, sizeof(reply_msg));
}
```

**Reader (Client):**
```c
#include <bifrost.h>
#include <bifrost_command.h>

bifrost_writer_ref_t server = bifrost_reader_resolve_writer(reader, "hwmon_daemon");
if (server.id != 0) {
    char reply[64] = {0};
    uint32_t reply_len = sizeof(reply);
    int32_t status = 0;

    // Synchronous RPC with 500ms timeout
    bifrost_error_t err = bifrost_reader_call(
        reader, server, 101, NULL, 0,
        reply, &reply_len, &status, 500000000ULL
    );

    if (err == BIFROST_OK && status == BIFROST_OK) {
        printf("Received reply: %s\n", reply);
    }
}
```

---

## 📖 Next Steps

*   [**User Guide**](02_user_guide.md): In-depth walkthrough of pools, arenas, dynamic expansion, and command dispatchers.
*   [**API Reference**](03_api_reference.md): Detailed function definitions and error codes.
*   [**Internals & Performance**](04_internals.md): Lock-free design, atomic barriers, and latency optimization.
*   [**Network & AF_XDP Tuning**](NETWORK_TUNING.md): Sub-microsecond distributed communication and kernel bypass.
