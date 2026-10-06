/**
 * Bifrost System Monitor Client
 * 
 * Displays live CPU/RAM stats with colored progress bars.
 * Includes read latency benchmarking.
 * 
 * Based on BifrostMemory template pattern (c.rs).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <float.h>
#include <bifrost.h>
#include "sysmon_protocol.h"

// ═══════════════════════════════════════════════════════════════════════════
// Global State
// ═══════════════════════════════════════════════════════════════════════════

static bifrost_reader_t* g_reader = NULL;
static volatile int g_running = 1;

// Latency tracking
static double g_latency_min = DBL_MAX;
static double g_latency_max = 0.0;
static double g_latency_sum = 0.0;
static uint64_t g_latency_count = 0;

// ═══════════════════════════════════════════════════════════════════════════
// Utilities
// ═══════════════════════════════════════════════════════════════════════════

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static double get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000.0 + ts.tv_nsec / 1000.0;
}

static void print_bar(float pct, int width) {
    int filled = (int)((pct / 100.0f) * width);
    printf("[");
    for (int i = 0; i < width; i++) {
        if (i < filled) {
            if (pct > 80)      printf("\033[91m█\033[0m");  // Red
            else if (pct > 50) printf("\033[93m▓\033[0m");  // Yellow
            else               printf("\033[92m░\033[0m");  // Green
        } else {
            printf(" ");
        }
    }
    printf("]");
}

// ═══════════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════════

int main(void) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║  🖥️  Bifrost System Monitor Client                          ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n\n");
    
    // Wait for pool to exist
    printf("🔍 Looking for daemon...\n");
    int attempts = 0;
    while (!bifrost_pool_exists(SYSMON_POOL_NAME) && attempts < 30) {
        sleep(1);
        attempts++;
        printf("\r⏳ Waiting for daemon... (%d/30)", attempts);
        fflush(stdout);
    }
    
    if (!bifrost_pool_exists(SYSMON_POOL_NAME)) {
        printf("\n❌ Daemon not found! Start the daemon first:\n");
        printf("   ./sysmon_daemon_bifrost\n");
        return 1;
    }
    printf("\n");
    
    // Join as reader
    g_reader = bifrost_join_as_reader(SYSMON_POOL_NAME, "sysmon_client");
    if (!g_reader) {
        printf("❌ Failed to connect: %s\n", bifrost_error_string(bifrost_get_last_error()));
        return 1;
    }
    printf("✅ Connected to daemon!\n");
    printf("📊 Reading stats... (Ctrl+C to exit)\n\n");
    sleep(1);
    
    int frame = 0;
    (void)frame;
    
    while (g_running) {
        // ═══════════════════════════════════════════════════════════════════
        // 📊 BENCHMARK: Measure read latency
        // ═══════════════════════════════════════════════════════════════════
        double t_start = get_time_us();
        
        size_t size;
        SysmonState* s = bifrost_reader_get(g_reader, SYSMON_WRITER_NAME, &size);
        
        double t_end = get_time_us();
        double latency_us = t_end - t_start;
        
        // Update latency stats
        if (latency_us < g_latency_min) g_latency_min = latency_us;
        if (latency_us > g_latency_max) g_latency_max = latency_us;
        g_latency_sum += latency_us;
        g_latency_count++;
        double avg_us = g_latency_sum / g_latency_count;
        
        // ═══════════════════════════════════════════════════════════════════
        // Display (every 100000 reads to avoid terminal slowdown)
        // ═══════════════════════════════════════════════════════════════════
        if (s && s->magic == SYSMON_MAGIC) {
            if (g_latency_count % 100000 == 0) {
                // Clear screen
                printf("\033[2J\033[H");
                
                printf("╔═══════════════════════════════════════════════════════════════╗\n");
                printf("║  🖥️  Bifrost Monitor (MAX PERF)   Reads: %-10lu        ║\n", (unsigned long)g_latency_count);
                printf("╠═══════════════════════════════════════════════════════════════╣\n");
                
                // Overall CPU
                printf("║  CPU: ");
                print_bar(s->cpu_usage_percent, 25);
                printf(" %5.1f%%             ║\n", s->cpu_usage_percent);
                
                printf("╠═══════════════════════════════════════════════════════════════╣\n");
                
                // RAM
                float mem_pct = s->memory_total_mb > 0 
                    ? (float)s->memory_used_mb / s->memory_total_mb * 100 
                    : 0;
                printf("║  RAM: ");
                print_bar(mem_pct, 25);
                printf(" %u/%u MB          ║\n", s->memory_used_mb, s->memory_total_mb);
                
                // Latency stats
                printf("╠═══════════════════════════════════════════════════════════════╣\n");
                printf("║  📊 Read Latency: %.3f µs (min: %.3f, max: %.3f)             ║\n",
                    latency_us, g_latency_min, g_latency_max);
                printf("║  📈 Avg: %.3f µs | Throughput: %.2f M reads/sec             ║\n",
                    avg_us, g_latency_count / (g_latency_sum) );
                
                printf("╚═══════════════════════════════════════════════════════════════╝\n");
                printf("  Updates: %lu | Ctrl+C to exit\n", (unsigned long)s->update_counter);
            }
        }
    }
    
    // Print final latency stats
    printf("\n\n");
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║  📊 Final Latency Statistics                                  ║\n");
    printf("╠═══════════════════════════════════════════════════════════════╣\n");
    printf("║  Samples: %-10lu                                          ║\n", (unsigned long)g_latency_count);
    printf("║  Min:     %.2f µs                                            ║\n", g_latency_min);
    printf("║  Max:     %.2f µs                                            ║\n", g_latency_max);
    printf("║  Avg:     %.2f µs                                            ║\n", g_latency_sum / g_latency_count);
    printf("╚═══════════════════════════════════════════════════════════════╝\n");
    
    // Cleanup
    bifrost_reader_leave(g_reader);
    
    printf("\n👋 Goodbye!\n");
    return 0;
}
