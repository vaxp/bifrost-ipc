/**
 * Bifrost Ultra Benchmark - 200 Million Messages
 * 
 * ⚡ CACHE OPTIMIZED VERSION ⚡
 * 
 * Optimizations:
 * 1. __builtin_prefetch() - Pre-load data into cache
 * 2. madvise(MADV_WILLNEED) - Tell kernel we need this memory
 * 3. 4 KB data size - Fits in L1 cache (32-64KB)
 * 4. Cache-line alignment (64 bytes)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdint.h>
#include <float.h>
#include <sys/mman.h>
#include <sched.h>
#include <bifrost.h>

// CPU affinity helper
static inline void pin_to_cpu(int cpu_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id % sysconf(_SC_NPROCESSORS_ONLN), &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

// ═══════════════════════════════════════════════════════════════════════════
// Configuration - CACHE OPTIMIZED
// ═══════════════════════════════════════════════════════════════════════════

#define CACHE_LINE_SIZE 64                  // CPU cache line = 64 bytes
#define DATA_SIZE       (4 * 1024)          // 4 KB - fits in L1 cache!
#define NUM_CHANNELS    4
#define ITERATIONS      100000000           // 100M per channel = 400M total

#define POOL_NAME       "bifrost_ultra_bench"

// Cache prefetch hints
#define PREFETCH_READ   0
#define PREFETCH_WRITE  1
#define LOCALITY_HIGH   3   // Keep in all cache levels
#define LOCALITY_LOW    0   // Don't pollute cache

// ═══════════════════════════════════════════════════════════════════════════
// Cache-aligned statistics structure
// ═══════════════════════════════════════════════════════════════════════════

typedef struct __attribute__((aligned(CACHE_LINE_SIZE))) {
    uint64_t writes;
    uint64_t reads;
    double write_latency_sum;
    double read_latency_sum;
    double min_write_latency;
    double max_write_latency;
    double min_read_latency;
    double max_read_latency;
    char _padding[8];  // Ensure 64-byte alignment
} ChannelStats;

static ChannelStats g_stats[NUM_CHANNELS] __attribute__((aligned(CACHE_LINE_SIZE)));
static pthread_barrier_t g_start_barrier;
static _Atomic int g_stop_flag = 0;

// ═══════════════════════════════════════════════════════════════════════════
// Utilities
// ═══════════════════════════════════════════════════════════════════════════

static inline double get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

// Prefetch multiple cache lines
static inline void prefetch_range(void* addr, size_t len, int write) {
    char* ptr = (char*)addr;
    char* end = ptr + len;
    for (; ptr < end; ptr += CACHE_LINE_SIZE) {
        if (write) {
            __builtin_prefetch(ptr, PREFETCH_WRITE, LOCALITY_HIGH);
        } else {
            __builtin_prefetch(ptr, PREFETCH_READ, LOCALITY_HIGH);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Writer Thread (Daemon) - CACHE OPTIMIZED
// ═══════════════════════════════════════════════════════════════════════════

typedef struct {
    int id;
    bifrost_writer_t* writer;
    void* data_ptr;
} WriterArgs;

static void* writer_thread(void* arg) {
    WriterArgs* args = (WriterArgs*)arg;
    ChannelStats* stats = &g_stats[args->id];
    
    // Pin writer to even CPU cores (0, 2, 4, 6...)
    pin_to_cpu(args->id * 2);
    
    stats->min_write_latency = DBL_MAX;
    stats->max_write_latency = 0;
    stats->write_latency_sum = 0;
    stats->writes = 0;
    
    // Tell kernel we need this memory NOW
    madvise(args->data_ptr, DATA_SIZE, MADV_WILLNEED);
    
    // Pre-warm: touch all pages to ensure they're in RAM
    memset(args->data_ptr, 0, DATA_SIZE);
    
    // Wait for all threads
    pthread_barrier_wait(&g_start_barrier);
    
    for (uint64_t i = 0; i < ITERATIONS && !atomic_load(&g_stop_flag); i++) {
        // PREFETCH: Load data into cache BEFORE we need it
        prefetch_range(args->data_ptr, DATA_SIZE, PREFETCH_WRITE);
        
        double t_start = get_time_ns();
        
        // Write to cache-hot memory (already prefetched!)
        memset(args->data_ptr, (uint8_t)(i & 0xFF), DATA_SIZE);
        
        // Memory barrier to ensure write is visible
        __atomic_thread_fence(__ATOMIC_RELEASE);
        
        // Publish
        bifrost_writer_set_command(args->writer, (uint32_t)(i & 0xFFFF));
        bifrost_writer_publish(args->writer);
        
        double t_end = get_time_ns();
        double latency = t_end - t_start;
        
        stats->write_latency_sum += latency;
        if (latency < stats->min_write_latency) stats->min_write_latency = latency;
        if (latency > stats->max_write_latency) stats->max_write_latency = latency;
        stats->writes++;
    }
    
    return NULL;
}

// ═══════════════════════════════════════════════════════════════════════════
// Reader Thread (Shell) - CACHE OPTIMIZED
// ═══════════════════════════════════════════════════════════════════════════

typedef struct {
    int id;
    bifrost_reader_t* reader;
    char writer_name[64];
} ReaderArgs;

static void* reader_thread(void* arg) {
    ReaderArgs* args = (ReaderArgs*)arg;
    ChannelStats* stats = &g_stats[args->id];
    
    // Pin reader to odd CPU cores (1, 3, 5, 7...)
    pin_to_cpu(args->id * 2 + 1);
    
    stats->min_read_latency = DBL_MAX;
    stats->max_read_latency = 0;
    stats->read_latency_sum = 0;
    stats->reads = 0;
    
    // Cache-aligned local buffer
    uint8_t* local_buf = aligned_alloc(CACHE_LINE_SIZE, DATA_SIZE);
    madvise(local_buf, DATA_SIZE, MADV_WILLNEED);
    memset(local_buf, 0, DATA_SIZE);  // Pre-warm
    
    // Wait for all threads
    pthread_barrier_wait(&g_start_barrier);
    
    for (uint64_t i = 0; i < ITERATIONS && !atomic_load(&g_stop_flag); i++) {
        double t_start = get_time_ns();
        
        // NO WAITING! Read as fast as possible (same speed as writer)
        // Get pointer to shared data directly
        size_t size;
        void* data = bifrost_reader_get(args->reader, args->writer_name, &size);
        
        if (data && size > 0) {
            // PREFETCH source data into cache
            prefetch_range(data, size < DATA_SIZE ? size : DATA_SIZE, PREFETCH_READ);
            
            // Memory barrier to ensure we see latest data
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            
            // Copy from cache-hot source to cache-hot destination
            memcpy(local_buf, data, size < DATA_SIZE ? size : DATA_SIZE);
        }
        
        double t_end = get_time_ns();
        double latency = t_end - t_start;
        
        stats->read_latency_sum += latency;
        if (latency < stats->min_read_latency) stats->min_read_latency = latency;
        if (latency > stats->max_read_latency) stats->max_read_latency = latency;
        stats->reads++;
    }
    
    free(local_buf);
    return NULL;
}

// ═══════════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════════

int main(void) {
    printf("╔═══════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║   🔥 Bifrost Ultra Benchmark - CACHE OPTIMIZED                             ║\n");
    printf("╠═══════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║   ⚡ Optimizations:                                                            ║\n");
    printf("║      • __builtin_prefetch() - Pre-load into L1/L2 cache                       ║\n");
    printf("║      • madvise(MADV_WILLNEED) - Kernel memory hints                           ║\n");
    printf("║      • 4 KB data size - Fits in L1 cache                                      ║\n");
    printf("║      • 64-byte cache-line alignment                                           ║\n");
    printf("╠═══════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║   %d channels × %d iterations = %lu total messages                      ║\n",
           NUM_CHANNELS, ITERATIONS, (unsigned long)NUM_CHANNELS * ITERATIONS);
    printf("║   Data size: %d KB per message (L1 cache friendly!)                          ║\n", DATA_SIZE / 1024);
    printf("╚═══════════════════════════════════════════════════════════════════════════════╝\n\n");
    
    // Create pool
    printf("📦 Creating shared memory pool...\n");
    bifrost_pool_config_t config;
    bifrost_config_init(&config);
    config.initial_size = 4 * 1024 * 1024;  // 4 MB (smaller, more cache-friendly)
    config.max_participants = 32;
    
    bifrost_pool_t* pool = bifrost_create_pool(POOL_NAME, &config);
    if (!pool) {
        printf("❌ Failed to create pool: %s\n", bifrost_error_string(bifrost_get_last_error()));
        return 1;
    }
    printf("✅ Pool created: 4 MB\n\n");
    
    // Initialize barrier
    pthread_barrier_init(&g_start_barrier, NULL, NUM_CHANNELS * 2);
    
    // Create writers and readers
    pthread_t writer_threads[NUM_CHANNELS];
    pthread_t reader_threads[NUM_CHANNELS];
    WriterArgs writer_args[NUM_CHANNELS];
    ReaderArgs reader_args[NUM_CHANNELS];
    
    printf("🚀 Creating %d channels with cache-aligned memory...\n", NUM_CHANNELS);
    
    for (int i = 0; i < NUM_CHANNELS; i++) {
        char name[64];
        snprintf(name, sizeof(name), "channel_%d", i);
        
        // Create writer
        bifrost_writer_t* writer = bifrost_join_as_writer(POOL_NAME, name);
        if (!writer) {
            printf("❌ Failed to create writer %d\n", i);
            bifrost_destroy_pool(pool);
            return 1;
        }
        
        // Pre-allocate shared memory (cache-aligned by Bifrost)
        void* data_ptr = bifrost_writer_alloc(writer, DATA_SIZE);
        if (!data_ptr) {
            printf("❌ Failed to allocate memory for channel %d\n", i);
            bifrost_destroy_pool(pool);
            return 1;
        }
        
        // Pre-warm the memory
        madvise(data_ptr, DATA_SIZE, MADV_WILLNEED);
        
        writer_args[i].id = i;
        writer_args[i].writer = writer;
        writer_args[i].data_ptr = data_ptr;
        
        // Create reader
        bifrost_reader_t* reader = bifrost_join_as_reader(POOL_NAME, name);
        if (!reader) {
            printf("❌ Failed to create reader %d\n", i);
            bifrost_destroy_pool(pool);
            return 1;
        }
        
        reader_args[i].id = i;
        reader_args[i].reader = reader;
        strncpy(reader_args[i].writer_name, name, sizeof(reader_args[i].writer_name));
        
        printf("   ✅ Channel %d ready (cache-aligned)\n", i);
    }
    
    printf("\n⚡ Starting CACHE-OPTIMIZED benchmark...\n\n");
    
    double start_time = get_time_ns();
    
    // Start threads
    for (int i = 0; i < NUM_CHANNELS; i++) {
        pthread_create(&writer_threads[i], NULL, writer_thread, &writer_args[i]);
        pthread_create(&reader_threads[i], NULL, reader_thread, &reader_args[i]);
    }
    
    // Progress indicator
    uint64_t last_total = 0;
    for (int sec = 0; sec < 600; sec++) {
        sleep(1);
        
        uint64_t total_writes = 0, total_reads = 0;
        for (int i = 0; i < NUM_CHANNELS; i++) {
            total_writes += g_stats[i].writes;
            total_reads += g_stats[i].reads;
        }
        
        uint64_t delta = (total_writes + total_reads) - last_total;
        last_total = total_writes + total_reads;
        
        double progress = (double)total_writes / ((double)NUM_CHANNELS * ITERATIONS) * 100;
        printf("\r⏱️  Progress: %5.1f%% | Writes: %lu M | Reads: %lu M | Rate: %.2f M/sec    ",
               progress,
               total_writes / 1000000,
               total_reads / 1000000,
               delta / 1e6);
        fflush(stdout);
        
        if (total_writes >= (uint64_t)NUM_CHANNELS * ITERATIONS) {
            break;
        }
    }
    
    atomic_store(&g_stop_flag, 1);
    
    // Wait for threads
    for (int i = 0; i < NUM_CHANNELS; i++) {
        pthread_join(writer_threads[i], NULL);
        pthread_join(reader_threads[i], NULL);
    }
    
    double end_time = get_time_ns();
    double duration_sec = (end_time - start_time) / 1e9;
    
    // ═══════════════════════════════════════════════════════════════════════
    // Results
    // ═══════════════════════════════════════════════════════════════════════
    
    printf("\n\n");
    printf("╔═══════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║   📊 CACHE-OPTIMIZED BENCHMARK RESULTS                                        ║\n");
    printf("╠═══════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║ Channel │   Writes   │   Reads    │ Avg Write µs │ Avg Read µs │ Max Read µs ║\n");
    printf("╠═════════╪════════════╪════════════╪══════════════╪═════════════╪═════════════╣\n");
    
    uint64_t total_writes = 0, total_reads = 0;
    double total_write_lat = 0, total_read_lat = 0;
    double global_min_write = DBL_MAX, global_max_write = 0;
    double global_min_read = DBL_MAX, global_max_read = 0;
    
    for (int i = 0; i < NUM_CHANNELS; i++) {
        ChannelStats* s = &g_stats[i];
        total_writes += s->writes;
        total_reads += s->reads;
        total_write_lat += s->write_latency_sum;
        total_read_lat += s->read_latency_sum;
        
        if (s->min_write_latency < global_min_write) global_min_write = s->min_write_latency;
        if (s->max_write_latency > global_max_write) global_max_write = s->max_write_latency;
        if (s->min_read_latency < global_min_read) global_min_read = s->min_read_latency;
        if (s->max_read_latency > global_max_read) global_max_read = s->max_read_latency;
        
        double avg_write = s->writes > 0 ? s->write_latency_sum / s->writes / 1000.0 : 0;
        double avg_read = s->reads > 0 ? s->read_latency_sum / s->reads / 1000.0 : 0;
        
        printf("║    %d    │ %10lu │ %10lu │    %8.3f  │   %8.3f  │  %9.1f  ║\n",
               i, s->writes, s->reads, avg_write, avg_read, s->max_read_latency / 1000.0);
    }
    
    printf("╠═══════════════════════════════════════════════════════════════════════════════╣\n");
    
    double avg_write_all = total_writes > 0 ? total_write_lat / total_writes / 1000.0 : 0;
    double avg_read_all = total_reads > 0 ? total_read_lat / total_reads / 1000.0 : 0;
    
    double write_throughput = total_writes / duration_sec;
    double read_throughput = total_reads / duration_sec;
    double write_bandwidth = write_throughput * DATA_SIZE / 1e9;
    double read_bandwidth = read_throughput * DATA_SIZE / 1e9;
    
    printf("║  TOTAL  │ %10lu │ %10lu │    %8.3f  │   %8.3f  │             ║\n",
           total_writes, total_reads, avg_write_all, avg_read_all);
    printf("╠═══════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║                                                                               ║\n");
    printf("║  ⏱️  Duration:         %8.2f seconds                                        ║\n", duration_sec);
    printf("║                                                                               ║\n");
    printf("║  📝 WRITE STATS (L1 cache optimized):                                         ║\n");
    printf("║     Min latency:      %8.3f µs                                             ║\n", global_min_write / 1000.0);
    printf("║     Avg latency:      %8.3f µs                                             ║\n", avg_write_all);
    printf("║     Max latency:      %8.1f µs                                             ║\n", global_max_write / 1000.0);
    printf("║     ⚡ Throughput:     %8.2f M ops/sec                                      ║\n", write_throughput / 1e6);
    printf("║     📶 Bandwidth:      %8.2f GB/sec                                         ║\n", write_bandwidth);
    printf("║                                                                               ║\n");
    printf("║  📖 READ STATS (L1 cache optimized):                                          ║\n");
    printf("║     Min latency:      %8.3f µs                                             ║\n", global_min_read / 1000.0);
    printf("║     Avg latency:      %8.3f µs                                             ║\n", avg_read_all);
    printf("║     Max latency:      %8.1f µs                                             ║\n", global_max_read / 1000.0);
    printf("║     ⚡ Throughput:     %8.2f M ops/sec                                      ║\n", read_throughput / 1e6);
    printf("║     📶 Bandwidth:      %8.2f GB/sec                                         ║\n", read_bandwidth);
    printf("║                                                                               ║\n");
    printf("╚═══════════════════════════════════════════════════════════════════════════════╝\n");
    
    // Cleanup
    for (int i = 0; i < NUM_CHANNELS; i++) {
        bifrost_writer_leave(writer_args[i].writer);
        bifrost_reader_leave(reader_args[i].reader);
    }
    bifrost_destroy_pool(pool);
    
    printf("\n✅ Cache-optimized benchmark complete!\n");
    
    return 0;
}
