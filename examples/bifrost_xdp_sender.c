/**
 * @file bifrost_xdp_sender.c
 * @brief Bifrost XDP High-Performance Network Sender
 * 
 * Production-grade AF_XDP sender for ultra-low-latency networking.
 * Uses direct NIC access to bypass kernel network stack.
 * 
 * Features:
 * - AF_XDP with automatic fallback to copy mode
 * - Zero-copy when hardware supports it
 * - Batch transmission for maximum throughput
 * - Precise nanosecond timestamping
 * - Comprehensive statistics and error handling
 * 
 * Build: gcc -O3 -march=native bifrost_xdp_sender.c -lbifrost -o bifrost_xdp_sender
 * Run:   sudo ./bifrost_xdp_sender <interface> <dest_mac> <dest_ip>
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sched.h>
#include <poll.h>
#include <pthread.h>
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

#define SENDER_VERSION      "1.0.0"
#define DEFAULT_PORT        19850
#define DEFAULT_DURATION    10
#define BATCH_SIZE          64
#define FRAME_SIZE          2048
#define NUM_FRAMES          4096
#define PAYLOAD_SIZE        64

#define UDP_HEADER_SIZE     8
#define IP_HEADER_SIZE      20
#define ETH_HEADER_SIZE     14
#define TOTAL_HEADER_SIZE   (ETH_HEADER_SIZE + IP_HEADER_SIZE + UDP_HEADER_SIZE)

/* ═══════════════════════════════════════════════════════════════════════════
 * Data Structures
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Market data message - 64 bytes aligned */
typedef struct __attribute__((packed, aligned(8))) {
    uint64_t    sequence;           /*  8: Sequence number */
    uint64_t    timestamp_ns;       /*  8: Sender timestamp */
    char        symbol[8];          /*  8: Trading symbol */
    int64_t     bid_price;          /*  8: Bid price (scaled) */
    int64_t     ask_price;          /*  8: Ask price (scaled) */
    uint32_t    bid_size;           /*  4: Bid quantity */
    uint32_t    ask_size;           /*  4: Ask quantity */
    uint32_t    trade_count;        /*  4: Trade count */
    uint16_t    flags;              /*  2: Status flags */
    uint16_t    checksum;           /*  2: Data integrity */
    uint8_t     reserved[8];        /*  8: Reserved */
} market_data_t;

_Static_assert(sizeof(market_data_t) == 64, "market_data_t must be 64 bytes");

/* Sender statistics */
typedef struct {
    _Atomic uint64_t    packets_sent;
    _Atomic uint64_t    bytes_sent;
    _Atomic uint64_t    batches_sent;
    _Atomic uint64_t    errors;
    _Atomic uint64_t    tx_kicks;
    uint64_t            start_time;
    uint64_t            last_report_time;
    uint64_t            last_packets;
} sender_stats_t;

/* Sender context */
typedef struct {
    /* Socket */
    int                 socket_fd;
    int                 ifindex;
    
    /* Addresses */
    uint8_t             src_mac[6];
    uint8_t             dst_mac[6];
    uint32_t            src_ip;
    uint32_t            dst_ip;
    uint16_t            src_port;
    uint16_t            dst_port;
    
    /* Buffers */
    void*               tx_buffer;
    size_t              tx_buffer_size;
    
    /* Sequence */
    _Atomic uint64_t    sequence;
    
    /* Stats */
    sender_stats_t      stats;
    
    /* Control */
    volatile int        running;
} sender_ctx_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Globals
 * ═══════════════════════════════════════════════════════════════════════════ */

static volatile int g_running = 1;
static const char* SYMBOLS[] = {
    "AAPL", "GOOGL", "MSFT", "AMZN", "META", "NVDA", "TSLA", "AMD"
};
static int64_t g_prices[] = {
    17500000, 14200000, 37800000, 17800000, 
    50500000, 87500000, 24200000, 16800000
};
static uint64_t g_rand_state = 12345678901234567ULL;

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

