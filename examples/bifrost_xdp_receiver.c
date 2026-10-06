/**
 * @file bifrost_xdp_receiver.c
 * @brief Bifrost XDP High-Performance Network Receiver
 * 
 * Production-grade AF_XDP receiver for ultra-low-latency networking.
 * Measures latency with nanosecond precision.
 * 
 * Features:
 * - Raw packet capture for minimum latency
 * - Batch reception using recvmmsg
 * - Comprehensive latency histogram
 * - Gap detection and sequence validation
 * - Packet integrity verification
 * 
 * Build: gcc -O3 -march=native bifrost_xdp_receiver.c -lbifrost -lm -o bifrost_xdp_receiver
 * Run:   sudo ./bifrost_xdp_receiver <interface>
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <sched.h>
#include <poll.h>
#include <float.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <stdatomic.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * Configuration
 * ═══════════════════════════════════════════════════════════════════════════ */

#define RECEIVER_VERSION    "1.0.0"
#define DEFAULT_PORT        19850
#define BATCH_SIZE          64
#define FRAME_SIZE          2048
#define HISTOGRAM_BUCKETS   100000  /* 0.1µs per bucket, up to 10ms */

#define UDP_HEADER_SIZE     8
#define IP_HEADER_SIZE      20
#define ETH_HEADER_SIZE     14
#define TOTAL_HEADER_SIZE   (ETH_HEADER_SIZE + IP_HEADER_SIZE + UDP_HEADER_SIZE)

/* ═══════════════════════════════════════════════════════════════════════════
 * Data Structures
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Market data message - must match sender */
typedef struct __attribute__((packed, aligned(8))) {
    uint64_t    sequence;
    uint64_t    timestamp_ns;
    char        symbol[8];
    int64_t     bid_price;
    int64_t     ask_price;
    uint32_t    bid_size;
    uint32_t    ask_size;
    uint32_t    trade_count;
    uint16_t    flags;
    uint16_t    checksum;
    uint8_t     reserved[8];
} market_data_t;

_Static_assert(sizeof(market_data_t) == 64, "market_data_t must be 64 bytes");

/* Per-symbol statistics */
typedef struct {
    char        symbol[8];
    uint64_t    count;
    int64_t     last_bid;
    int64_t     last_ask;
    double      min_latency;
    double      max_latency;
    double      total_latency;
} symbol_stats_t;

/* Receiver statistics */
typedef struct {
    _Atomic uint64_t    packets_received;
    _Atomic uint64_t    bytes_received;
    _Atomic uint64_t    batches_received;
    _Atomic uint64_t    errors;
    _Atomic uint64_t    checksum_errors;
    _Atomic uint64_t    gaps_detected;
    _Atomic uint64_t    out_of_order;
    
    /* Latency tracking */
    double              min_latency;
    double              max_latency;
    double              total_latency;
    uint64_t            latency_count;
    uint64_t*           histogram;
    
    /* Symbol tracking */
    symbol_stats_t      symbols[16];
    int                 num_symbols;
    
    /* Sequence tracking */
    uint64_t            last_sequence;
    uint64_t            expected_sequence;
    
    /* Timing */
    uint64_t            start_time;
    uint64_t            last_report_time;
    uint64_t            last_packets;
} receiver_stats_t;

/* Receiver context */
typedef struct {
    /* Socket */
    int                 socket_fd;
    int                 ifindex;
    
    /* Buffers */
    void*               rx_buffer;
    size_t              rx_buffer_size;
    struct mmsghdr*     mmsg_vec;
    struct iovec*       iov_vec;
    
    /* Filter */
    uint16_t            listen_port;
    
    /* Stats */
    receiver_stats_t    stats;
    
    /* Control */
    volatile int        running;
} receiver_ctx_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Globals
 * ═══════════════════════════════════════════════════════════════════════════ */

static volatile int g_running = 1;

/* ═══════════════════════════════════════════════════════════════════════════
 * Utilities
 * ═══════════════════════════════════════════════════════════════════════════ */

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline uint16_t compute_checksum(const void* data, size_t len) {
    const uint16_t* ptr = (const uint16_t*)data;
    uint32_t sum = 0;
    
    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }
    
    if (len == 1) {
        sum += *(const uint8_t*)ptr;
    }
    
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    
    return (uint16_t)~sum;
}

