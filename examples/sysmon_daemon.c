/**
 * Bifrost System Monitor Daemon
 * 
 * Reads CPU/RAM/Uptime from /proc and publishes to shared memory.
 * Uses Bifrost lock-free IPC library.
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

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define POOL_NAME           "system_monitor"
#define DAEMON_NAME         "sysmon_daemon"
#define MAX_CORES           16
#define UPDATE_INTERVAL_US  100000  /* 100ms */

/* Commands */
#define CMD_UPDATE          1
#define CMD_REFRESH         2

/* ============================================================================
 * Data Structure (published to shared memory)
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

#define SYSMON_MAGIC 0x56454E4F  /* "VENO" */

/* ============================================================================
 * Globals
 * ============================================================================ */

static bifrost_pool_t* g_pool = NULL;
static bifrost_writer_t* g_writer = NULL;
static system_state_t* g_state = NULL;
static volatile int g_running = 1;
static uint64_t g_counter = 0;

static uint64_t prev_total[MAX_CORES + 1] = {0};
static uint64_t prev_idle[MAX_CORES + 1] = {0};

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

static inline uint64_t get_timestamp_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ============================================================================
 * System Stats Reading
 * ============================================================================ */

static void read_cpu_stats(void) {
    FILE* f = fopen("/proc/stat", "r");
    if (!f) return;
    
    char line[256];
    int core_idx = 0;
    
    while (fgets(line, sizeof(line), f) && core_idx <= MAX_CORES) {
        if (strncmp(line, "cpu", 3) != 0) continue;
        
        uint64_t user, nice, system, idle, iowait, irq, softirq;
        const char* ptr = line + (line[3] == ' ' ? 4 : 5);
        
        if (sscanf(ptr, "%lu %lu %lu %lu %lu %lu %lu",
                   &user, &nice, &system, &idle, &iowait, &irq, &softirq) != 7)
            continue;
        
        uint64_t total = user + nice + system + idle + iowait + irq + softirq;
        uint64_t idle_time = idle + iowait;
        
        uint64_t total_delta = total - prev_total[core_idx];
        uint64_t idle_delta = idle_time - prev_idle[core_idx];
        
        float usage = total_delta > 0 ? 
            (1.0f - (float)idle_delta / (float)total_delta) * 100.0f : 0;
        
        if (line[3] == ' ') {
            g_state->cpu_usage_percent = usage;
        } else if (core_idx > 0 && core_idx - 1 < MAX_CORES) {
            g_state->cpu_cores[core_idx - 1] = usage;
        }
        
        prev_total[core_idx] = total;
        prev_idle[core_idx] = idle_time;
        core_idx++;
    }
    
    g_state->core_count = core_idx > 1 ? core_idx - 1 : 0;
    fclose(f);
}

static void read_memory_stats(void) {
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return;
    
    char line[256];
    uint64_t total_kb = 0, available_kb = 0;
    
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "MemTotal:", 9) == 0)
            sscanf(line + 9, "%lu", &total_kb);
        else if (strncmp(line, "MemAvailable:", 13) == 0)
            sscanf(line + 13, "%lu", &available_kb);
    }
    
    g_state->memory_total_mb = (uint32_t)(total_kb / 1024);
    g_state->memory_used_mb = (uint32_t)((total_kb - available_kb) / 1024);
    fclose(f);
}

static void read_uptime(void) {
    FILE* f = fopen("/proc/uptime", "r");
    if (!f) return;
    
    double uptime;
    if (fscanf(f, "%lf", &uptime) == 1)
        g_state->uptime_seconds = (uint64_t)uptime;
    fclose(f);
}

static void update_and_publish(void) {
    read_cpu_stats();
    read_memory_stats();
    read_uptime();
    
    g_state->magic = SYSMON_MAGIC;
    g_state->version = 1;
    g_state->update_counter = ++g_counter;
    g_state->timestamp_ns = get_timestamp_ns();
    
    bifrost_writer_set_command(g_writer, CMD_UPDATE);
    bifrost_writer_publish(g_writer);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║   🖥️  Bifrost System Monitor Daemon                        ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n\n");
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* Create pool if it doesn't exist */
    if (bifrost_pool_exists(POOL_NAME)) {
        printf("📂 Opening existing pool '%s'...\n", POOL_NAME);
        g_pool = bifrost_open_pool(POOL_NAME);
    } else {
        printf("🆕 Creating new pool '%s'...\n", POOL_NAME);
        bifrost_pool_config_t config;
        bifrost_config_init(&config);
        config.initial_size = 1024 * 1024;  /* 1 MB */
        config.max_participants = 32;
        g_pool = bifrost_create_pool(POOL_NAME, &config);
    }
    
    if (!g_pool) {
        printf("❌ Failed to create/open pool: %s\n", bifrost_error_string(bifrost_get_last_error()));
        return 1;
    }
    
    /* Join as writer */
    g_writer = bifrost_join_as_writer(POOL_NAME, DAEMON_NAME);
    if (!g_writer) {
        printf("❌ Failed to join as writer: %s\n", bifrost_error_string(bifrost_get_last_error()));
        bifrost_close_pool(g_pool);
        return 1;
    }
    
    printf("✅ Writer ID: %u\n", bifrost_writer_get_id(g_writer));
    
    /* Allocate space for state (once, then reuse) */
    g_state = bifrost_writer_alloc(g_writer, sizeof(system_state_t));
    if (!g_state) {
        printf("❌ Failed to allocate state: %s\n", bifrost_error_string(bifrost_get_last_error()));
        bifrost_writer_leave(g_writer);
        bifrost_close_pool(g_pool);
        return 1;
    }
    
    printf("📊 State size: %zu bytes\n", sizeof(system_state_t));
    
    /* Initial update to detect cores */
    memset(g_state, 0, sizeof(system_state_t));
    update_and_publish();
    
    printf("🔍 Detected %u CPU cores\n", g_state->core_count);
    printf("🚀 Publishing... (Ctrl+C to stop)\n\n");
    
    /* Main loop - NO SLEEP for maximum throughput */
    while (g_running) {
        update_and_publish();
    }
    
    printf("\n\n👋 Shutting down...\n");
    
    bifrost_writer_leave(g_writer);
    
    /* Only destroy if we created it and no other participants */
    bifrost_pool_stats_t stats;
    bifrost_get_pool_stats(g_pool, &stats);
    if (stats.writer_count == 0 && stats.reader_count == 0) {
        bifrost_destroy_pool(g_pool);
        printf("🗑️  Pool destroyed.\n");
    } else {
        bifrost_close_pool(g_pool);
        printf("📂 Pool closed (other participants active).\n");
    }
    
    printf("👋 Goodbye!\n");
    return 0;
}
