/**
 * Bifrost Performance Benchmark
 * 
 * Measures latency and throughput of the Bifrost lock-free shared memory library.
 * 
 * This benchmark tests realistic usage patterns:
 * - Writer allocates ONCE, then updates in-place and re-publishes
 * - Reader polls for new versions
 */

#define _GNU_SOURCE
#include <bifrost.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdint.h>
#include <math.h>

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define DATA_SIZE       1024            /* 1 KB per message */
#define NUM_CHANNELS    4               /* Number of writer/reader pairs */
#define ITERATIONS      100000          /* Operations per channel */
#define WARMUP_ITERS    1000            /* Warmup iterations */

#define POOL_NAME       "benchmark_pool"

/* ============================================================================
 * Types
 * ============================================================================ */

typedef struct {
    _Atomic uint64_t sequence;
    _Atomic uint64_t timestamp_ns;
    _Atomic uint32_t ready;
    char payload[DATA_SIZE - 24];
} benchmark_data_t;

typedef struct {
    int id;
    char writer_name[64];
    benchmark_data_t* shared_data;  /* Pointer to shared memory */
    _Atomic int* stop_flag;
} thread_args_t;

typedef struct {
    uint64_t successful;
    double total_latency_ns;
    double min_latency_ns;
    double max_latency_ns;
    double* latencies;
    size_t latency_count;
} stats_t;

static stats_t writer_stats[NUM_CHANNELS];
static stats_t reader_stats[NUM_CHANNELS];
static pthread_barrier_t start_barrier;
static _Atomic int stop_flag = 0;

/* ============================================================================
 * Timing Utilities
 * ============================================================================ */

