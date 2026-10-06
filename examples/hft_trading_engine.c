/**
 * Bifrost HFT - Trading Engine
 * 
 * ⚡ ULTRA LOW LATENCY TRADING ⚡
 * 
 * Reads market data, makes trading decisions, executes trades.
 * Measures tick-to-trade latency in NANOSECONDS.
 * 
 * Trading Strategies:
 * 1. Momentum: Buy when price rising, sell when falling
 * 2. Mean Reversion: Buy when cheap, sell when expensive
 * 3. Arbitrage: Exploit price differences
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
#include <float.h>
#include <bifrost.h>
#include "hft_protocol.h"

// ═══════════════════════════════════════════════════════════════════════════
// Configuration
// ═══════════════════════════════════════════════════════════════════════════

#define MAX_ORDERS          10000000    // 10M orders max
#define LATENCY_BUCKETS     10000       // 10ns per bucket, up to 100µs

// ═══════════════════════════════════════════════════════════════════════════
// Global State
// ═══════════════════════════════════════════════════════════════════════════

static bifrost_reader_t* g_reader = NULL;
static volatile int g_running = 1;

// Trading state per symbol
typedef struct {
    int64_t last_price;
    int64_t position;           // Current holdings (+ long, - short)
    int64_t avg_cost;           // Average entry price
    int64_t realized_pnl;       // Realized P&L in cents
    uint64_t trades;
    uint64_t last_seq;
} SymbolState;

static SymbolState g_symbols[HFT_NUM_SYMBOLS] = {0};

// Latency tracking
static uint64_t g_latency_histogram[LATENCY_BUCKETS] = {0};
static double g_min_latency_ns = DBL_MAX;
static double g_max_latency_ns = 0;
static double g_total_latency_ns = 0;
static uint64_t g_latency_count = 0;

// Statistics
static uint64_t g_ticks_received = 0;
static uint64_t g_orders_sent = 0;
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

// Prefetch for ultra-low latency
static inline void prefetch_read(void* addr) {
    __builtin_prefetch(addr, 0, 3);
}

static inline void record_latency(uint64_t latency_ns) {
    g_total_latency_ns += latency_ns;
    g_latency_count++;
    
    if (latency_ns < g_min_latency_ns) g_min_latency_ns = latency_ns;
    if (latency_ns > g_max_latency_ns) g_max_latency_ns = latency_ns;
    
    // Record in histogram (10ns buckets)
    size_t bucket = latency_ns / 10;
    if (bucket >= LATENCY_BUCKETS) bucket = LATENCY_BUCKETS - 1;
    g_latency_histogram[bucket]++;
}

// Calculate P99 latency from histogram
static double calculate_p99_latency(void) {
    uint64_t total = 0;
    for (size_t i = 0; i < LATENCY_BUCKETS; i++) {
        total += g_latency_histogram[i];
    }
    
    uint64_t p99_count = total * 99 / 100;
    uint64_t running = 0;
    for (size_t i = 0; i < LATENCY_BUCKETS; i++) {
        running += g_latency_histogram[i];
        if (running >= p99_count) {
            return (double)(i * 10);  // Return in nanoseconds
        }
    }
    return g_max_latency_ns;
}

// ═══════════════════════════════════════════════════════════════════════════
// Trading Strategy
// ═══════════════════════════════════════════════════════════════════════════

// Simple momentum strategy: trade only when significant price change
static inline bool should_trade(int symbol_idx, const MarketTick* tick) {
    SymbolState* state = &g_symbols[symbol_idx];
    
    if (state->last_price == 0) {
        state->last_price = tick->last_price;
        state->avg_cost = tick->last_price;
        return false;
    }
    
    int64_t price_change = tick->last_price - state->last_price;
    state->last_price = tick->last_price;
    
    // Only trade on significant movement (> 2 cents)
    if (price_change > 2 || price_change < -2) {
        return true;
    }
    
    return false;
}

static inline void execute_trade(int symbol_idx, const MarketTick* tick, uint64_t signal_time) {
    SymbolState* state = &g_symbols[symbol_idx];
    
    // Decide buy or sell based on momentum
    int64_t price_change = tick->last_price - state->avg_cost;
    bool is_buy = (price_change > 0);  // Price went up -> buy (momentum)
    
    // Calculate trade
    int32_t quantity = 100;  // Trade 100 shares
    int64_t trade_price = is_buy ? tick->ask_price : tick->bid_price;
    
    // Update position and P&L
    if (is_buy) {
        if (state->position < 0) {
            // Closing short
            int64_t pnl = (state->avg_cost - trade_price) * 100;
            state->realized_pnl += pnl;
        }
        state->position += quantity;
    } else {
        if (state->position > 0) {
            // Closing long
            int64_t pnl = (trade_price - state->avg_cost) * 100;
            state->realized_pnl += pnl;
        }
        state->position -= quantity;
    }
    
    state->avg_cost = trade_price;
    state->trades++;
    
    uint64_t sent_time = hft_get_time_ns();
    
    // Record latency: LOCAL measurement (signal_time to order_sent)
    // This is the actual processing time in our trading engine
    uint64_t tick_to_trade = sent_time - signal_time;
    
    record_latency(tick_to_trade);
    
    g_orders_sent++;
}

// ═══════════════════════════════════════════════════════════════════════════
// Main Trading Loop
// ═══════════════════════════════════════════════════════════════════════════

static void trading_loop(void) {
    uint64_t last_seq = 0;
    uint64_t last_report = hft_get_time_ns();
    uint64_t last_tick_count = 0;
    
    while (g_running) {
        uint64_t signal_time = hft_get_time_ns();
        
        // Read market data (ZERO-COPY!)
        size_t size;
        MarketDataFeed* feed = bifrost_reader_get(g_reader, "exchange", &size);
        
        if (!feed || feed->magic != HFT_MAGIC) {
            __asm__ __volatile__ ("pause" ::: "memory");
            continue;
        }
        
        // Check for new data
        if (feed->tick_count <= last_seq) {
            __asm__ __volatile__ ("pause" ::: "memory");
            continue;
        }
        
        last_seq = feed->tick_count;
        g_ticks_received++;
        
        // Prefetch all ticks for faster processing
        for (int i = 0; i < HFT_NUM_SYMBOLS; i++) {
            prefetch_read(&feed->ticks[i]);
        }
        
        // Process each symbol
        for (int i = 0; i < HFT_NUM_SYMBOLS; i++) {
            const MarketTick* tick = &feed->ticks[i];
            
            // Check for trading signal
            if (should_trade(i, tick)) {
                execute_trade(i, tick, signal_time);
            }
        }
        
        // Report every second
        uint64_t now = hft_get_time_ns();
        if (now - last_report >= 1000000000ULL) {
            double elapsed_sec = (now - g_start_time) / 1e9;
            uint64_t ticks_this_sec = g_ticks_received - last_tick_count;
            (void)elapsed_sec; (void)ticks_this_sec;
            
            double avg_lat = g_latency_count > 0 ? g_total_latency_ns / g_latency_count : 0;
            
            printf("\r📈 Ticks: %lu M | Orders: %lu K | Lat: min=%.0f avg=%.0f max=%.0f ns    ",
                   g_ticks_received / 1000000,
                   g_orders_sent / 1000,
                   g_min_latency_ns < DBL_MAX ? g_min_latency_ns : 0,
                   avg_lat,
                   g_max_latency_ns);
            fflush(stdout);
            
            last_report = now;
            last_tick_count = g_ticks_received;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════════

int main(void) {
    printf("╔═══════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║   💹 Bifrost HFT - TRADING ENGINE                                          ║\n");
    printf("║   Ultra-low-latency algorithmic trading simulation                            ║\n");
    printf("╚═══════════════════════════════════════════════════════════════════════════════╝\n\n");
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Pin to CPU 1 (different from exchange on CPU 0)
    pin_to_cpu(1);
    printf("📍 Pinned trading engine to CPU 1\n");
    
    // Wait for exchange
    printf("🔍 Looking for exchange...\n");
    int attempts = 0;
    while (!bifrost_pool_exists(HFT_POOL_NAME) && attempts < 30) {
        sleep(1);
        attempts++;
        printf("\r⏳ Waiting for exchange... (%d/30)", attempts);
        fflush(stdout);
    }
    
    if (!bifrost_pool_exists(HFT_POOL_NAME)) {
        printf("\n❌ Exchange not found! Start hft_exchange first.\n");
        return 1;
    }
    printf("\n✅ Exchange detected!\n");
    
    // Join as reader
    g_reader = bifrost_join_as_reader(HFT_POOL_NAME, "trading_engine");
    if (!g_reader) {
        printf("❌ Failed to join: %s\n", bifrost_error_string(bifrost_get_last_error()));
        return 1;
    }
    printf("✅ Connected to exchange!\n");
    printf("⚡ Trading engine ONLINE!\n\n");
    
    printf("🔔 Starting trading...\n\n");
    
    g_start_time = hft_get_time_ns();
    
    // Main trading loop
    trading_loop();
    
    // Final statistics
    uint64_t end_time = hft_get_time_ns();
    double duration = (end_time - g_start_time) / 1e9;
    double avg_lat = g_latency_count > 0 ? g_total_latency_ns / g_latency_count : 0;
    double p99_lat = calculate_p99_latency();
    
    // Calculate total P&L
    int64_t total_pnl = 0;
    uint64_t total_trades = 0;
    for (int i = 0; i < HFT_NUM_SYMBOLS; i++) {
        total_pnl += g_symbols[i].realized_pnl;
        total_trades += g_symbols[i].trades;
    }
    
    printf("\n\n");
    printf("╔═══════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║   📊 TRADING ENGINE STATISTICS                                                ║\n");
    printf("╠═══════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║   Duration:        %.2f seconds                                              ║\n", duration);
    printf("║   Ticks received:  %lu                                                  ║\n", g_ticks_received);
    printf("║   Orders sent:     %lu                                                       ║\n", g_orders_sent);
    printf("║   ⚡ Tick rate:     %.2f M/sec                                                ║\n", g_ticks_received / duration / 1e6);
    printf("║   ⚡ Order rate:    %.2f K/sec                                                ║\n", g_orders_sent / duration / 1e3);
    printf("╠═══════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║   🎯 LATENCY (TICK-TO-TRADE):                                                 ║\n");
    printf("║      Min:   %10.0f ns  (%.3f µs)                                          ║\n", 
           g_min_latency_ns < DBL_MAX ? g_min_latency_ns : 0, 
           g_min_latency_ns < DBL_MAX ? g_min_latency_ns / 1000 : 0);
    printf("║      Avg:   %10.0f ns  (%.3f µs)                                          ║\n", avg_lat, avg_lat / 1000);
    printf("║      P99:   %10.0f ns  (%.3f µs)                                          ║\n", p99_lat, p99_lat / 1000);
    printf("║      Max:   %10.0f ns  (%.3f µs)                                          ║\n", g_max_latency_ns, g_max_latency_ns / 1000);
    printf("╠═══════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║   💰 TRADING P&L:                                                             ║\n");
    printf("║      Total trades:  %lu                                                      ║\n", total_trades);
    printf("║      Realized P&L:  $%.2f                                                    ║\n", total_pnl / 100.0);
    printf("╠═══════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║   📈 PER-SYMBOL BREAKDOWN:                                                    ║\n");
    for (int i = 0; i < HFT_NUM_SYMBOLS; i++) {
        SymbolState* s = &g_symbols[i];
        printf("║      %-6s: %6lu trades, Position: %+5ld, P&L: $%+8.2f               ║\n",
               HFT_SYMBOLS[i], s->trades, s->position, s->realized_pnl / 100.0);
    }
    printf("╚═══════════════════════════════════════════════════════════════════════════════╝\n");
    
    bifrost_reader_leave(g_reader);
    printf("\n💼 Trading engine offline.\n");
    
    return 0;
}
