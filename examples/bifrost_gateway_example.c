/**
 * @file bifrost_gateway_example.c
 * @brief Bifrost Gateway Usage Example
 * 
 * Demonstrates how to use the Bifrost Gateway API to bridge
 * local shared memory with network communication.
 * 
 * Build: Part of Bifrost examples
 * Run:   sudo ./bifrost_gateway_example <interface> <remote_mac> <remote_ip>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

#include "bifrost.h"
#include "bifrost_gateway.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * Configuration
 * ═══════════════════════════════════════════════════════════════════════════ */

#define POOL_NAME       "bifrost_market_data"
#define WRITER_NAME     "local_writer"

/* ═══════════════════════════════════════════════════════════════════════════
 * Signal Handling
 * ═══════════════════════════════════════════════════════════════════════════ */

static volatile int g_running = 1;
static bifrost_gateway_t* g_gateway = NULL;

static void signal_handler(int sig) {
    (void)sig;
    printf("\n⏹️  Stopping...\n");
    g_running = 0;
    if (g_gateway) {
        bifrost_gateway_stop(g_gateway);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Market Data Structure
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct __attribute__((packed)) {
    uint64_t    sequence;
    uint64_t    timestamp_ns;
    char        symbol[8];
    int64_t     bid_price;
    int64_t     ask_price;
    uint32_t    bid_size;
    uint32_t    ask_size;
} market_quote_t;

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Local Writer (simulates market data source)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void* writer_thread(void* arg) {
    (void)arg;
    
    /* Join pool as writer */
    bifrost_writer_t* writer = bifrost_join_as_writer(POOL_NAME, WRITER_NAME);
    if (!writer) {
        fprintf(stderr, "ERROR: Cannot join pool as writer\n");
        return NULL;
    }
    
    printf("📝 Local writer started\n");
    
    const char* symbols[] = {"AAPL", "GOOGL", "MSFT", "AMZN"};
    uint64_t sequence = 0;
    
    while (g_running) {
        market_quote_t* quote = bifrost_writer_alloc(writer, sizeof(market_quote_t));
        if (quote) {
            int sym_idx = sequence % 4;
            
            quote->sequence = ++sequence;
            quote->timestamp_ns = get_time_ns();
            memset(quote->symbol, 0, 8);
            strncpy(quote->symbol, symbols[sym_idx], 7);
            quote->bid_price = 15000000 + (sequence % 10000);
            quote->ask_price = quote->bid_price + 100;
            quote->bid_size = 100 + (sequence % 1000);
            quote->ask_size = 100 + (sequence % 1000);
            
            bifrost_writer_publish(writer);
        }
        
        usleep(1000);  /* 1ms between quotes */
    }
    
    bifrost_writer_leave(writer);
    printf("📝 Local writer stopped\n");
    
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════════════════════════════════ */

static void print_usage(const char* prog) {
    printf("Bifrost Gateway Example\n\n");
    printf("Usage: %s <interface> <remote_mac> <remote_ip> [mode]\n\n", prog);
    printf("Arguments:\n");
    printf("  interface   Network interface (e.g., lo, enp0s31f6)\n");
    printf("  remote_mac  Remote MAC address (xx:xx:xx:xx:xx:xx)\n");
    printf("  remote_ip   Remote IP address\n");
    printf("  mode        shm2net, net2shm, or bridge (default: shm2net)\n");
    printf("\nExample:\n");
    printf("  sudo %s lo 00:00:00:00:00:00 127.0.0.1 shm2net\n", prog);
}

int main(int argc, char* argv[]) {
    printf("╔═══════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║   🌐 Bifrost Gateway Example                                               ║\n");
    printf("║   Bridges local shared memory with network                                    ║\n");
    printf("╚═══════════════════════════════════════════════════════════════════════════════╝\n\n");
    
    if (argc < 4) {
        print_usage(argv[0]);
        return 1;
    }
    
    if (geteuid() != 0) {
        fprintf(stderr, "ERROR: Must run as root for raw socket access\n");
        return 1;
    }
    
    const char* interface = argv[1];
    const char* remote_mac = argv[2];
    const char* remote_ip = argv[3];
    
    /* Parse mode */
    bifrost_gateway_mode_t mode = BIFROST_GATEWAY_SHM_TO_NET;
    if (argc > 4) {
        if (strcmp(argv[4], "net2shm") == 0) {
            mode = BIFROST_GATEWAY_NET_TO_SHM;
        } else if (strcmp(argv[4], "bridge") == 0) {
            mode = BIFROST_GATEWAY_BRIDGE;
        }
    }
    
    const char* mode_str = mode == BIFROST_GATEWAY_SHM_TO_NET ? "SHM → Network" :
                           mode == BIFROST_GATEWAY_NET_TO_SHM ? "Network → SHM" : "Bridge";
    
    /* Setup signals */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* Step 1: Create the pool first (as director) */
    printf("📦 Creating Bifrost pool: %s\n", POOL_NAME);
    bifrost_pool_config_t pool_cfg;
    bifrost_config_init(&pool_cfg);
    bifrost_pool_t* pool = bifrost_create_pool(POOL_NAME, &pool_cfg);
    if (!pool) {
        fprintf(stderr, "ERROR: Cannot create pool\n");
        return 1;
    }
    
    /* Step 2: Start local writer thread (for shm2net mode) */
    pthread_t writer_tid = 0;
    if (mode == BIFROST_GATEWAY_SHM_TO_NET || mode == BIFROST_GATEWAY_BRIDGE) {
        printf("📝 Starting local writer...\n");
        pthread_create(&writer_tid, NULL, writer_thread, NULL);
        usleep(100000);  /* Let writer initialize */
    }
    
    /* Step 3: Create gateway */
    printf("🌐 Creating gateway...\n");
    bifrost_gateway_config_t config = bifrost_gateway_config_default();
    config.interface = interface;
    config.remote_mac = remote_mac;
    config.remote_ip = remote_ip;
    config.pool_name = POOL_NAME;
    config.mode = mode;
    
    g_gateway = bifrost_gateway_create(&config);
    if (!g_gateway) {
        fprintf(stderr, "ERROR: Cannot create gateway\n");
        if (writer_tid) {
            g_running = 0;
            pthread_join(writer_tid, NULL);
        }
        bifrost_destroy_pool(pool);
        return 1;
    }
    
    printf("\n✅ Gateway started!\n");
    printf("📡 Interface:   %s\n", interface);
    printf("🔗 Remote:      %s → %s\n", remote_mac, remote_ip);
    printf("📦 Pool:        %s\n", POOL_NAME);
    printf("🔄 Mode:        %s\n\n", mode_str);
    printf("Press Ctrl+C to stop...\n\n");
    
    /* Step 4: Run gateway with stats */
    uint64_t last_report = get_time_ns();
    bifrost_gateway_stats_t last_stats = {0};
    
    while (g_running) {
        bifrost_gateway_process(g_gateway);
        
        /* Print stats every second */
        uint64_t now = get_time_ns();
        if (now - last_report >= 1000000000ULL) {
            bifrost_gateway_stats_t stats;
            bifrost_gateway_get_stats(g_gateway, &stats);
            
            uint64_t s2n_rate = stats.shm_to_net_count - last_stats.shm_to_net_count;
            uint64_t n2s_rate = stats.net_to_shm_count - last_stats.net_to_shm_count;
            
            printf("\r📊 SHM→NET: %lu (%.0f/s) | NET→SHM: %lu (%.0f/s)    ",
                   stats.shm_to_net_count, (double)s2n_rate,
                   stats.net_to_shm_count, (double)n2s_rate);
            fflush(stdout);
            
            last_stats = stats;
            last_report = now;
        }
    }
    
    /* Cleanup */
    printf("\n\n🧹 Cleaning up...\n");
    
    bifrost_gateway_destroy(g_gateway);
    
    if (writer_tid) {
        pthread_join(writer_tid, NULL);
    }
    
    bifrost_destroy_pool(pool);
    
    printf("👋 Done!\n");
    
    return 0;
}
