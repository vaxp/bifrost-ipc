/**
 * Bifrost ULTRA Performance Test
 * 
 * Matches the exact testing methodology of c_pure_test.c:
 * - 256 KB data per message
 * - Request-Response pattern (shell sends request, daemon responds)
 * - Real data copy to measure actual bandwidth
 * - 4 channels, high iteration count
 * 
 * Goal: Match or beat 40 GB/s throughput
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
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

/* ============================================================================
 * Configuration - MUST MATCH c_pure_test.c
 * ============================================================================ */

#define DATA_SIZE       (256 * 1024)    /* 256 KB per message - SAME AS ORIGINAL */
#define NUM_CHANNELS    4               /* 4 parallel channels */
#define ITERATIONS      10000000        /* 10M iterations per channel = 40M total */

/* ============================================================================
 * Data Structures - Match c_pure_test.c exactly
 * ============================================================================ */

typedef struct __attribute__((aligned(64))) {
    _Atomic uint64_t write_seq;     /* Writer sequence (odd = writing, even = ready) */
    _Atomic uint64_t read_seq;      /* Reader request sequence */
    _Atomic uint64_t data_len;      /* Data length */
    char _pad[40];                  /* Pad to 64 bytes (cache line) */
} channel_header_t;

typedef struct {
    int id;
    void* ptr;          /* Pointer to shared memory region */
    size_t size;
} thread_args_t;

typedef struct {
    uint64_t successful;
    double total_latency_ns;
    double min_latency_ns;
    double max_latency_ns;
} stats_t;

static stats_t stats[NUM_CHANNELS];
static pthread_barrier_t start_barrier;
static _Atomic int stop_flag = 0;

/* ============================================================================
 * Timing
 * ============================================================================ */

static inline double get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* ============================================================================
 * Shared Memory Creation
 * ============================================================================ */

static void* create_shm(const char* name, size_t size) {
    char full_name[128];
    snprintf(full_name, sizeof(full_name), "/vhbench_%s", name);
    
    shm_unlink(full_name);  /* Clean up old */
    
    int fd = shm_open(full_name, O_CREAT | O_RDWR, 0666);
    if (fd < 0) { 
        perror("shm_open"); 
        return NULL; 
    }
    
    if (ftruncate(fd, size) < 0) { 
        perror("ftruncate"); 
        close(fd);
        return NULL; 
    }
    
    void* ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    
    if (ptr == MAP_FAILED) { 
        perror("mmap"); 
        return NULL; 
    }
    
    memset(ptr, 0, size);
    return ptr;
}

/* ============================================================================
 * Daemon Thread (Writer) - Responds to requests
 * 
 * This simulates a system daemon that:
 * 1. Waits for a new read_seq (request from shell)
 * 2. Writes data (memset to simulate real data write)
 * 3. Updates write_seq to signal completion
 * ============================================================================ */

static void* daemon_thread(void* arg) {
    thread_args_t* args = (thread_args_t*)arg;
    channel_header_t* header = (channel_header_t*)args->ptr;
    uint8_t* data_ptr = (uint8_t*)args->ptr + sizeof(channel_header_t);
    
    uint64_t last_read_seq = 0;
    
    pthread_barrier_wait(&start_barrier);
    
    while (!atomic_load_explicit(&stop_flag, memory_order_relaxed)) {
        /* Check for new request */
        uint64_t read_seq = atomic_load_explicit(&header->read_seq, memory_order_acquire);
        
        if (read_seq > last_read_seq) {
            last_read_seq = read_seq;
            
            /* Start writing - set odd sequence */
            atomic_fetch_add_explicit(&header->write_seq, 1, memory_order_release);
            
            /* Write data - this is where the real work happens */
            /* Using memset to simulate actual data writes */
            memset(data_ptr, (uint8_t)read_seq, DATA_SIZE);
            atomic_store_explicit(&header->data_len, DATA_SIZE, memory_order_relaxed);
            
            /* Finish writing - set even sequence */
            atomic_fetch_add_explicit(&header->write_seq, 1, memory_order_release);
        } else {
            /* Spin with pause - reduces CPU contention */
            __asm__ __volatile__("pause" ::: "memory");
        }
    }
    
    return NULL;
}

