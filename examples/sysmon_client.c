/**
 * Bifrost System Monitor Client (Status Bar)
 * 
 * Reads system stats from daemon and displays with latency measurement.
 * Tests real-world read latency of Bifrost library.
 */

#define _GNU_SOURCE
#include <bifrost.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include <float.h>

/* ============================================================================
 * Configuration (must match daemon)
 * ============================================================================ */

#define POOL_NAME           "system_monitor"
#define DAEMON_NAME         "sysmon_daemon"
#define CLIENT_NAME         "status_bar"
#define MAX_CORES           16
#define POLL_INTERVAL_US    100000  /* 100ms */

#define CMD_UPDATE          1

#define SYSMON_MAGIC        0x56454E4F

/* ============================================================================
 * Data Structure (must match daemon)
 * ============================================================================ */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    
    float cpu_usage_percent;
    float cpu_cores[MAX_CORES];
    uint32_t core_count;
    
    uint32_t memory_used_mb;
    uint32_t memory_total_mb;
    
    uint64_t uptime_seconds;
    uint64_t update_counter;
    uint64_t timestamp_ns;
} system_state_t;

/* ============================================================================
 * Globals
 * ============================================================================ */

static bifrost_reader_t* g_reader = NULL;
static volatile int g_running = 1;

/* Latency tracking */
static double g_latency_min = DBL_MAX;
static double g_latency_max = 0.0;
static double g_latency_sum = 0.0;
static uint64_t g_latency_count = 0;

/* ============================================================================
 * Signal Handler
 * ============================================================================ */

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ============================================================================
 * Timing
 * ============================================================================ */

static inline double get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000.0 + ts.tv_nsec / 1000.0;
}

/* ============================================================================
 * Display Helpers
 * ============================================================================ */

static void print_bar(float pct, int width) {
    int filled = (int)((pct / 100.0f) * width);
    printf("[");
    for (int i = 0; i < width; i++) {
        if (i < filled) {
            if (pct > 80)
                printf("\033[91m█\033[0m");  /* Red */
            else if (pct > 50)
                printf("\033[93m▓\033[0m");  /* Yellow */
            else
                printf("\033[92m░\033[0m");  /* Green */
        } else {
            printf(" ");
        }
    }
    printf("]");
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║   🖥️  Bifrost Status Bar Client                            ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n\n");
    
    /* Check if pool exists */
    if (!bifrost_pool_exists(POOL_NAME)) {
        printf("❌ Pool '%s' not found! Start the daemon first:\n", POOL_NAME);
        printf("   ./sysmon_daemon\n");
        return 1;
    }
    
    /* Generate unique client name using PID to allow restarting after kills */
    char client_name[64];
    snprintf(client_name, sizeof(client_name), "%s_%d", CLIENT_NAME, getpid());

    /* Join as reader */
    g_reader = bifrost_join_as_reader(POOL_NAME, client_name);
    if (!g_reader) {
        printf("❌ Failed to connect: %s\n", bifrost_error_string(bifrost_get_last_error()));
        return 1;
    }
    
    printf("✅ Connected to pool '%s'\n", POOL_NAME);
    printf("📊 Reading stats... (Ctrl+C to exit)\n\n");
    
    uint64_t last_version = 0;
    int frame = 0;
    
    while (g_running) {
        /* ═══════════════════════════════════════════════════════════════════
         * 📊 BENCHMARK: Measure read latency
         * ═══════════════════════════════════════════════════════════════════ */
        double t_start = get_time_us();
        
        size_t size;
        system_state_t* state = bifrost_reader_get(g_reader, DAEMON_NAME, &size);
        
        double t_end = get_time_us();
        double latency_us = t_end - t_start;
        
        /* Update latency stats */
        if (latency_us < g_latency_min) g_latency_min = latency_us;
        if (latency_us > g_latency_max) g_latency_max = latency_us;
        g_latency_sum += latency_us;
        g_latency_count++;
        double avg_us = g_latency_sum / g_latency_count;
        
        if (state && state->magic == SYSMON_MAGIC) {
            uint64_t version = bifrost_reader_get_version(g_reader, DAEMON_NAME);
            
            if (version > last_version) {
                last_version = version;
                frame++;
                
                /* Clear screen and display */
                printf("\033[2J\033[H");
                printf("╔═══════════════════════════════════════════════════════════════╗\n");
                printf("║  🖥️  Bifrost Monitor                    Frame: %-6d      ║\n", frame);
                printf("╠═══════════════════════════════════════════════════════════════╣\n");
                printf("║  CPU: "); 
                print_bar(state->cpu_usage_percent, 25);
                printf(" %5.1f%%             ║\n", state->cpu_usage_percent);
                printf("╠═══════════════════════════════════════════════════════════════╣\n");
                
                /* Show up to 8 cores */
                uint32_t show = state->core_count > 8 ? 8 : state->core_count;
                for (uint32_t i = 0; i < show; i++) {
                    printf("║  Core %u: ", i);
                    print_bar(state->cpu_cores[i], 20);
                    printf(" %5.1f%%                ║\n", state->cpu_cores[i]);
                }
                if (state->core_count > 8) {
                    printf("║  ... +%u more cores                                            ║\n",
                           state->core_count - 8);
                }
                
                printf("╠═══════════════════════════════════════════════════════════════╣\n");
                float mem_pct = state->memory_total_mb > 0 ?
                    (float)state->memory_used_mb / state->memory_total_mb * 100 : 0;
                printf("║  RAM: ");
                print_bar(mem_pct, 25);
                printf(" %u/%u MB          ║\n", state->memory_used_mb, state->memory_total_mb);
                
                printf("╠═══════════════════════════════════════════════════════════════╣\n");
                printf("║  ⏱️  Uptime: %lud %luh %lum                                      ║\n",
                       (unsigned long)(state->uptime_seconds / 86400),
                       (unsigned long)((state->uptime_seconds % 86400) / 3600),
                       (unsigned long)((state->uptime_seconds % 3600) / 60));
                
                printf("╠═══════════════════════════════════════════════════════════════╣\n");
                printf("║  📊 \033[96mRead Latency:\033[0m %.2f µs (min: %.2f, max: %.2f, avg: %.2f)  ║\n",
                       latency_us, g_latency_min, g_latency_max, avg_us);
                printf("╚═══════════════════════════════════════════════════════════════╝\n");
                printf("  Updates: %lu | Version: %lu | Ctrl+C to exit\n",
                       (unsigned long)state->update_counter, (unsigned long)version);
            }
        }
        /* NO SLEEP - read as fast as possible */
    }
    
    /* Print final latency stats */
    printf("\n\n");
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║               📊 Final Latency Statistics                     ║\n");
    printf("╠═══════════════════════════════════════════════════════════════╣\n");
    printf("║   Samples:  %-10lu                                        ║\n", (unsigned long)g_latency_count);
    printf("║   Min:      %-8.2f µs                                      ║\n", g_latency_min);
    printf("║   Max:      %-8.2f µs                                      ║\n", g_latency_max);
    printf("║   Avg:      %-8.2f µs                                      ║\n", g_latency_sum / g_latency_count);
    printf("╚═══════════════════════════════════════════════════════════════╝\n");
    
    bifrost_reader_leave(g_reader);
    printf("\n👋 Goodbye!\n");
    
    return 0;
}
