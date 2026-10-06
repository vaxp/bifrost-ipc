/**
 * @file bifrost_e2e_demo.c
 * @brief Bifrost End-to-End Demo
 * 
 * Complete demonstration of Bifrost architecture:
 * 
 * ┌─────────────────────────────────────────────────────────────────────────┐
 * │                      End-to-End Flow on Localhost                       │
 * ├─────────────────────────────────────────────────────────────────────────┤
 * │                                                                         │
 * │  [Writer Thread]                      [Reader Thread]                   │
 * │       ↓                                     ↑                           │
 * │    Write to                           Read from                         │
 * │   LOCAL SHM                          REMOTE SHM                         │
 * │       ↓                                     ↑                           │
 * │  ┌─────────┐                         ┌─────────┐                        │
 * │  │ Gateway │ ═══ NETWORK (lo) ════▶  │ Gateway │                        │
 * │  │ SHM→NET │                         │ NET→SHM │                        │
 * │  └─────────┘                         └─────────┘                        │
 * │                                                                         │
 * └─────────────────────────────────────────────────────────────────────────┘
 * 
 * Build: cmake .. && make bifrost_e2e_demo
 * Run:   sudo ./bifrost_e2e_demo
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>

#include "bifrost.h"
#include "bifrost_gateway.h"
#include <sched.h>
#include <pthread.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Tuning & Architecture
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════════════
 * CPU Utilities
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Pause instruction to prevent pipeline flush and reduce power */
static inline void cpu_relax(void) {
    #if defined(__x86_64__) || defined(__i386__)
        __asm__ volatile("pause" ::: "memory");
    #else
        sched_yield();
    #endif
}

/* Sleep for minimal time to let Kernel process SoftIRQs */
static inline void brief_sleep(void) {
    struct timespec ts = {0, 1000}; /* 1 microsecond */
    nanosleep(&ts, NULL);
}



/* ═══════════════════════════════════════════════════════════════════════════
 * Configuration
 * ═══════════════════════════════════════════════════════════════════════════ */

#define LOCAL_POOL      "bifrost_local_pool"
#define REMOTE_POOL     "bifrost_remote_pool"
#define WRITER_NAME     "data_producer"
#define READER_NAME     "data_consumer"

/* ═══════════════════════════════════════════════════════════════════════════
 * Shared State
 * ═══════════════════════════════════════════════════════════════════════════ */

static volatile int g_running = 1;

/* Stats */
static _Atomic uint64_t g_messages_written = 0;
static _Atomic uint64_t g_messages_read = 0;
static _Atomic uint64_t g_latency_sum = 0;
static _Atomic uint64_t g_latency_min = UINT64_MAX;
static _Atomic uint64_t g_latency_max = 0;

/* ═══════════════════════════════════════════════════════════════════════════
 * Message Structure
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct __attribute__((packed)) {
    uint64_t    sequence;
    uint64_t    timestamp_ns;
    char        data[48];
} demo_message_t;



/* ═══════════════════════════════════════════════════════════════════════════
 * Signal Handler
 * ═══════════════════════════════════════════════════════════════════════════ */

static bifrost_gateway_t* g_gw_sender = NULL;
static bifrost_gateway_t* g_gw_receiver = NULL;