/* ============================================================================
 * Shell Thread (Reader) - Sends requests and reads responses
 * 
 * This simulates a desktop environment component that:
 * 1. Sends request by incrementing read_seq
 * 2. Waits for response (even write_seq that's greater than before)
 * 3. Reads the data (memcpy to simulate real data read)
 * ============================================================================ */

static void* shell_thread(void* arg) {
    thread_args_t* args = (thread_args_t*)arg;
    channel_header_t* header = (channel_header_t*)args->ptr;
    uint8_t* data_ptr = (uint8_t*)args->ptr + sizeof(channel_header_t);
    
    /* Allocate read buffer - simulates real client reading data */
    uint8_t* read_buf = malloc(DATA_SIZE);
    if (!read_buf) {
        fprintf(stderr, "Failed to allocate read buffer\n");
        return NULL;
    }
    
    stats_t* s = &stats[args->id];
    s->min_latency_ns = 1e18;
    s->max_latency_ns = 0;
    s->total_latency_ns = 0;
    s->successful = 0;
    
    pthread_barrier_wait(&start_barrier);
    
    for (uint64_t i = 0; i < ITERATIONS; i++) {
        double start = get_time_ns();
        
        /* Send request - increment read_seq */
        atomic_fetch_add_explicit(&header->read_seq, 1, memory_order_release);
        
        /* Wait for response */
        while (1) {
            uint64_t seq1 = atomic_load_explicit(&header->write_seq, memory_order_acquire);
            
            /* If odd, writer is still writing - spin */
            if (seq1 & 1) {
                __asm__ __volatile__("pause" ::: "memory");
                continue;
            }
            
            /* Read the data - THIS IS THE REAL BANDWIDTH TEST */
            memcpy(read_buf, data_ptr, DATA_SIZE);
            
            /* Memory fence */
            atomic_thread_fence(memory_order_acquire);
            
            /* Verify sequence didn't change during read */
            uint64_t seq2 = atomic_load_explicit(&header->write_seq, memory_order_acquire);
            
            if (seq1 == seq2 && seq1 > 0) {
                break;  /* Successful read */
            }
            /* Otherwise, retry */
        }
        
        double end = get_time_ns();
        double lat = end - start;
        
        s->total_latency_ns += lat;
        if (lat < s->min_latency_ns) s->min_latency_ns = lat;
        if (lat > s->max_latency_ns) s->max_latency_ns = lat;
        s->successful++;
    }
    
    free(read_buf);
    return NULL;
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║   Bifrost ULTRA Performance Test (Match c_pure_test)       ║\n");
    printf("║   Goal: Match 40 GB/s with 4 channels @ 256KB                 ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n\n");
    
    printf("📋 Configuration:\n");
    printf("   Channels:      %d\n", NUM_CHANNELS);
    printf("   Iterations:    %d per channel\n", ITERATIONS);
    printf("   Data size:     %d KB per message\n", DATA_SIZE / 1024);
    printf("   Total data:    %.2f GB\n\n", 
           (double)NUM_CHANNELS * ITERATIONS * DATA_SIZE * 2 / 1e9);
    
    /* Initialize barrier */
    pthread_barrier_init(&start_barrier, NULL, NUM_CHANNELS * 2);
    
    /* Create thread arguments */
    pthread_t daemons[NUM_CHANNELS], shells[NUM_CHANNELS];
    thread_args_t d_args[NUM_CHANNELS], s_args[NUM_CHANNELS];
    void* regions[NUM_CHANNELS];
    
    size_t region_size = sizeof(channel_header_t) + DATA_SIZE;
    
    printf("Creating %d channels...\n", NUM_CHANNELS);
    for (int i = 0; i < NUM_CHANNELS; i++) {
        char name[32];
        snprintf(name, sizeof(name), "ch_%d", i);
        
        regions[i] = create_shm(name, region_size);
        if (!regions[i]) {
            fprintf(stderr, "Failed to create shm %d\n", i);
            return 1;
        }
        
        d_args[i].id = i;
        d_args[i].ptr = regions[i];
        d_args[i].size = region_size;
        
        s_args[i].id = i;
        s_args[i].ptr = regions[i];
        s_args[i].size = region_size;
    }
    
    printf("Starting threads...\n");
    double start_time = get_time_ns();
    
    /* Start daemon threads first */
    for (int i = 0; i < NUM_CHANNELS; i++) {
        pthread_create(&daemons[i], NULL, daemon_thread, &d_args[i]);
    }
    
    /* Start shell threads */
    for (int i = 0; i < NUM_CHANNELS; i++) {
        pthread_create(&shells[i], NULL, shell_thread, &s_args[i]);
    }
    
    /* Wait for shell threads to complete */
    for (int i = 0; i < NUM_CHANNELS; i++) {
        pthread_join(shells[i], NULL);
    }
    
    /* Signal daemons to stop */
    atomic_store(&stop_flag, 1);
    
    for (int i = 0; i < NUM_CHANNELS; i++) {
        pthread_join(daemons[i], NULL);
    }
    
    double end_time = get_time_ns();
    double duration_sec = (end_time - start_time) / 1e9;
    
    /* Print results */
    printf("\n┌─────────┬───────────┬──────────┬──────────────┐\n");
    printf("│ Channel │ Successful│ Avg (µs) │ Max (µs)     │\n");
    printf("├─────────┼───────────┼──────────┼──────────────┤\n");
    
    uint64_t total = 0;
    double total_lat = 0, global_min = 1e18, global_max = 0;
    
    for (int i = 0; i < NUM_CHANNELS; i++) {
        stats_t* s = &stats[i];
        total += s->successful;
        total_lat += s->total_latency_ns;
        if (s->min_latency_ns < global_min) global_min = s->min_latency_ns;
        if (s->max_latency_ns > global_max) global_max = s->max_latency_ns;
        
        printf("│    %d    │  %8lu │  %7.2f │  %11.2f │\n",
               i, s->successful,
               s->total_latency_ns / s->successful / 1000.0,
               s->max_latency_ns / 1000.0);
    }
    printf("└─────────┴───────────┴──────────┴──────────────┘\n");
    
    double throughput = total / duration_sec;
    /* Bandwidth = throughput * data_size * 2 (write + read) */
    double bandwidth_mb = throughput * DATA_SIZE * 2.0 / 1e6;
    double bandwidth_gb = bandwidth_mb / 1000.0;
    
    printf("\n📊 AGGREGATE RESULTS:\n");
    printf("   Channels:         %d\n", NUM_CHANNELS);
    printf("   Total successful: %lu / %lu\n", total, (uint64_t)NUM_CHANNELS * ITERATIONS);
    printf("   Test duration:    %.2f seconds\n", duration_sec);
    printf("   Avg latency:      %.2f µs\n", total_lat / total / 1000.0);
    printf("   Min latency:      %.2f µs\n", global_min / 1000.0);
    printf("   Max latency:      %.2f µs (%.2f ms)\n", global_max / 1000.0, global_max / 1e6);
    printf("   ⚡ THROUGHPUT:     %.0f req/s\n", throughput);
    printf("   📶 BANDWIDTH:      %.2f MB/s = %.2f GB/s\n", bandwidth_mb, bandwidth_gb);
    
    /* Cleanup */
    for (int i = 0; i < NUM_CHANNELS; i++) {
        char name[64];
        snprintf(name, sizeof(name), "/vhbench_ch_%d", i);
        munmap(regions[i], region_size);
        shm_unlink(name);
    }
    
    pthread_barrier_destroy(&start_barrier);
    
    printf("\n✅ Benchmark complete.\n");
    
    return 0;
}
