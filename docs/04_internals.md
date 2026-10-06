# Bifrost Internals & Architecture

This document details the low-level memory layout, concurrency primitives, and optimization techniques that give Bifrost its sub-microsecond latency and high throughput.

---

## 🧠 Low-Level Memory Layout

A Bifrost shared memory pool (`/dev/shm/bifrost_<name>`) is mapped contiguously into the virtual address space of participating processes. Internally, all pointers are stored as **relative offsets** from the pool base address, making the pool relocatable even when mapped at different base addresses in different processes.

```
┌─────────────────────────────────────────────────────────────┐
│ 1. Pool Header (64-byte cacheline aligned)                  │
│    - Magic ('BFR1'), ABI Version, Header Size               │
│    - Global Atomic Stats (writes, reads, active counts)     │
│    - Arena Offset, Command Region Offset                    │
├─────────────────────────────────────────────────────────────┤
│ 2. Participant & Slot Directory                             │
│    - Fixed array of participant slots (max_participants)    │
│    - Slot state machine: FREE -> WRITING -> READY           │
│    - ABA Generation Counters (incremented on each recycle)  │
│    - Data version sequence counters                         │
├─────────────────────────────────────────────────────────────┤
│ 3. Data Arena (Offset-based Bump Allocator)                 │
│    - Contiguous byte buffer for published data              │
│    - Lock-free atomic bump allocation (fetch_add)           │
├─────────────────────────────────────────────────────────────┤
│ 4. Command & Control Region (RPC Mailboxes)                 │
│    - Writer Inboxes: Lock-free MPSC bounded ring buffers    │
│    - Reader Reply Boxes: Dedicated circular queues          │
│    - Futex words for low-power event notification           │
└─────────────────────────────────────────────────────────────┘
```

---

## ⚡ Concurrency & Memory Ordering

Bifrost does not use POSIX mutexes or heavyweight OS synchronization locks in its communication path.

### 1. Data Publishing (Release Semantics)
When a writer publishes updated data:
```c
// 1. Data written directly into pre-allocated memory slot
slot->data_ptr[i] = value;

// 2. Hardware memory fence ensuring writes are committed to RAM/L3 before flag update
atomic_thread_fence(memory_order_release);

// 3. Atomic store marks data as READY and increments version
atomic_store_explicit(&slot->version, new_ver, memory_order_release);
atomic_store_explicit(&slot->state, BIFROST_SLOT_READY, memory_order_release);
```

### 2. Data Reading (Acquire Semantics)
When a reader accesses data:
```c
// 1. Acquire load ensures subsequent reads cannot be speculatively reordered before this check
if (atomic_load_explicit(&slot->state, memory_order_acquire) == BIFROST_SLOT_READY) {
    uint64_t v = atomic_load_explicit(&slot->version, memory_order_acquire);
    // 2. Direct pointer access to memory without copying
    consume(slot->data_ptr);
}
```

### 3. ABA Safety via Generation Counters
If a writer daemon terminates abnormally and another process registers in the same slot index, an outstanding reader request could erroneously execute on the new process. Bifrost solves this via `bifrost_writer_ref_t` which pairs the slot ID with an atomically incremented generation counter:
```c
typedef struct {
    uint32_t id;    // Participant slot index
    uint32_t gen;   // Incarnation counter
} bifrost_writer_ref_t;
```
If the generation does not match the live slot generation, requests are rejected with `BIFROST_ERR_NOTFOUND`.

### 4. Futex Sleep vs. Active Spinning
*   **Spinning Mode**: For ultra-low latency (< 1 microsecond), callers poll in tight loops.
*   **Futex Mode**: Functions like `bifrost_writer_wait_command()` and `bifrost_reader_wait_reply()` spin briefly in user-space, then fall back to the Linux `SYS_futex` syscall to put the thread to sleep without burning CPU cycles when idle.

---

## 🚀 Performance Optimization Checklist

### 1. Zero-Allocation Loops
*   **Anti-Pattern**: Calling `bifrost_writer_alloc()` inside a high-frequency loop (e.g. 60 FPS or 1 MHz).
*   **Best Practice**: Allocate your structure once during startup, then update the values in place and call `bifrost_writer_publish()`.

### 2. Fast ID Lookups
*   **Anti-Pattern**: Calling `bifrost_reader_get(reader, "writer_name", &size)` in every frame. String comparison adds overhead.
*   **Best Practice**: Resolve the writer once with `bifrost_reader_find_writer()`, then call `bifrost_reader_get_by_id()` for instant array indexing.

### 3. CPU Core Pinning & OS Isolation
On Linux systems dedicated to high-frequency processing:
1. Isolate target CPU cores from the kernel scheduler via kernel parameter: `isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3`.
2. Pin the Writer thread to Core 2 (`pthread_setaffinity_np`).
3. Pin the Reader thread to Core 3.
4. Set CPU frequency governor to `performance`.

### 4. Huge Pages
For large frame buffers (such as 4K/8K video streaming):
Mount `/dev/shm` with huge page support or configure transparent huge pages (THP) to reduce Translation Lookaside Buffer (TLB) misses.
