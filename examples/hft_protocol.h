/**
 * Bifrost High-Frequency Trading (HFT) Simulation
 * 
 * ⚡ ULTRA LOW LATENCY TRADING TEST ⚡
 * 
 * Simulates real HFT environment:
 * - Market Data Feed (Exchange) → publishes price ticks
 * - Trading Engine → reads prices, executes trades
 * - Measures tick-to-trade latency in NANOSECONDS
 * 
 * In real HFT, every nanosecond matters!
 * Target: < 1 microsecond tick-to-trade latency
 */

#ifndef HFT_PROTOCOL_H
#define HFT_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

// ═══════════════════════════════════════════════════════════════════════════
// Configuration
// ═══════════════════════════════════════════════════════════════════════════

#define HFT_POOL_NAME       "hft_exchange"
#define HFT_MAGIC           0x48465420  // "HFT "

#define HFT_NUM_SYMBOLS     8           // Trading 8 stocks simultaneously
#define HFT_SYMBOL_LEN      8

// Cache line alignment for maximum performance
#define HFT_CACHE_LINE      64

// ═══════════════════════════════════════════════════════════════════════════
// Market Data - Published by Exchange
// ═══════════════════════════════════════════════════════════════════════════

typedef struct __attribute__((aligned(HFT_CACHE_LINE))) {
    char symbol[HFT_SYMBOL_LEN];    // "AAPL", "GOOGL", etc.
    
    // Prices (in cents to avoid floating point)
    int64_t bid_price;              // Best bid price
    int64_t ask_price;              // Best ask price
    int64_t last_price;             // Last traded price
    
    // Quantities
    uint32_t bid_size;              // Shares at bid
    uint32_t ask_size;              // Shares at ask
    uint32_t last_size;             // Last trade size
    
    // Timestamps (nanoseconds since epoch)
    uint64_t exchange_timestamp;    // When exchange published
    uint64_t sequence_num;          // For detecting gaps
    
    char _padding[8];
} MarketTick;

typedef struct __attribute__((aligned(HFT_CACHE_LINE))) {
    uint32_t magic;
    uint32_t num_symbols;
    uint64_t tick_count;            // Total ticks published
    uint64_t timestamp_ns;          // Last update time
    
    MarketTick ticks[HFT_NUM_SYMBOLS];
} MarketDataFeed;

// ═══════════════════════════════════════════════════════════════════════════
// Trade Orders - Sent by Trading Engine
// ═══════════════════════════════════════════════════════════════════════════

typedef enum {
    ORDER_BUY = 1,
    ORDER_SELL = 2
} OrderSide;

typedef enum {
    ORDER_MARKET = 1,       // Execute at market price
    ORDER_LIMIT = 2         // Execute at specific price or better
} OrderType;

typedef struct __attribute__((aligned(HFT_CACHE_LINE))) {
    uint64_t order_id;
    char symbol[HFT_SYMBOL_LEN];
    
    OrderSide side;
    OrderType type;
    
    int64_t price;          // Limit price (0 for market orders)
    uint32_t quantity;
    
    // Timing (in nanoseconds)
    uint64_t signal_time;   // When trading signal was generated
    uint64_t order_time;    // When order was created
    uint64_t sent_time;     // When order was sent
    
    // Latency breakdown (filled after execution)
    uint64_t tick_to_decision_ns;   // Time to analyze and decide
    uint64_t decision_to_order_ns;  // Time to create order
    uint64_t total_latency_ns;      // Total tick-to-trade
} TradeOrder;

// ═══════════════════════════════════════════════════════════════════════════
// Trading Statistics
// ═══════════════════════════════════════════════════════════════════════════

typedef struct {
    uint64_t ticks_received;
    uint64_t ticks_processed;
    uint64_t orders_sent;
    uint64_t orders_filled;
    
    // Latency stats (nanoseconds)
    double min_latency_ns;
    double max_latency_ns;
    double total_latency_ns;
    double avg_latency_ns;
    
    // P99 latency tracking
    uint64_t latency_histogram[1000];  // Buckets of 10ns each (0-10µs)
    
    // PnL tracking
    int64_t realized_pnl;       // In cents
    int64_t unrealized_pnl;
    uint64_t total_volume;      // Shares traded
    
    // Performance
    double ticks_per_second;
    double orders_per_second;
} TradingStats;

// ═══════════════════════════════════════════════════════════════════════════
// Stock Symbols
// ═══════════════════════════════════════════════════════════════════════════

static const char* HFT_SYMBOLS[HFT_NUM_SYMBOLS] = {
    "AAPL", "GOOGL", "MSFT", "AMZN",
    "META", "NVDA", "TSLA", "AMD"
};

// Base prices in cents
static const int64_t HFT_BASE_PRICES[HFT_NUM_SYMBOLS] = {
    17500,  // AAPL  $175.00
    14200,  // GOOGL $142.00
    37800,  // MSFT  $378.00
    17800,  // AMZN  $178.00
    50500,  // META  $505.00
    87500,  // NVDA  $875.00
    24200,  // TSLA  $242.00
    16800   // AMD   $168.00
};

// ═══════════════════════════════════════════════════════════════════════════
// Utility
// ═══════════════════════════════════════════════════════════════════════════

static inline uint64_t hft_get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static inline double hft_ns_to_us(uint64_t ns) {
    return (double)ns / 1000.0;
}

static inline double hft_ns_to_ms(uint64_t ns) {
    return (double)ns / 1000000.0;
}

#endif // HFT_PROTOCOL_H