static void signal_handler(int sig) {
    (void)sig;
    printf("\n⏹️  Stopping demo...\n");
    g_running = 0;
    if (g_gw_sender) bifrost_gateway_stop(g_gw_sender);
    if (g_gw_receiver) bifrost_gateway_stop(g_gw_receiver);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Writer Thread - Writes to LOCAL SHM
 * ═══════════════════════════════════════════════════════════════════════════ */

static void* writer_thread(void* arg) {
    (void)arg;
    
    bifrost_writer_t* writer = bifrost_join_as_writer(LOCAL_POOL, WRITER_NAME);
    if (!writer) {
        fprintf(stderr, "ERROR: Writer cannot join pool\n");
        return NULL;
    }
    
    printf("   📝 Writer started (writing to %s)\n", LOCAL_POOL);
    
    uint64_t seq = 0;
    while (g_running) {
        demo_message_t* msg = bifrost_writer_alloc(writer, sizeof(demo_message_t));
        if (msg) {
            msg->sequence = ++seq;
            msg->timestamp_ns = get_time_ns();
            snprintf(msg->data, sizeof(msg->data), "Message #%lu from Bifrost", seq);
            
            bifrost_writer_publish(writer);
            atomic_fetch_add(&g_messages_written, 1);
        } else {
            /* Pool busy/full - Critical to YIELD here to let Reader/Gateway catch up */
            sched_yield();
            continue;
        }
        
        /* Full throttle - no sleeping */
    }
    
    bifrost_writer_leave(writer);
    printf("   📝 Writer stopped (wrote %lu messages)\n", atomic_load(&g_messages_written));
    
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Reader Thread - Reads from REMOTE SHM
 * ═══════════════════════════════════════════════════════════════════════════ */

static void* reader_thread(void* arg) {
    (void)arg;
    
    /* Wait for pool to be created */
    usleep(500000);
    
    bifrost_reader_t* reader = bifrost_join_as_writer(REMOTE_POOL, READER_NAME) ? NULL : bifrost_join_as_reader(REMOTE_POOL, READER_NAME); /* small hack to wait? no just use usleep */
    /* Wait, the function is: */
    reader = bifrost_join_as_reader(REMOTE_POOL, READER_NAME);
    if (!reader) {
        fprintf(stderr, "ERROR: Reader cannot join pool\n");
        return NULL;
    }
    
    printf("   📖 Reader started (reading from %s)\n", REMOTE_POOL);
    
    uint64_t last_seq = 0;
    while (g_running) {
        bifrost_writer_info_t writers[8];
        uint32_t num = bifrost_reader_list_writers(reader, writers, 8);
        
        int got_data = 0;
        for (uint32_t i = 0; i < num; i++) {
            size_t size;
            demo_message_t* msg = bifrost_reader_get_by_id(reader, writers[i].id, &size);
            if (msg && size >= sizeof(demo_message_t)) {
                if (msg->sequence > last_seq) {
                    uint64_t now = get_time_ns();
                    uint64_t latency = now - msg->timestamp_ns;
                    
                    atomic_fetch_add(&g_messages_read, 1);
                    atomic_fetch_add(&g_latency_sum, latency);
                    
                    uint64_t min = atomic_load(&g_latency_min);
                    while (latency < min) {
                        atomic_compare_exchange_weak(&g_latency_min, &min, latency);
                    }
                    
                    uint64_t max = atomic_load(&g_latency_max);
                    while (latency > max) {
                        atomic_compare_exchange_weak(&g_latency_max, &max, latency);
                    }
                    
                    last_seq = msg->sequence;
                    got_data = 1;
                }
            }
        }
        
        if (!got_data) sched_yield();
    }
    
    bifrost_reader_leave(reader);
    printf("   📖 Reader stopped (read %lu messages)\n", atomic_load(&g_messages_read));
    
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Gateway Threads
 * ═══════════════════════════════════════════════════════════════════════════ */

static void* gateway_sender_thread(void* arg) {
    (void)arg;
    
    usleep(200000);  /* Wait for pool */
    
    bifrost_gateway_config_t cfg = bifrost_gateway_config_default();
    cfg.interface = "lo";
    cfg.remote_mac = "00:00:00:00:00:00";
    cfg.remote_ip = "127.0.0.1";
    cfg.pool_name = LOCAL_POOL;
    cfg.mode = BIFROST_GATEWAY_SHM_TO_NET;
    cfg.port = 19850;
    cfg.batch_size = 1; /* Immediate send for lowest latency */
    
    g_gw_sender = bifrost_gateway_create(&cfg);
    if (!g_gw_sender) {
        fprintf(stderr, "ERROR: Cannot create sender gateway\n");
        return NULL;
    }
    
    printf("   🌐 Gateway SHM→NET started\n");
    
    while (g_running) {
        int processed = bifrost_gateway_process(g_gw_sender);
        if (processed == 0) brief_sleep();
    }
    
    printf("   🌐 Gateway SHM→NET stopped\n");
    
    return NULL;
}

static void* gateway_receiver_thread(void* arg) {
    (void)arg;
    
    usleep(200000);  /* Wait for pool */
    
    bifrost_gateway_config_t cfg = bifrost_gateway_config_default();
    cfg.interface = "lo";
    cfg.remote_mac = "00:00:00:00:00:00";
    cfg.remote_ip = "127.0.0.1";
    cfg.pool_name = REMOTE_POOL;
    cfg.mode = BIFROST_GATEWAY_NET_TO_SHM;
    cfg.port = 19850;
    /* Batch size doesn't matter for recv as much, but keep it low */
    cfg.batch_size = 64; 
    
    g_gw_receiver = bifrost_gateway_create(&cfg);
    if (!g_gw_receiver) {
        fprintf(stderr, "ERROR: Cannot create receiver gateway\n");
        return NULL;
    }
    
    printf("   🌐 Gateway NET→SHM started\n");
    
    while (g_running) {
        int processed = bifrost_gateway_process(g_gw_receiver);
        if (processed == 0) brief_sleep();
    }
    
    printf("   🌐 Gateway NET→SHM stopped\n");
    
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("╔═══════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║   🔄 Bifrost End-to-End Demo                                               ║\n");
    printf("║   Full pipeline: Writer → SHM → Gateway → Network → Gateway → SHM → Reader   ║\n");
    printf("╚═══════════════════════════════════════════════════════════════════════════════╝\n\n");
    
    if (geteuid() != 0) {
        fprintf(stderr, "ERROR: Must run as root for raw socket access\n");
        return 1;
    }
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* Create pools */
    printf("📦 Creating pools...\n");
    bifrost_pool_config_t pool_cfg;
    bifrost_config_init(&pool_cfg);
    
    bifrost_pool_t* local_pool = bifrost_create_pool(LOCAL_POOL, &pool_cfg);
    bifrost_pool_t* remote_pool = bifrost_create_pool(REMOTE_POOL, &pool_cfg);
    
    if (!local_pool || !remote_pool) {
        fprintf(stderr, "ERROR: Cannot create pools\n");
        return 1;
    }
    
    printf("   ✅ %s created\n", LOCAL_POOL);
    printf("   ✅ %s created\n\n", REMOTE_POOL);
    
    /* Start threads */
    printf("🚀 Starting components...\n");
    
    pthread_t tid_writer, tid_reader, tid_gw_send, tid_gw_recv;
    
    pthread_create(&tid_gw_recv, NULL, gateway_receiver_thread, NULL);
    pthread_create(&tid_gw_send, NULL, gateway_sender_thread, NULL);
    pthread_create(&tid_writer, NULL, writer_thread, NULL);
    pthread_create(&tid_reader, NULL, reader_thread, NULL);
    
    usleep(500000);  /* Let everything start */
    
    printf("\n✅ All components running!\n");
    printf("   Press Ctrl+C to stop...\n\n");
    
    /* Stats loop */
    uint64_t last_read = 0;
    uint64_t start_time = get_time_ns();
    
    while (g_running) {
        sleep(1);
        
        uint64_t written = atomic_load(&g_messages_written);
        uint64_t read = atomic_load(&g_messages_read);
        uint64_t lat_sum = atomic_load(&g_latency_sum);
        uint64_t lat_min = atomic_load(&g_latency_min);
        uint64_t lat_max = atomic_load(&g_latency_max);
        
        double avg_lat = read > 0 ? (double)lat_sum / read / 1000.0 : 0;
        double min_lat = lat_min < UINT64_MAX ? lat_min / 1000.0 : 0;
        double max_lat = lat_max / 1000.0;
        
        printf("\r📊 Written: %lu | Read: %lu | Rate: %lu/s | Lat: min=%.1f avg=%.1f max=%.1f µs    ",
               written, read,
               read - last_read,
               min_lat, avg_lat, max_lat);
        fflush(stdout);
        
        last_read = read;
    }
    
    /* Cleanup */
    printf("\n\n🧹 Stopping threads...\n");
    
    pthread_join(tid_writer, NULL);
    pthread_join(tid_reader, NULL);
    pthread_join(tid_gw_send, NULL);
    pthread_join(tid_gw_recv, NULL);
    
    /* Final stats */
    double duration = (get_time_ns() - start_time) / 1e9;
    uint64_t written = atomic_load(&g_messages_written);
    uint64_t read = atomic_load(&g_messages_read);
    uint64_t lat_sum = atomic_load(&g_latency_sum);
    uint64_t lat_min = atomic_load(&g_latency_min);
    uint64_t lat_max = atomic_load(&g_latency_max);
    
    bifrost_gateway_stats_t send_stats = {0}, recv_stats = {0};
    if (g_gw_sender) bifrost_gateway_get_stats(g_gw_sender, &send_stats);
    if (g_gw_receiver) bifrost_gateway_get_stats(g_gw_receiver, &recv_stats);
    
    if (g_gw_sender) bifrost_gateway_destroy(g_gw_sender);
    if (g_gw_receiver) bifrost_gateway_destroy(g_gw_receiver);
    
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║   📊 FINAL STATISTICS                                                         ║\n");
    printf("╠═══════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║   Duration:      %.2f seconds                                                ║\n", duration);
    printf("║   Written:       %lu messages                                          ║\n", written);
    printf("║   Read:          %lu messages                                          ║\n", read);
    printf("║   GW Sent:       %lu packets                                           ║\n", send_stats.shm_to_net_count);
    printf("║   GW Recv:       %lu packets                                           ║\n", recv_stats.net_to_shm_count);
    printf("║   GW Errors:     %lu                                                   ║\n", recv_stats.errors + send_stats.errors);
    printf("║   Delivery:      %.1f%%                                                       ║\n", 
           written > 0 ? 100.0 * read / written : 0);
    printf("╠═══════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║   ⚡ LATENCY (End-to-End):                                                     ║\n");
    printf("║      Min:        %.1f µs                                                      ║\n", 
           lat_min < UINT64_MAX ? lat_min / 1000.0 : 0);
    printf("║      Avg:        %.1f µs                                                      ║\n", 
           read > 0 ? (double)lat_sum / read / 1000.0 : 0);
    printf("║      Max:        %.1f µs                                                      ║\n", lat_max / 1000.0);
    printf("╚═══════════════════════════════════════════════════════════════════════════════╝\n");
    
    bifrost_destroy_pool(local_pool);
    bifrost_destroy_pool(remote_pool);
    
    printf("\n👋 Demo complete!\n");
    
    return 0;
}
