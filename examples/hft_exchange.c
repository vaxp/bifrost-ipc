/**
 * Bifrost HFT - Market Data Feed (Exchange Simulator)
 * 
 * Simulates a stock exchange publishing market data at ultra-high speeds.
 * Publishes price ticks for 8 symbols simultaneously.
 * 
 * Target: Millions of ticks per second
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <sched.h>
#include <sys/mman.h>
#include <bifrost.h>
#include "hft_protocol.h"

// ═══════════════════════════════════════════════════════════════════════════
// Configuration
// ═══════════════════════════════════════════════════════════════════════════

#define EXCHANGE_DURATION_SEC   30  // Run for 30 seconds

// ═══════════════════════════════════════════════════════════════════════════
// Global State
// ═══════════════════════════════════════════════════════════════════════════

static bifrost_pool_t* g_pool = NULL;
static bifrost_writer_t* g_writer = NULL;
static MarketDataFeed* g_feed = NULL;
static volatile int g_running = 1;

// Statistics
static uint64_t g_ticks_published = 0;
static uint64_t g_start_time = 0;

// ═══════════════════════════════════════════════════════════════════════════
// Utilities
// ═══════════════════════════════════════════════════════════════════════════

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static inline void pin_to_cpu(int cpu) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu % sysconf(_SC_NPROCESSORS_ONLN), &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
}

// Fast pseudo-random for price simulation
static uint64_t g_rand_state = 12345;
static inline int64_t fast_rand_range(int64_t min, int64_t max) {
    g_rand_state ^= g_rand_state << 13;
    g_rand_state ^= g_rand_state >> 7;
    g_rand_state ^= g_rand_state << 17;
    return min + (g_rand_state % (max - min + 1));
}

// ═══════════════════════════════════════════════════════════════════════════
// Price Simulation
// ═══════════════════════════════════════════════════════════════════════════

static int64_t g_prices[HFT_NUM_SYMBOLS];

static void init_prices(void) {
    for (int i = 0; i < HFT_NUM_SYMBOLS; i++) {
        g_prices[i] = HFT_BASE_PRICES[i];
    }
}

static void update_prices(void) {
    for (int i = 0; i < HFT_NUM_SYMBOLS; i++) {
        // Random walk: -5 to +5 cents
        int64_t change = fast_rand_range(-5, 5);
        g_prices[i] += change;
        
        // Keep price positive
        if (g_prices[i] < 100) g_prices[i] = 100;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Publish Market Data
// ═══════════════════════════════════════════════════════════════════════════

static inline void publish_tick(void) {
    uint64_t now = hft_get_time_ns();
    
    // Update prices
    update_prices();
    
    // Fill market data feed
    g_feed->magic = HFT_MAGIC;
    g_feed->num_symbols = HFT_NUM_SYMBOLS;
    g_feed->tick_count = ++g_ticks_published;
    g_feed->timestamp_ns = now;
    
    for (int i = 0; i < HFT_NUM_SYMBOLS; i++) {
        MarketTick* tick = &g_feed->ticks[i];
        
        strncpy(tick->symbol, HFT_SYMBOLS[i], HFT_SYMBOL_LEN);
        
        // Bid/Ask spread: 1-3 cents
        int64_t spread = fast_rand_range(1, 3);
        tick->bid_price = g_prices[i];
        tick->ask_price = g_prices[i] + spread;
        tick->last_price = g_prices[i] + fast_rand_range(0, spread);
        
        // Random sizes
        tick->bid_size = (uint32_t)fast_rand_range(100, 10000);
        tick->ask_size = (uint32_t)fast_rand_range(100, 10000);
        tick->last_size = (uint32_t)fast_rand_range(1, 1000);
        
        tick->exchange_timestamp = now;
        tick->sequence_num = g_ticks_published;
    }
    
    // Memory barrier + publish
    __atomic_thread_fence(__ATOMIC_RELEASE);
    bifrost_writer_set_command(g_writer, (uint32_t)(g_ticks_published & 0xFFFFFFFF));
    bifrost_writer_publish(g_writer);
}

// ═══════════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════════

int main(void) {
    printf("╔═══════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║   📈 Bifrost HFT - MARKET DATA EXCHANGE                                    ║\n");
    printf("║   Simulating ultra-low-latency market data feed                               ║\n");
    printf("╚═══════════════════════════════════════════════════════════════════════════════╝\n\n");
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Pin to CPU 0 for consistent latency
    pin_to_cpu(0);
    printf("📍 Pinned exchange to CPU 0\n");
    
    // Initialize prices
    init_prices();
    printf("💹 Initialized %d symbols:\n", HFT_NUM_SYMBOLS);
    for (int i = 0; i < HFT_NUM_SYMBOLS; i++) {
        printf("   %s: $%.2f\n", HFT_SYMBOLS[i], HFT_BASE_PRICES[i] / 100.0);
    }
    printf("\n");
    
    // Create pool
    printf("📦 Creating exchange pool...\n");
    bifrost_pool_config_t config;
    bifrost_config_init(&config);
    config.initial_size = 4 * 1024 * 1024;  // 4 MB
    config.max_participants = 32;
    
    g_pool = bifrost_create_pool(HFT_POOL_NAME, &config);
    if (!g_pool) {
        printf("❌ Failed to create pool: %s\n", bifrost_error_string(bifrost_get_last_error()));
        return 1;
    }
    printf("✅ Pool created\n");
    
    // Join as writer
    g_writer = bifrost_join_as_writer(HFT_POOL_NAME, "exchange");
    if (!g_writer) {
        printf("❌ Failed to join as writer\n");
        bifrost_destroy_pool(g_pool);
        return 1;
    }
    
    // Allocate market data feed
    g_feed = bifrost_writer_alloc(g_writer, sizeof(MarketDataFeed));
    if (!g_feed) {
        printf("❌ Failed to allocate feed\n");
        bifrost_writer_leave(g_writer);
        bifrost_destroy_pool(g_pool);
        return 1;
    }
    
    // Pre-warm memory
    madvise(g_feed, sizeof(MarketDataFeed), MADV_WILLNEED);
    memset(g_feed, 0, sizeof(MarketDataFeed));
    
    printf("✅ Exchange ready!\n");
    printf("⏳ Waiting for trading engines to connect...\n");
    printf("   Start hft_trading_engine in another terminal!\n\n");
    
    sleep(2);  // Give traders time to connect
    
    printf("🔔 MARKET OPEN! Publishing ticks at maximum speed...\n\n");
    
    g_start_time = hft_get_time_ns();
    uint64_t last_report = g_start_time;
    uint64_t last_tick_count = 0;
    
    // Main loop - publish as fast as possible!
    while (g_running) {
        publish_tick();
        
        // Report every second
        uint64_t now = hft_get_time_ns();
        if (now - last_report >= 1000000000ULL) {
            double elapsed_sec = (now - g_start_time) / 1e9;
            uint64_t ticks_this_sec = g_ticks_published - last_tick_count;
            double avg_tps = g_ticks_published / elapsed_sec;
            
            printf("\r📊 Ticks: %lu M | Rate: %.2f M/sec | Avg: %.2f M/sec    ",
                   g_ticks_published / 1000000,
                   ticks_this_sec / 1e6,
                   avg_tps / 1e6);
            fflush(stdout);
            
            last_report = now;
            last_tick_count = g_ticks_published;
            
            // Check duration
            if (elapsed_sec >= EXCHANGE_DURATION_SEC) {
                break;
            }
        }
    }
    
    g_running = 0;
    
    // Final stats
    uint64_t end_time = hft_get_time_ns();
    double duration = (end_time - g_start_time) / 1e9;
    
    printf("\n\n");
    printf("╔═══════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║   📊 EXCHANGE STATISTICS                                                      ║\n");
    printf("╠═══════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║   Duration:        %.2f seconds                                              ║\n", duration);
    printf("║   Total ticks:     %lu                                                 ║\n", g_ticks_published);
    printf("║   Symbols:         %d                                                         ║\n", HFT_NUM_SYMBOLS);
    printf("║   ⚡ Tick rate:     %.2f million/sec                                          ║\n", g_ticks_published / duration / 1e6);
    printf("║   📶 Data rate:     %.2f GB/sec                                               ║\n", 
           g_ticks_published * sizeof(MarketDataFeed) / duration / 1e9);
    printf("╚═══════════════════════════════════════════════════════════════════════════════╝\n");
    
    // Cleanup
    bifrost_writer_leave(g_writer);
    bifrost_destroy_pool(g_pool);
    
    printf("\n🔔 MARKET CLOSED\n");
    
    return 0;
}
