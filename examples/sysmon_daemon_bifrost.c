/**
 * Bifrost System Monitor Daemon
 * 
 * Reads CPU/RAM/Uptime from /proc and publishes to shared memory.
 * Uses Bifrost library (lock-free shared memory).
 * 
 * Based on BifrostMemory template pattern (c.rs).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <bifrost.h>
#include "sysmon_protocol.h"

// ═══════════════════════════════════════════════════════════════════════════
// Global State
// ═══════════════════════════════════════════════════════════════════════════

static bifrost_pool_t* g_pool = NULL;
static bifrost_writer_t* g_writer = NULL;
static volatile int g_running = 1;
static uint64_t g_counter = 0;

// CPU tracking for delta calculation
static uint64_t prev_total[SYSMON_MAX_CORES + 1] = {0};
static uint64_t prev_idle[SYSMON_MAX_CORES + 1] = {0};

// Current state (written to shared memory)
static SysmonState g_state = {0};

// ═══════════════════════════════════════════════════════════════════════════
// Signal Handler
// ═══════════════════════════════════════════════════════════════════════════

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// System Stats Readers (from /proc)
// ═══════════════════════════════════════════════════════════════════════════

static void read_cpu_stats(void) {
    FILE* f = fopen("/proc/stat", "r");
    if (!f) return;
    
    char line[256];
    int core_idx = 0;
    
    while (fgets(line, sizeof(line), f) && core_idx <= SYSMON_MAX_CORES) {
        if (strncmp(line, "cpu", 3) != 0) continue;
        
        uint64_t user, nice, system, idle, iowait, irq, softirq;
        int offset = (line[3] == ' ') ? 4 : 5;  // "cpu " vs "cpu0"
        
        if (sscanf(line + offset, "%lu %lu %lu %lu %lu %lu %lu",
                   &user, &nice, &system, &idle, &iowait, &irq, &softirq) != 7)
            continue;
        
        uint64_t total = user + nice + system + idle + iowait + irq + softirq;
        uint64_t idle_time = idle + iowait;
        
        uint64_t total_delta = total - prev_total[core_idx];
        uint64_t idle_delta = idle_time - prev_idle[core_idx];
        
        float usage = 0;
        if (total_delta > 0) {
            usage = (1.0f - (float)idle_delta / (float)total_delta) * 100.0f;
        }
        
        if (line[3] == ' ') {
            // Overall CPU
            g_state.cpu_usage_percent = usage;
        } else if (core_idx - 1 >= 0 && core_idx - 1 < SYSMON_MAX_CORES) {
            // Per-core
            g_state.cpu_cores[core_idx - 1] = usage;
        }
        
        prev_total[core_idx] = total;
        prev_idle[core_idx] = idle_time;
        core_idx++;
    }
    
    g_state.core_count = core_idx > 1 ? core_idx - 1 : 0;
    fclose(f);
}

static void read_memory_stats(void) {
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return;
    
    char line[256];
    uint64_t total_kb = 0, available_kb = 0;
    
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "MemTotal:", 9) == 0) {
            sscanf(line + 9, "%lu", &total_kb);
        } else if (strncmp(line, "MemAvailable:", 13) == 0) {
            sscanf(line + 13, "%lu", &available_kb);
        }
    }
    
    g_state.memory_total_mb = (uint32_t)(total_kb / 1024);
    g_state.memory_used_mb = (uint32_t)((total_kb - available_kb) / 1024);
    fclose(f);
}

static void read_uptime(void) {
    FILE* f = fopen("/proc/uptime", "r");
    if (!f) return;
    
    double uptime;
    if (fscanf(f, "%lf", &uptime) == 1) {
        g_state.uptime_seconds = (uint64_t)uptime;
    }
    fclose(f);
}

// ═══════════════════════════════════════════════════════════════════════════
// Shared memory pointer (allocated once, reused)
// ═══════════════════════════════════════════════════════════════════════════

static SysmonState* g_shared = NULL;

// ═══════════════════════════════════════════════════════════════════════════
// Update and Publish
// ═══════════════════════════════════════════════════════════════════════════

static void update_and_publish(void) {
    if (!g_shared) return;
    
    // Read all stats
    read_cpu_stats();
    read_memory_stats();
    read_uptime();
    
    // Update metadata
    g_state.magic = SYSMON_MAGIC;
    g_state.version = 1;
    g_state.update_counter = ++g_counter;
    
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    g_state.timestamp_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    
    // Direct write to pre-allocated shared memory - NO memcpy!
    *g_shared = g_state;
    
    // Set command and publish
    bifrost_writer_set_command(g_writer, CMD_STATS_UPDATE);
    bifrost_writer_publish(g_writer);
}

// ═══════════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════════

int main(void) {
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║  🖥️  Bifrost System Monitor Daemon                          ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n\n");
    
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Create pool (Director role)
    printf("📦 Creating shared memory pool...\n");
    g_pool = bifrost_create_pool(SYSMON_POOL_NAME, NULL);
    if (!g_pool) {
        printf("❌ Failed to create pool: %s\n", bifrost_error_string(bifrost_get_last_error()));
        return 1;
    }
    printf("✅ Pool created: %s\n", SYSMON_POOL_NAME);
    
    // Join as writer
    printf("📝 Joining as writer...\n");
    g_writer = bifrost_join_as_writer(SYSMON_POOL_NAME, SYSMON_WRITER_NAME);
    if (!g_writer) {
        printf("❌ Failed to join as writer: %s\n", bifrost_error_string(bifrost_get_last_error()));
        bifrost_destroy_pool(g_pool);
        return 1;
    }
    printf("✅ Writer registered: %s (ID: %u)\n\n", SYSMON_WRITER_NAME, bifrost_writer_get_id(g_writer));
    
    // Allocate shared memory ONCE
    printf("💾 Allocating shared memory (%zu bytes)...\n", sizeof(SysmonState));
    g_shared = bifrost_writer_alloc(g_writer, sizeof(SysmonState));
    if (!g_shared) {
        printf("❌ Failed to allocate: %s\n", bifrost_error_string(bifrost_get_last_error()));
        bifrost_writer_leave(g_writer);
        bifrost_destroy_pool(g_pool);
        return 1;
    }
    printf("✅ Memory allocated at offset in pool\n\n");
    
    // Initial update
    update_and_publish();
    printf("🔍 Detected %u CPU cores\n", g_state.core_count);
    printf("🚀 Publishing stats... (Ctrl+C to stop)\n\n");
    
    // Main loop - updates same memory location, no new allocations!
    while (g_running) {
        update_and_publish();
        
        // Print status (every 100000 updates to avoid terminal slowdown)
        if (g_counter % 100000 == 0) {
            printf("\r🖥️  CPU: %5.1f%% | RAM: %u/%u MB | Updates: %lu    ",
                g_state.cpu_usage_percent,
                g_state.memory_used_mb,
                g_state.memory_total_mb,
                (unsigned long)g_state.update_counter);
            fflush(stdout);
        }
    }
    
    // Cleanup
    printf("\n\n🧹 Cleaning up...\n");
    printf("📊 Total updates: %lu\n", (unsigned long)g_counter);
    bifrost_writer_leave(g_writer);
    bifrost_destroy_pool(g_pool);
    
    printf("👋 Goodbye!\n");
    return 0;
}