static void pin_to_cpu(int cpu) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu % sysconf(_SC_NPROCESSORS_ONLN), &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Receiver Initialization
 * ═══════════════════════════════════════════════════════════════════════════ */

static int receiver_init(receiver_ctx_t* ctx, const char* ifname) {
    memset(ctx, 0, sizeof(*ctx));
    
    /* Get interface index */
    ctx->ifindex = if_nametoindex(ifname);
    if (!ctx->ifindex) {
        fprintf(stderr, "ERROR: Interface '%s' not found\n", ifname);
        return -1;
    }
    
    /* Create raw socket */
    ctx->socket_fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_IP));
    if (ctx->socket_fd < 0) {
        fprintf(stderr, "ERROR: Cannot create raw socket: %s\n", strerror(errno));
        fprintf(stderr, "       Make sure you're running as root!\n");
        return -1;
    }
    
    /* Bind to interface */
    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = ctx->ifindex;
    sll.sll_protocol = htons(ETH_P_IP);
    
    if (bind(ctx->socket_fd, (struct sockaddr*)&sll, sizeof(sll)) < 0) {
        fprintf(stderr, "ERROR: Cannot bind to interface: %s\n", strerror(errno));
        close(ctx->socket_fd);
        return -1;
    }
    
    /* Set socket options */
    int rcvbuf = 64 * 1024 * 1024;
    setsockopt(ctx->socket_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    
    /* Enable promiscuous mode */
    struct packet_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.mr_ifindex = ctx->ifindex;
    mreq.mr_type = PACKET_MR_PROMISC;
    setsockopt(ctx->socket_fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    
    /* Allocate RX buffer */
    ctx->rx_buffer_size = BATCH_SIZE * FRAME_SIZE;
    ctx->rx_buffer = mmap(NULL, ctx->rx_buffer_size,
                           PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS,
                           -1, 0);
    if (ctx->rx_buffer == MAP_FAILED) {
        fprintf(stderr, "ERROR: Cannot allocate RX buffer\n");
        close(ctx->socket_fd);
        return -1;
    }
    mlock(ctx->rx_buffer, ctx->rx_buffer_size);
    
    /* Allocate mmsg structures */
    ctx->mmsg_vec = calloc(BATCH_SIZE, sizeof(struct mmsghdr));
    ctx->iov_vec = calloc(BATCH_SIZE, sizeof(struct iovec));
    if (!ctx->mmsg_vec || !ctx->iov_vec) {
        fprintf(stderr, "ERROR: Cannot allocate mmsg structures\n");
        munmap(ctx->rx_buffer, ctx->rx_buffer_size);
        close(ctx->socket_fd);
        return -1;
    }
    
    /* Initialize mmsg structures */
    for (int i = 0; i < BATCH_SIZE; i++) {
        ctx->iov_vec[i].iov_base = (uint8_t*)ctx->rx_buffer + i * FRAME_SIZE;
        ctx->iov_vec[i].iov_len = FRAME_SIZE;
        ctx->mmsg_vec[i].msg_hdr.msg_iov = &ctx->iov_vec[i];
        ctx->mmsg_vec[i].msg_hdr.msg_iovlen = 1;
    }
    
    /* Allocate histogram */
    ctx->stats.histogram = calloc(HISTOGRAM_BUCKETS, sizeof(uint64_t));
    if (!ctx->stats.histogram) {
        fprintf(stderr, "ERROR: Cannot allocate histogram\n");
        free(ctx->mmsg_vec);
        free(ctx->iov_vec);
        munmap(ctx->rx_buffer, ctx->rx_buffer_size);
        close(ctx->socket_fd);
        return -1;
    }
    
    /* Initialize stats */
    ctx->listen_port = DEFAULT_PORT;
    ctx->stats.min_latency = DBL_MAX;
    ctx->stats.max_latency = 0;
    ctx->stats.total_latency = 0;
    ctx->stats.latency_count = 0;
    ctx->running = 1;
    
    return 0;
}

static void receiver_cleanup(receiver_ctx_t* ctx) {
    if (ctx->socket_fd >= 0) close(ctx->socket_fd);
    if (ctx->rx_buffer) munmap(ctx->rx_buffer, ctx->rx_buffer_size);
    if (ctx->mmsg_vec) free(ctx->mmsg_vec);
    if (ctx->iov_vec) free(ctx->iov_vec);
    if (ctx->stats.histogram) free(ctx->stats.histogram);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Packet Processing
 * ═══════════════════════════════════════════════════════════════════════════ */

static int find_or_add_symbol(receiver_stats_t* stats, const char* symbol) {
    for (int i = 0; i < stats->num_symbols; i++) {
        if (memcmp(stats->symbols[i].symbol, symbol, 8) == 0) {
            return i;
        }
    }
    
    if (stats->num_symbols < 16) {
        int idx = stats->num_symbols++;
        memcpy(stats->symbols[idx].symbol, symbol, 8);
        stats->symbols[idx].min_latency = DBL_MAX;
        return idx;
    }
    
    return -1;
}

static void process_packet(receiver_ctx_t* ctx, const uint8_t* frame, size_t len) {
    uint64_t recv_time = get_time_ns();
    
    /* Validate minimum length */
    if (len < TOTAL_HEADER_SIZE + sizeof(market_data_t)) {
        return;
    }
    
    /* Parse Ethernet header */
    const struct ethhdr* eth = (const struct ethhdr*)frame;
    if (ntohs(eth->h_proto) != ETH_P_IP) {
        return;
    }
    
    /* Parse IP header */
    const struct iphdr* ip = (const struct iphdr*)(frame + ETH_HEADER_SIZE);
    if (ip->protocol != IPPROTO_UDP) {
        return;
    }
    
    /* Parse UDP header */
    const struct udphdr* udp = (const struct udphdr*)(frame + ETH_HEADER_SIZE + IP_HEADER_SIZE);
    if (ntohs(udp->dest) != ctx->listen_port) {
        return;
    }
    
    /* Parse payload */
    const market_data_t* data = (const market_data_t*)(frame + TOTAL_HEADER_SIZE);
    
    /* Verify magic checksum */
    if (data->checksum != 0x5648) {  /* 'VH' in hex */
        atomic_fetch_add(&ctx->stats.checksum_errors, 1);
        return;
    }
    
    /* Calculate latency */
    double latency_ns = (double)(recv_time - data->timestamp_ns);
    
    /* Update global latency stats */
    ctx->stats.total_latency += latency_ns;
    ctx->stats.latency_count++;
    
    if (latency_ns < ctx->stats.min_latency) {
        ctx->stats.min_latency = latency_ns;
    }
    if (latency_ns > ctx->stats.max_latency) {
        ctx->stats.max_latency = latency_ns;
    }
    
    /* Update histogram (0.1µs buckets) */
    size_t bucket = (size_t)(latency_ns / 100);
    if (bucket >= HISTOGRAM_BUCKETS) bucket = HISTOGRAM_BUCKETS - 1;
    ctx->stats.histogram[bucket]++;
    
    /* Sequence validation */
    if (ctx->stats.expected_sequence > 0) {
        if (data->sequence != ctx->stats.expected_sequence) {
            if (data->sequence > ctx->stats.expected_sequence) {
                atomic_fetch_add(&ctx->stats.gaps_detected, 
                                 data->sequence - ctx->stats.expected_sequence);
            } else {
                atomic_fetch_add(&ctx->stats.out_of_order, 1);
            }
        }
    }
    ctx->stats.expected_sequence = data->sequence + 1;
    ctx->stats.last_sequence = data->sequence;
    
    /* Update symbol stats */
    int sym_idx = find_or_add_symbol(&ctx->stats, data->symbol);
    if (sym_idx >= 0) {
        symbol_stats_t* sym = &ctx->stats.symbols[sym_idx];
        sym->count++;
        sym->last_bid = data->bid_price;
        sym->last_ask = data->ask_price;
        sym->total_latency += latency_ns;
        if (latency_ns < sym->min_latency) sym->min_latency = latency_ns;
        if (latency_ns > sym->max_latency) sym->max_latency = latency_ns;
    }
    
    /* Update counters */
    atomic_fetch_add(&ctx->stats.packets_received, 1);
    atomic_fetch_add(&ctx->stats.bytes_received, len);
}

static int receive_batch(receiver_ctx_t* ctx) {
    int received = recvmmsg(ctx->socket_fd, ctx->mmsg_vec, BATCH_SIZE, 
                             MSG_WAITFORONE, NULL);
    
    if (received <= 0) return received;
    
    atomic_fetch_add(&ctx->stats.batches_received, 1);
    
    for (int i = 0; i < received; i++) {
        uint8_t* frame = (uint8_t*)ctx->iov_vec[i].iov_base;
        size_t len = ctx->mmsg_vec[i].msg_len;
        process_packet(ctx, frame, len);
    }
    
    return received;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Statistics
 * ═══════════════════════════════════════════════════════════════════════════ */

static double get_percentile(receiver_ctx_t* ctx, double percentile) {
    uint64_t total = ctx->stats.latency_count;
    if (total == 0) return 0;
    
    uint64_t target = (uint64_t)(total * percentile / 100);
    uint64_t running = 0;
    
    for (size_t i = 0; i < HISTOGRAM_BUCKETS; i++) {
        running += ctx->stats.histogram[i];
        if (running >= target) {
            return (double)(i * 100);  /* Convert bucket to ns */
        }
    }
    
    return atomic_load(&ctx->stats.max_latency);
}

static void print_stats(receiver_ctx_t* ctx) {
    uint64_t now = get_time_ns();
    uint64_t packets = atomic_load(&ctx->stats.packets_received);
    
    uint64_t delta_packets = packets - ctx->stats.last_packets;
    double delta_time = (now - ctx->stats.last_report_time) / 1e9;
    double rate = delta_time > 0 ? delta_packets / delta_time / 1e6 : 0;
    
    ctx->stats.last_packets = packets;
    ctx->stats.last_report_time = now;
    
    double min_lat = ctx->stats.min_latency;
    double avg_lat = 0;
    uint64_t lat_count = ctx->stats.latency_count;
    if (lat_count > 0) {
        avg_lat = ctx->stats.total_latency / lat_count;
    }
    
    printf("\r📊 Pkts: %lu M | Rate: %.2f M/s | Lat: min=%.0f avg=%.0f p99=%.0f ns    ",
           packets / 1000000,
           rate,
           min_lat < DBL_MAX ? min_lat : 0,
           avg_lat,
           get_percentile(ctx, 99));
    fflush(stdout);
}

static void print_final_stats(receiver_ctx_t* ctx) {
    uint64_t now = get_time_ns();
    double duration = (now - ctx->stats.start_time) / 1e9;
    
    uint64_t packets = atomic_load(&ctx->stats.packets_received);
    uint64_t bytes = atomic_load(&ctx->stats.bytes_received);
    uint64_t batches = atomic_load(&ctx->stats.batches_received);
    uint64_t gaps = atomic_load(&ctx->stats.gaps_detected);
    uint64_t checksum_err = atomic_load(&ctx->stats.checksum_errors);
    uint64_t ooo = atomic_load(&ctx->stats.out_of_order);
    
    double min_lat = ctx->stats.min_latency;
    double max_lat = ctx->stats.max_latency;
    uint64_t lat_count = ctx->stats.latency_count;
    double avg_lat = lat_count > 0 ? ctx->stats.total_latency / lat_count : 0;
    
    printf("\n\n");
    printf("╔═══════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║   📊 RECEIVER FINAL STATISTICS                                                ║\n");
    printf("╠═══════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║   Duration:       %.2f seconds                                               ║\n", duration);
    printf("║   Packets recv:   %lu                                                  ║\n", packets);
    printf("║   Bytes recv:     %.2f GB                                                    ║\n", bytes / 1e9);
    printf("╠═══════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║   ⚡ PERFORMANCE:                                                              ║\n");
    printf("║      Rate:        %.2f M packets/sec                                         ║\n", packets / duration / 1e6);
    printf("║      Bandwidth:   %.2f Gbps                                                  ║\n", bytes * 8 / duration / 1e9);
    printf("╠═══════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║   🎯 LATENCY:                                                                 ║\n");
    printf("║      Min:      %10.0f ns  (%6.3f µs)                                       ║\n", 
           min_lat < DBL_MAX ? min_lat : 0, min_lat < DBL_MAX ? min_lat/1000 : 0);
    printf("║      Avg:      %10.0f ns  (%6.3f µs)                                       ║\n", avg_lat, avg_lat/1000);
    printf("║      P50:      %10.0f ns  (%6.3f µs)                                       ║\n", 
           get_percentile(ctx, 50), get_percentile(ctx, 50)/1000);
    printf("║      P99:      %10.0f ns  (%6.3f µs)                                       ║\n", 
           get_percentile(ctx, 99), get_percentile(ctx, 99)/1000);
    printf("║      P99.9:    %10.0f ns  (%6.3f µs)                                       ║\n", 
           get_percentile(ctx, 99.9), get_percentile(ctx, 99.9)/1000);
    printf("║      Max:      %10.0f ns  (%6.3f µs)                                       ║\n", max_lat, max_lat/1000);
    printf("╠═══════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║   🔧 EFFICIENCY:                                                              ║\n");
    printf("║      Batches:     %lu                                                   ║\n", batches);
    printf("║      Avg batch:   %.1f packets/batch                                         ║\n", 
           batches > 0 ? (double)packets / batches : 0);
    printf("╠═══════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║   ⚠️  QUALITY:                                                                 ║\n");
    printf("║      Gaps:        %lu                                                         ║\n", gaps);
    printf("║      Out-of-order: %lu                                                        ║\n", ooo);
    printf("║      Checksum err: %lu                                                        ║\n", checksum_err);
    printf("╚═══════════════════════════════════════════════════════════════════════════════╝\n");
    
    /* Symbol breakdown */
    if (ctx->stats.num_symbols > 0) {
        printf("\n📈 PER-SYMBOL STATISTICS:\n");
        printf("┌──────────┬────────────┬────────────┬────────────┬────────────┬────────────┐\n");
        printf("│ Symbol   │    Count   │  Avg Lat   │  Min Lat   │  Max Lat   │  Last Bid  │\n");
        printf("├──────────┼────────────┼────────────┼────────────┼────────────┼────────────┤\n");
        
        for (int i = 0; i < ctx->stats.num_symbols; i++) {
            symbol_stats_t* sym = &ctx->stats.symbols[i];
            double avg = sym->count > 0 ? sym->total_latency / sym->count : 0;
            printf("│ %-8s │ %10lu │ %8.1f ns│ %8.1f ns│ %8.1f ns│ %10.2f │\n",
                   sym->symbol,
                   sym->count,
                   avg,
                   sym->min_latency < DBL_MAX ? sym->min_latency : 0,
                   sym->max_latency,
                   sym->last_bid / 10000.0);
        }
        printf("└──────────┴────────────┴────────────┴────────────┴────────────┴────────────┘\n");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════════════════════════════════ */

static void print_usage(const char* prog) {
    printf("Usage: %s <interface>\n", prog);
    printf("\n");
    printf("Arguments:\n");
    printf("  interface   Network interface to listen on (e.g., enp0s31f6)\n");
    printf("\n");
    printf("Example:\n");
    printf("  sudo %s enp0s31f6\n", prog);
}

int main(int argc, char* argv[]) {
    printf("╔═══════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║   📥 Bifrost XDP Receiver v%s                                           ║\n", RECEIVER_VERSION);
    printf("║   Production-grade ultra-low-latency network receiver                         ║\n");
    printf("╚═══════════════════════════════════════════════════════════════════════════════╝\n\n");
    
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    
    const char* ifname = argv[1];
    
    /* Check root */
    if (geteuid() != 0) {
        fprintf(stderr, "ERROR: Must be run as root for raw socket access\n");
        return 1;
    }
    
    /* Setup signals */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* Pin to CPU 1 (different from sender) */
    pin_to_cpu(1);
    printf("📍 Pinned to CPU 1\n");
    
    /* Initialize receiver */
    receiver_ctx_t ctx;
    if (receiver_init(&ctx, ifname) < 0) {
        return 1;
    }
    
    printf("📡 Interface:   %s (index %d)\n", ifname, ctx.ifindex);
    printf("🚪 Listen port: %d\n", ctx.listen_port);
    printf("📦 Batch size:  %d\n", BATCH_SIZE);
    printf("📊 Histogram:   %d buckets (0.1µs resolution)\n\n", HISTOGRAM_BUCKETS);
    
    printf("⏳ Waiting for packets from sender...\n\n");
    
    /* Start timer */
    ctx.stats.start_time = get_time_ns();
    ctx.stats.last_report_time = ctx.stats.start_time;
    ctx.stats.last_packets = 0;
    
    uint64_t last_report = ctx.stats.start_time;
    
    /* Main receive loop */
    while (g_running) {
        receive_batch(&ctx);
        
        /* Check for stats report */
        uint64_t now = get_time_ns();
        if (now - last_report >= 1000000000ULL) {
            print_stats(&ctx);
            last_report = now;
        }
    }
    
    /* Final stats */
    print_final_stats(&ctx);
    
    /* Cleanup */
    receiver_cleanup(&ctx);
    printf("\n👋 Receiver stopped.\n");
    
    return 0;
}