static inline double get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static inline uint64_t get_time_ns_u64(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ============================================================================
 * Statistics Helpers
 * ============================================================================ */

static int compare_double(const void* a, const void* b) {
    double da = *(const double*)a;
    double db = *(const double*)b;
    return (da > db) - (da < db);
}

static double percentile(double* arr, size_t n, double p) {
    if (n == 0) return 0;
    qsort(arr, n, sizeof(double), compare_double);
    size_t idx = (size_t)(p / 100.0 * (n - 1));
    return arr[idx];
}

static void stats_init(stats_t* s) {
    s->successful = 0;
    s->total_latency_ns = 0;
    s->min_latency_ns = 1e18;
    s->max_latency_ns = 0;
    s->latencies = malloc(ITERATIONS * sizeof(double));
    s->latency_count = 0;
}

static void stats_record(stats_t* s, double latency_ns) {
    s->total_latency_ns += latency_ns;
    if (latency_ns < s->min_latency_ns) s->min_latency_ns = latency_ns;
    if (latency_ns > s->max_latency_ns) s->max_latency_ns = latency_ns;
    if (s->latency_count < (size_t)ITERATIONS) {
        s->latencies[s->latency_count++] = latency_ns;
    }
    s->successful++;
}

static void stats_free(stats_t* s) {
    if (s->latencies) {
        free(s->latencies);
        s->latencies = NULL;
    }
}

/* ============================================================================
 * Writer Thread (Daemon)
 * 
 * Allocates space ONCE, then updates in-place and publishes repeatedly.
 * This simulates a daemon that updates its data periodically.
 * ============================================================================ */

static void* writer_thread(void* arg) {
    thread_args_t* args = (thread_args_t*)arg;
    int id = args->id;
    
    /* Join pool as writer */
    bifrost_writer_t* writer = bifrost_join_as_writer(POOL_NAME, args->writer_name);
    if (!writer) {
        fprintf(stderr, "Writer %d: Failed to join: %s\n", id, 
                bifrost_error_string(bifrost_get_last_error()));
        return NULL;
    }
    
    stats_init(&writer_stats[id]);
    
    /* Allocate space ONCE at the start */
    benchmark_data_t* data = bifrost_writer_alloc(writer, sizeof(benchmark_data_t));
    if (!data) {
        fprintf(stderr, "Writer %d: Failed to allocate\n", id);
        bifrost_writer_leave(writer);
        return NULL;
    }
    
    /* Share pointer with reader thread */
    args->shared_data = data;
    
    /* Initialize data */
    atomic_store(&data->sequence, 0);
    atomic_store(&data->timestamp_ns, 0);
    atomic_store(&data->ready, 1);
    
    pthread_barrier_wait(&start_barrier);
    
    for (uint64_t seq = 0; seq < ITERATIONS && !atomic_load(args->stop_flag); seq++) {
        double start = get_time_ns();
        
        /* Update data in-place (no allocation, no memcpy to self) */
        atomic_store(&data->sequence, seq);
        atomic_store(&data->timestamp_ns, get_time_ns_u64());
        
        /* Fill payload to simulate work */
        data->payload[0] = (char)(seq & 0xFF);
        
        /* Set command and publish */
        bifrost_writer_set_command(writer, (uint32_t)(seq % 100));
        bifrost_writer_publish(writer);
        
        double end = get_time_ns();
        double latency = end - start;
        
        /* Skip warmup in stats */
        if (seq >= WARMUP_ITERS) {
            stats_record(&writer_stats[id], latency);
        }
    }
    
    bifrost_writer_leave(writer);
    return NULL;
}

/* ============================================================================
 * Reader Thread (Desktop Environment)
 * 
 * Polls for new data using version number, reads directly from shared memory.
 * ============================================================================ */

static void* reader_thread(void* arg) {
    thread_args_t* args = (thread_args_t*)arg;
    int id = args->id;
    
    /* Join pool as reader */
    char reader_name[64];
    snprintf(reader_name, sizeof(reader_name), "reader_%d", id);
    
    bifrost_reader_t* reader = bifrost_join_as_reader(POOL_NAME, reader_name);
    if (!reader) {
        fprintf(stderr, "Reader %d: Failed to join: %s\n", id, 
                bifrost_error_string(bifrost_get_last_error()));
        return NULL;
    }
    
    stats_init(&reader_stats[id]);
    
    pthread_barrier_wait(&start_barrier);
    
    uint64_t last_version = 0;
    uint64_t reads = 0;
    
    while (reads < ITERATIONS && !atomic_load(args->stop_flag)) {
        /* Check version to detect new data */
        uint64_t version = bifrost_reader_get_version(reader, args->writer_name);
        
        if (version > last_version) {
            double start = get_time_ns();
            
            /* Read data directly - NO memcpy */
            size_t size;
            benchmark_data_t* data = bifrost_reader_get(reader, args->writer_name, &size);
            
            if (data) {
                /* Access the data to ensure it's really read */
                uint64_t seq = atomic_load(&data->sequence);
                uint64_t ts = atomic_load(&data->timestamp_ns);
                (void)seq;  /* Prevent optimization */
                
                /* Calculate end-to-end latency */
                uint64_t now = get_time_ns_u64();
                double e2e_latency = (double)(now - ts);
                (void)start;  /* We measure e2e, not read latency */
                
                /* Record read operation latency */
                if (reads >= WARMUP_ITERS) {
                    stats_record(&reader_stats[id], e2e_latency);
                }
                
                last_version = version;
                reads++;
            }
        } else {
            /* Spin-wait with pause instruction */
            __asm__ __volatile__("pause" ::: "memory");
        }
    }
    
    bifrost_reader_leave(reader);
    return NULL;
}

/* ============================================================================
 * Print Results
 * ============================================================================ */

static void print_stats_table(const char* title, stats_t* stats_arr, int count) {
    printf("\n%s:\n", title);
    printf("┌─────────┬───────────┬──────────┬──────────┬──────────┬──────────┐\n");
    printf("│ Channel │ Successful│ Avg (µs) │ Min (µs) │ P99 (µs) │ Max (µs) │\n");
    printf("├─────────┼───────────┼──────────┼──────────┼──────────┼──────────┤\n");
    
    uint64_t total = 0;
    double total_lat = 0;
    double global_min = 1e18, global_max = 0;
    
    for (int i = 0; i < count; i++) {
        stats_t* s = &stats_arr[i];
        total += s->successful;
        total_lat += s->total_latency_ns;
        if (s->min_latency_ns < global_min) global_min = s->min_latency_ns;
        if (s->max_latency_ns > global_max) global_max = s->max_latency_ns;
        
        double avg = s->successful > 0 ? s->total_latency_ns / s->successful / 1000.0 : 0;
        double min = s->min_latency_ns / 1000.0;
        double p99 = percentile(s->latencies, s->latency_count, 99) / 1000.0;
        double max = s->max_latency_ns / 1000.0;
        
        printf("│    %d    │  %8lu │ %8.3f │ %8.3f │ %8.3f │ %8.3f │\n",
               i, s->successful, avg, min, p99, max);
    }
    
    printf("└─────────┴───────────┴──────────┴──────────┴──────────┴──────────┘\n");
    
    if (total > 0) {
        printf("   Total: %lu ops, Avg: %.3f µs, Min: %.3f µs, Max: %.3f µs\n",
               total, total_lat / total / 1000.0, global_min / 1000.0, global_max / 1000.0);
    }
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║      Bifrost Lock-Free Shared Memory Benchmark             ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n\n");
    
    printf("📋 Configuration:\n");
    printf("   Channels:      %d\n", NUM_CHANNELS);
    printf("   Iterations:    %d per channel\n", ITERATIONS);
    printf("   Data size:     %zu bytes per message\n", sizeof(benchmark_data_t));
    printf("   Warmup:        %d iterations\n", WARMUP_ITERS);
    printf("   Library:       Bifrost %s\n\n", bifrost_version());
    
    /* Clean up any existing pool */
    if (bifrost_pool_exists(POOL_NAME)) {
        printf("Cleaning up existing pool...\n");
        bifrost_pool_t* old = bifrost_open_pool(POOL_NAME);
        if (old) bifrost_destroy_pool(old);
    }
    
    /* Create pool */
    printf("Creating benchmark pool...\n");
    bifrost_pool_config_t config;
    bifrost_config_init(&config);
    config.initial_size = 4 * 1024 * 1024;  /* 4 MB - enough for 4 writers */
    config.max_participants = NUM_CHANNELS * 2 + 4;
    
    bifrost_pool_t* pool = bifrost_create_pool(POOL_NAME, &config);
    if (!pool) {
        fprintf(stderr, "Failed to create pool: %s\n", bifrost_error_string(bifrost_get_last_error()));
        return 1;
    }
    
    printf("Pool created: %zu KB\n\n", config.initial_size / 1024);
    
    /* Initialize synchronization */
    pthread_barrier_init(&start_barrier, NULL, NUM_CHANNELS * 2);
    atomic_store(&stop_flag, 0);
    
    /* Prepare thread arguments */
    pthread_t writers[NUM_CHANNELS], readers[NUM_CHANNELS];
    thread_args_t w_args[NUM_CHANNELS], r_args[NUM_CHANNELS];
    
    for (int i = 0; i < NUM_CHANNELS; i++) {
        w_args[i].id = i;
        snprintf(w_args[i].writer_name, sizeof(w_args[i].writer_name), "writer_%d", i);
        w_args[i].shared_data = NULL;
        w_args[i].stop_flag = &stop_flag;
        
        r_args[i].id = i;
        snprintf(r_args[i].writer_name, sizeof(r_args[i].writer_name), "writer_%d", i);
        r_args[i].shared_data = NULL;
        r_args[i].stop_flag = &stop_flag;
    }
    
    printf("🚀 Starting benchmark...\n");
    double start_time = get_time_ns();
    
    /* Start threads */
    for (int i = 0; i < NUM_CHANNELS; i++) {
        pthread_create(&writers[i], NULL, writer_thread, &w_args[i]);
        pthread_create(&readers[i], NULL, reader_thread, &r_args[i]);
    }
    
    /* Wait for all threads */
    for (int i = 0; i < NUM_CHANNELS; i++) {
        pthread_join(writers[i], NULL);
    }
    
    /* Signal readers to stop */
    atomic_store(&stop_flag, 1);
    
    for (int i = 0; i < NUM_CHANNELS; i++) {
        pthread_join(readers[i], NULL);
    }
    
    double end_time = get_time_ns();
    double duration_sec = (end_time - start_time) / 1e9;
    
    /* Collect and print results */
    print_stats_table("📝 WRITER STATS (update + publish latency)", writer_stats, NUM_CHANNELS);
    print_stats_table("📖 READER STATS (end-to-end latency: write → read)", reader_stats, NUM_CHANNELS);
    
    /* Aggregate results */
    uint64_t total_writes = 0, total_reads = 0;
    double total_write_lat = 0, total_read_lat = 0;
    double write_min = 1e18, write_max = 0;
    double read_min = 1e18, read_max = 0;
    
    for (int i = 0; i < NUM_CHANNELS; i++) {
        total_writes += writer_stats[i].successful;
        total_reads += reader_stats[i].successful;
        total_write_lat += writer_stats[i].total_latency_ns;
        total_read_lat += reader_stats[i].total_latency_ns;
        
        if (writer_stats[i].min_latency_ns < write_min) 
            write_min = writer_stats[i].min_latency_ns;
        if (writer_stats[i].max_latency_ns > write_max) 
            write_max = writer_stats[i].max_latency_ns;
        if (reader_stats[i].min_latency_ns < read_min) 
            read_min = reader_stats[i].min_latency_ns;
        if (reader_stats[i].max_latency_ns > read_max) 
            read_max = reader_stats[i].max_latency_ns;
    }
    
    double write_throughput = total_writes / duration_sec;
    double read_throughput = total_reads / duration_sec;
    double write_bandwidth = write_throughput * sizeof(benchmark_data_t) / 1e9;
    double read_bandwidth = read_throughput * sizeof(benchmark_data_t) / 1e9;
    
    /* Get final pool stats */
    bifrost_pool_stats_t pool_stats;
    bifrost_get_pool_stats(pool, &pool_stats);
    
    printf("\n╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║                   📊 AGGREGATE RESULTS                        ║\n");
    printf("╠═══════════════════════════════════════════════════════════════╣\n");
    printf("║ Test Configuration:                                           ║\n");
    printf("║   Channels:            %-6d                                  ║\n", NUM_CHANNELS);
    printf("║   Duration:            %-6.2f seconds                         ║\n", duration_sec);
    printf("╠═══════════════════════════════════════════════════════════════╣\n");
    printf("║ WRITE Performance:                                            ║\n");
    printf("║   Operations:          %-10lu                              ║\n", total_writes);
    printf("║   Avg Latency:         %-8.3f µs                            ║\n", 
           total_writes > 0 ? total_write_lat / total_writes / 1000.0 : 0);
    printf("║   Min Latency:         %-8.3f µs                            ║\n", write_min / 1000.0);
    printf("║   Max Latency:         %-8.3f µs                            ║\n", write_max / 1000.0);
    printf("║   ⚡ Throughput:        %-10.0f ops/sec                     ║\n", write_throughput);
    printf("║   📶 Bandwidth:         %-6.2f GB/s                           ║\n", write_bandwidth);
    printf("╠═══════════════════════════════════════════════════════════════╣\n");
    printf("║ READ Performance (End-to-End):                                ║\n");
    printf("║   Operations:          %-10lu                              ║\n", total_reads);
    printf("║   Avg E2E Latency:     %-8.3f µs                            ║\n", 
           total_reads > 0 ? total_read_lat / total_reads / 1000.0 : 0);
    printf("║   Min E2E Latency:     %-8.3f µs                            ║\n", read_min / 1000.0);
    printf("║   Max E2E Latency:     %-8.3f µs                            ║\n", read_max / 1000.0);
    printf("║   ⚡ Throughput:        %-10.0f ops/sec                     ║\n", read_throughput);
    printf("║   📶 Bandwidth:         %-6.2f GB/s                           ║\n", read_bandwidth);
    printf("╠═══════════════════════════════════════════════════════════════╣\n");
    printf("║ Pool Statistics:                                              ║\n");
    printf("║   Total Publishes:     %-10lu                              ║\n", pool_stats.total_writes);
    printf("║   Total Reads:         %-10lu                              ║\n", pool_stats.total_reads);
    printf("║   Memory Used:         %-6zu KB                              ║\n", pool_stats.used_size / 1024);
    printf("╚═══════════════════════════════════════════════════════════════╝\n");
    
    /* Cleanup */
    for (int i = 0; i < NUM_CHANNELS; i++) {
        stats_free(&writer_stats[i]);
        stats_free(&reader_stats[i]);
    }
    
    pthread_barrier_destroy(&start_barrier);
    bifrost_destroy_pool(pool);
    
    printf("\n✅ Benchmark complete.\n");
    
    return 0;
}