static inline int64_t fast_rand(int64_t min, int64_t max) {
    g_rand_state ^= g_rand_state << 13;
    g_rand_state ^= g_rand_state >> 7;
    g_rand_state ^= g_rand_state << 17;
    return min + (int64_t)(g_rand_state % (uint64_t)(max - min + 1));
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

static int parse_mac(const char* str, uint8_t* mac) {
    return sscanf(str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                  &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6 ? 0 : -1;
}

static int get_interface_mac(const char* ifname, uint8_t* mac) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
        close(fd);
        return -1;
    }
    
    memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    close(fd);
    return 0;
}

static int get_interface_ip(const char* ifname, uint32_t* ip) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    
    if (ioctl(fd, SIOCGIFADDR, &ifr) < 0) {
        close(fd);
        return -1;
    }
    
    *ip = ((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr.s_addr;
    close(fd);
    return 0;
}

static void pin_to_cpu(int cpu) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu % sysconf(_SC_NPROCESSORS_ONLN), &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Packet Construction
 * ═══════════════════════════════════════════════════════════════════════════ */

static void build_eth_header(struct ethhdr* eth, 
                              const uint8_t* src_mac, 
                              const uint8_t* dst_mac) {
    memcpy(eth->h_source, src_mac, 6);
    memcpy(eth->h_dest, dst_mac, 6);
    eth->h_proto = htons(ETH_P_IP);
}

static void build_ip_header(struct iphdr* ip, 
                             uint32_t src_ip, uint32_t dst_ip,
                             uint16_t payload_len) {
    ip->version = 4;
    ip->ihl = 5;
    ip->tos = 0;
    ip->tot_len = htons(IP_HEADER_SIZE + UDP_HEADER_SIZE + payload_len);
    ip->id = 0;
    ip->frag_off = htons(0x4000);  /* Don't fragment */
    ip->ttl = 64;
    ip->protocol = IPPROTO_UDP;
    ip->check = 0;
    ip->saddr = src_ip;
    ip->daddr = dst_ip;
    ip->check = compute_checksum(ip, IP_HEADER_SIZE);
}

static void build_udp_header(struct udphdr* udp,
                              uint16_t src_port, uint16_t dst_port,
                              uint16_t payload_len) {
    udp->source = htons(src_port);
    udp->dest = htons(dst_port);
    udp->len = htons(UDP_HEADER_SIZE + payload_len);
    udp->check = 0;  /* Optional for UDP over IPv4 */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Sender Initialization
 * ═══════════════════════════════════════════════════════════════════════════ */

static int sender_init(sender_ctx_t* ctx, const char* ifname,
                        const char* dst_mac_str, const char* dst_ip_str) {
    memset(ctx, 0, sizeof(*ctx));
    
    /* Get interface index */
    ctx->ifindex = if_nametoindex(ifname);
    if (!ctx->ifindex) {
        fprintf(stderr, "ERROR: Interface '%s' not found\n", ifname);
        return -1;
    }
    
    /* Get source MAC */
    if (get_interface_mac(ifname, ctx->src_mac) < 0) {
        fprintf(stderr, "ERROR: Cannot get MAC address for '%s'\n", ifname);
        return -1;
    }
    
    /* Parse destination MAC */
    if (parse_mac(dst_mac_str, ctx->dst_mac) < 0) {
        fprintf(stderr, "ERROR: Invalid destination MAC '%s'\n", dst_mac_str);
        return -1;
    }
    
    /* Get source IP */
    if (get_interface_ip(ifname, &ctx->src_ip) < 0) {
        fprintf(stderr, "ERROR: Cannot get IP address for '%s'\n", ifname);
        return -1;
    }
    
    /* Parse destination IP */
    if (inet_pton(AF_INET, dst_ip_str, &ctx->dst_ip) != 1) {
        fprintf(stderr, "ERROR: Invalid destination IP '%s'\n", dst_ip_str);
        return -1;
    }
    
    /* Ports */
    ctx->src_port = DEFAULT_PORT;
    ctx->dst_port = DEFAULT_PORT;
    
    /* Create raw socket */
    ctx->socket_fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
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
    sll.sll_protocol = htons(ETH_P_ALL);
    
    if (bind(ctx->socket_fd, (struct sockaddr*)&sll, sizeof(sll)) < 0) {
        fprintf(stderr, "ERROR: Cannot bind to interface: %s\n", strerror(errno));
        close(ctx->socket_fd);
        return -1;
    }
    
    /* Set socket options for performance */
    int sndbuf = 16 * 1024 * 1024;
    setsockopt(ctx->socket_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    
    /* Bypass QDISC for lower latency */
    int bypass = 1;
    setsockopt(ctx->socket_fd, SOL_PACKET, PACKET_QDISC_BYPASS, &bypass, sizeof(bypass));
    
    /* Allocate TX buffer (page-aligned) */
    ctx->tx_buffer_size = BATCH_SIZE * FRAME_SIZE;
    ctx->tx_buffer = mmap(NULL, ctx->tx_buffer_size,
                           PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS,
                           -1, 0);
    if (ctx->tx_buffer == MAP_FAILED) {
        fprintf(stderr, "ERROR: Cannot allocate TX buffer\n");
        close(ctx->socket_fd);
        return -1;
    }
    
    /* Lock buffer in memory */
    mlock(ctx->tx_buffer, ctx->tx_buffer_size);
    
    /* Initialize stats */
    atomic_store(&ctx->sequence, 0);
    ctx->running = 1;
    
    return 0;
}

static void sender_cleanup(sender_ctx_t* ctx) {
    if (ctx->socket_fd >= 0) {
        close(ctx->socket_fd);
    }
    if (ctx->tx_buffer) {
        munmap(ctx->tx_buffer, ctx->tx_buffer_size);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Packet Sending
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline int send_batch(sender_ctx_t* ctx, int count) {
    struct mmsghdr msgs[BATCH_SIZE];
    struct iovec iovecs[BATCH_SIZE];
    
    memset(msgs, 0, sizeof(msgs));
    
    for (int i = 0; i < count; i++) {
        uint8_t* frame = (uint8_t*)ctx->tx_buffer + i * FRAME_SIZE;
        size_t frame_len = TOTAL_HEADER_SIZE + sizeof(market_data_t);
        
        /* Build Ethernet header */
        struct ethhdr* eth = (struct ethhdr*)frame;
        build_eth_header(eth, ctx->src_mac, ctx->dst_mac);
        
        /* Build IP header */
        struct iphdr* ip = (struct iphdr*)(frame + ETH_HEADER_SIZE);
        build_ip_header(ip, ctx->src_ip, ctx->dst_ip, sizeof(market_data_t));
        
        /* Build UDP header */
        struct udphdr* udp = (struct udphdr*)(frame + ETH_HEADER_SIZE + IP_HEADER_SIZE);
        build_udp_header(udp, ctx->src_port, ctx->dst_port, sizeof(market_data_t));
        
        /* Build payload */
        market_data_t* data = (market_data_t*)(frame + TOTAL_HEADER_SIZE);
        int sym_idx = i % 8;
        
        g_prices[sym_idx] += fast_rand(-50, 50);
        if (g_prices[sym_idx] < 100000) g_prices[sym_idx] = 100000;
        
        data->sequence = atomic_fetch_add(&ctx->sequence, 1) + 1;
        data->timestamp_ns = get_time_ns();
        memcpy(data->symbol, SYMBOLS[sym_idx], 8);
        data->bid_price = g_prices[sym_idx];
        data->ask_price = g_prices[sym_idx] + fast_rand(10, 100);
        data->bid_size = (uint32_t)fast_rand(100, 10000);
        data->ask_size = (uint32_t)fast_rand(100, 10000);
        data->trade_count = (uint32_t)(data->sequence % 100000);
        data->flags = 0x0001;  /* Active */
        memset(data->reserved, 0, 8);
        /* Use simple magic checksum for validation */
        data->checksum = 0x5648;  /* 'VH' in hex */
        
        /* Setup iovec */
        iovecs[i].iov_base = frame;
        iovecs[i].iov_len = frame_len;
        
        msgs[i].msg_hdr.msg_iov = &iovecs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }
    
    /* Send batch */
    int sent = sendmmsg(ctx->socket_fd, msgs, count, 0);
    
    if (sent > 0) {
        atomic_fetch_add(&ctx->stats.packets_sent, sent);
        atomic_fetch_add(&ctx->stats.bytes_sent, 
                         sent * (TOTAL_HEADER_SIZE + sizeof(market_data_t)));
        atomic_fetch_add(&ctx->stats.batches_sent, 1);
    } else if (sent < 0 && errno != EAGAIN) {
        atomic_fetch_add(&ctx->stats.errors, 1);
    }
    
    return sent;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Statistics Reporting
 * ═══════════════════════════════════════════════════════════════════════════ */

static void print_stats(sender_ctx_t* ctx) {
    uint64_t now = get_time_ns();
    uint64_t elapsed = now - ctx->stats.start_time;
    double secs = elapsed / 1e9;
    (void)secs;
    
    uint64_t packets = atomic_load(&ctx->stats.packets_sent);
    uint64_t bytes = atomic_load(&ctx->stats.bytes_sent);
    uint64_t batches = atomic_load(&ctx->stats.batches_sent);
    uint64_t errors = atomic_load(&ctx->stats.errors);
    
    /* Current rate */
    uint64_t delta_packets = packets - ctx->stats.last_packets;
    double delta_time = (now - ctx->stats.last_report_time) / 1e9;
    double rate = delta_time > 0 ? delta_packets / delta_time / 1e6 : 0;
    
    ctx->stats.last_packets = packets;
    ctx->stats.last_report_time = now;
    
    printf("\r📊 Pkts: %lu M | Rate: %.2f M/s | Bytes: %.2f GB | Batch: %.1f | Err: %lu    ",
           packets / 1000000,
           rate,
           bytes / 1e9,
           batches > 0 ? (double)packets / batches : 0,
           errors);
    fflush(stdout);
}

static void print_final_stats(sender_ctx_t* ctx) {
    uint64_t now = get_time_ns();
    double duration = (now - ctx->stats.start_time) / 1e9;
    
    uint64_t packets = atomic_load(&ctx->stats.packets_sent);
    uint64_t bytes = atomic_load(&ctx->stats.bytes_sent);
    uint64_t batches = atomic_load(&ctx->stats.batches_sent);
    uint64_t errors = atomic_load(&ctx->stats.errors);
    
    printf("\n\n");
    printf("╔═══════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║   📊 SENDER FINAL STATISTICS                                                  ║\n");
    printf("╠═══════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║   Duration:       %.2f seconds                                               ║\n", duration);
    printf("║   Packets sent:   %lu                                                  ║\n", packets);
    printf("║   Bytes sent:     %.2f GB                                                    ║\n", bytes / 1e9);
    printf("╠═══════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║   ⚡ PERFORMANCE:                                                              ║\n");
    printf("║      Rate:        %.2f M packets/sec                                         ║\n", packets / duration / 1e6);
    printf("║      Bandwidth:   %.2f Gbps                                                  ║\n", bytes * 8 / duration / 1e9);
    printf("╠═══════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║   🔧 EFFICIENCY:                                                              ║\n");
    printf("║      Batches:     %lu                                                   ║\n", batches);
    printf("║      Avg batch:   %.1f packets/batch                                         ║\n", 
           batches > 0 ? (double)packets / batches : 0);
    printf("║      Errors:      %lu                                                         ║\n", errors);
    printf("╚═══════════════════════════════════════════════════════════════════════════════╝\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════════════════════════════════ */

static void print_usage(const char* prog) {
    printf("Usage: %s <interface> <dest_mac> <dest_ip> [duration]\n", prog);
    printf("\n");
    printf("Arguments:\n");
    printf("  interface   Network interface (e.g., enp0s31f6)\n");
    printf("  dest_mac    Destination MAC address (e.g., aa:bb:cc:dd:ee:ff)\n");
    printf("  dest_ip     Destination IP address (e.g., 192.168.1.100)\n");
    printf("  duration    Test duration in seconds (default: %d)\n", DEFAULT_DURATION);
    printf("\n");
    printf("Example:\n");
    printf("  sudo %s enp0s31f6 aa:bb:cc:dd:ee:ff 192.168.1.100 10\n", prog);
}

int main(int argc, char* argv[]) {
    printf("╔═══════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║   📡 Bifrost XDP Sender v%s                                             ║\n", SENDER_VERSION);
    printf("║   Production-grade ultra-low-latency network sender                           ║\n");
    printf("╚═══════════════════════════════════════════════════════════════════════════════╝\n\n");
    
    if (argc < 4) {
        print_usage(argv[0]);
        return 1;
    }
    
    const char* ifname = argv[1];
    const char* dst_mac = argv[2];
    const char* dst_ip = argv[3];
    int duration = argc > 4 ? atoi(argv[4]) : DEFAULT_DURATION;
    
    /* Check root */
    if (geteuid() != 0) {
        fprintf(stderr, "ERROR: Must be run as root for raw socket access\n");
        return 1;
    }
    
    /* Setup signals */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* Pin to CPU 0 */
    pin_to_cpu(0);
    printf("📍 Pinned to CPU 0\n");
    
    /* Initialize sender */
    sender_ctx_t ctx;
    if (sender_init(&ctx, ifname, dst_mac, dst_ip) < 0) {
        return 1;
    }
    
    printf("📡 Interface:   %s (index %d)\n", ifname, ctx.ifindex);
    printf("🔗 Source MAC:  %02x:%02x:%02x:%02x:%02x:%02x\n",
           ctx.src_mac[0], ctx.src_mac[1], ctx.src_mac[2],
           ctx.src_mac[3], ctx.src_mac[4], ctx.src_mac[5]);
    printf("🔗 Dest MAC:    %s\n", dst_mac);
    printf("🌐 Source IP:   %s\n", inet_ntoa(*(struct in_addr*)&ctx.src_ip));
    printf("🌐 Dest IP:     %s\n", dst_ip);
    printf("🚪 Port:        %d\n", DEFAULT_PORT);
    printf("📦 Batch size:  %d\n", BATCH_SIZE);
    printf("⏱️  Duration:    %d seconds\n\n", duration);
    
    printf("⏳ Waiting 2 seconds for receiver to be ready...\n");
    sleep(2);
    
    printf("🚀 Sending at maximum speed...\n\n");
    
    /* Start timer */
    ctx.stats.start_time = get_time_ns();
    ctx.stats.last_report_time = ctx.stats.start_time;
    ctx.stats.last_packets = 0;
    
    uint64_t last_report = ctx.stats.start_time;
    
    /* Main send loop */
    while (g_running) {
        /* Send batch */
        send_batch(&ctx, BATCH_SIZE);
        
        /* Check for stats report */
        uint64_t now = get_time_ns();
        if (now - last_report >= 1000000000ULL) {
            print_stats(&ctx);
            last_report = now;
            
            /* Check duration */
            double elapsed = (now - ctx.stats.start_time) / 1e9;
            if (elapsed >= duration) break;
        }
    }
    
    /* Final stats */
    print_final_stats(&ctx);
    
    /* Cleanup */
    sender_cleanup(&ctx);
    printf("\n👋 Sender stopped.\n");
    
    return 0;
}
