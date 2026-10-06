/**
 * @file bifrost_gateway.c
 * @brief Bifrost Network Gateway Implementation
 * 
 * Bridges Bifrost shared memory with network protocol.
 * Part of the Bifrost core library.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sched.h>
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

#include "bifrost_gateway.h"
#include "bifrost.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * Constants
 * ═══════════════════════════════════════════════════════════════════════════ */

#define DEFAULT_PORT        19850
#define DEFAULT_BATCH_SIZE  64
#define FRAME_SIZE          2048
#define SLOT_SIZE           64

#define UDP_HEADER_SIZE     8
#define IP_HEADER_SIZE      20
#define ETH_HEADER_SIZE     14
#define TOTAL_HEADER_SIZE   (ETH_HEADER_SIZE + IP_HEADER_SIZE + UDP_HEADER_SIZE)

/* ═══════════════════════════════════════════════════════════════════════════
 * Internal Structures
 * ═══════════════════════════════════════════════════════════════════════════ */

struct bifrost_gateway {
    /* Bifrost handles */
    bifrost_writer_t*        writer;
    bifrost_reader_t*        reader;
    
    /* Network socket */
    int                 socket_fd;
    int                 ifindex;
    
    /* Addresses */
    uint8_t             src_mac[6];
    uint8_t             dst_mac[6];
    uint32_t            src_ip;
    uint32_t            dst_ip;
    uint16_t            port;
    
    /* Buffers */
    void*               tx_buffer;
    void*               rx_buffer;
    size_t              buffer_size;
    int                 batch_size;
    
    /* Mode */
    bifrost_gateway_mode_t   mode;
    
    /* Stats */
    _Atomic uint64_t    shm_to_net_count;
    _Atomic uint64_t    net_to_shm_count;
    _Atomic uint64_t    errors;
    uint64_t            start_time;
    
    /* State tracking */
    uint64_t            writer_versions[1024];  /* Last seen version for each writer ID */

    /* Control */
    volatile int        running;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Utilities
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int parse_mac(const char* str, uint8_t* mac) {
    return sscanf(str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                  &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6 ? 0 : -1;
}

static int get_interface_info(const char* ifname, uint8_t* mac, uint32_t* ip) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) >= 0) {
        memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    }
    
    if (ioctl(fd, SIOCGIFADDR, &ifr) >= 0) {
        *ip = ((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr.s_addr;
    }
    
    close(fd);
    return 0;
}

static inline uint16_t compute_checksum(const void* data, size_t len) {
    const uint16_t* ptr = (const uint16_t*)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *ptr++; len -= 2; }
    if (len == 1) sum += *(const uint8_t*)ptr;
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    return (uint16_t)~sum;
}

static void build_packet_headers(uint8_t* frame, bifrost_gateway_t* gw, uint16_t payload_len) {
    struct ethhdr* eth = (struct ethhdr*)frame;
    memcpy(eth->h_source, gw->src_mac, 6);
    memcpy(eth->h_dest, gw->dst_mac, 6);
    eth->h_proto = htons(ETH_P_IP);
    
    struct iphdr* ip = (struct iphdr*)(frame + ETH_HEADER_SIZE);
    ip->version = 4;
    ip->ihl = 5;
    ip->tos = 0;
    ip->tot_len = htons(IP_HEADER_SIZE + UDP_HEADER_SIZE + payload_len);
    ip->id = 0;
    ip->frag_off = htons(0x4000);
    ip->ttl = 64;
    ip->protocol = IPPROTO_UDP;
    ip->saddr = gw->src_ip;
    ip->daddr = gw->dst_ip;
    ip->check = 0;
    ip->check = compute_checksum(ip, IP_HEADER_SIZE);
    
    struct udphdr* udp = (struct udphdr*)(frame + ETH_HEADER_SIZE + IP_HEADER_SIZE);
    udp->source = htons(gw->port);
    udp->dest = htons(gw->port);
    udp->len = htons(UDP_HEADER_SIZE + payload_len);
    udp->check = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

bifrost_gateway_config_t bifrost_gateway_config_default(void) {
    bifrost_gateway_config_t config = {
        .interface = "lo",
        .remote_mac = "00:00:00:00:00:00",
        .remote_ip = "127.0.0.1",
        .pool_name = "bifrost_market_data",
        .port = DEFAULT_PORT,
        .mode = BIFROST_GATEWAY_BRIDGE,
        .batch_size = DEFAULT_BATCH_SIZE
    };
    return config;
}

bifrost_gateway_t* bifrost_gateway_create(const bifrost_gateway_config_t* config) {
    if (!config || !config->interface || !config->remote_mac || 
        !config->remote_ip || !config->pool_name) {
        return NULL;
    }
    
    bifrost_gateway_t* gw = calloc(1, sizeof(bifrost_gateway_t));
    if (!gw) return NULL;
    
    gw->mode = config->mode;
    gw->port = config->port ? config->port : DEFAULT_PORT;
    gw->batch_size = config->batch_size > 0 ? config->batch_size : DEFAULT_BATCH_SIZE;
    gw->socket_fd = -1;
    
    /* Get interface info */
    gw->ifindex = if_nametoindex(config->interface);
    if (!gw->ifindex) {
        fprintf(stderr, "BIFROST_GATEWAY: Interface '%s' not found\n", config->interface);
        free(gw);
        return NULL;
    }
    
    get_interface_info(config->interface, gw->src_mac, &gw->src_ip);
    
    if (parse_mac(config->remote_mac, gw->dst_mac) < 0) {
        fprintf(stderr, "BIFROST_GATEWAY: Invalid remote MAC '%s'\n", config->remote_mac);
        free(gw);
        return NULL;
    }
    
    if (inet_pton(AF_INET, config->remote_ip, &gw->dst_ip) != 1) {
        fprintf(stderr, "BIFROST_GATEWAY: Invalid remote IP '%s'\n", config->remote_ip);
        free(gw);
        return NULL;
    }
    
    /* Create raw socket */
    gw->socket_fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (gw->socket_fd < 0) {
        fprintf(stderr, "BIFROST_GATEWAY: Cannot create raw socket (need root)\n");
        free(gw);
        return NULL;
    }
    
    struct sockaddr_ll sll = {
        .sll_family = AF_PACKET,
        .sll_ifindex = gw->ifindex,
        .sll_protocol = htons(ETH_P_ALL)
    };
    bind(gw->socket_fd, (struct sockaddr*)&sll, sizeof(sll));
    
    /* Socket options */
    int bufsize = 16 * 1024 * 1024;
    setsockopt(gw->socket_fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    setsockopt(gw->socket_fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    
    int bypass = 1;
    setsockopt(gw->socket_fd, SOL_PACKET, PACKET_QDISC_BYPASS, &bypass, sizeof(bypass));
    
    /* Initialize Bifrost */
    if (gw->mode == BIFROST_GATEWAY_SHM_TO_NET || gw->mode == BIFROST_GATEWAY_BRIDGE) {
        gw->reader = bifrost_join_as_reader(config->pool_name, "gateway_reader");
        if (!gw->reader) {
            fprintf(stderr, "BIFROST_GATEWAY: Cannot join pool '%s' as reader\n", config->pool_name);
            close(gw->socket_fd);
            free(gw);
            return NULL;
        }
    }
    
    if (gw->mode == BIFROST_GATEWAY_NET_TO_SHM || gw->mode == BIFROST_GATEWAY_BRIDGE) {
        gw->writer = bifrost_join_as_writer(config->pool_name, "gateway_writer");
        if (!gw->writer) {
            fprintf(stderr, "BIFROST_GATEWAY: Cannot join pool '%s' as writer\n", config->pool_name);
            if (gw->reader) bifrost_reader_leave(gw->reader);
            close(gw->socket_fd);
            free(gw);
            return NULL;
        }
    }
    
    /* Allocate buffers */
    gw->buffer_size = gw->batch_size * FRAME_SIZE;
    gw->tx_buffer = mmap(NULL, gw->buffer_size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    gw->rx_buffer = mmap(NULL, gw->buffer_size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    if (gw->tx_buffer != MAP_FAILED) mlock(gw->tx_buffer, gw->buffer_size);
    if (gw->rx_buffer != MAP_FAILED) mlock(gw->rx_buffer, gw->buffer_size);
    
    gw->running = 1;
    gw->start_time = get_time_ns();
    
    return gw;
}

void bifrost_gateway_destroy(bifrost_gateway_t* gw) {
    if (!gw) return;
    
    gw->running = 0;
    
    if (gw->reader) bifrost_reader_leave(gw->reader);
    if (gw->writer) bifrost_writer_leave(gw->writer);
    if (gw->socket_fd >= 0) close(gw->socket_fd);
    if (gw->tx_buffer && gw->tx_buffer != MAP_FAILED) munmap(gw->tx_buffer, gw->buffer_size);
    if (gw->rx_buffer && gw->rx_buffer != MAP_FAILED) munmap(gw->rx_buffer, gw->buffer_size);
    
    free(gw);
}

int bifrost_gateway_process(bifrost_gateway_t* gw) {
    if (!gw || !gw->running) return -1;
    
    int processed = 0;
    
    /* SHM → Network */
    if ((gw->mode == BIFROST_GATEWAY_SHM_TO_NET || gw->mode == BIFROST_GATEWAY_BRIDGE) && gw->reader) {
        bifrost_writer_info_t writers[32];
        uint32_t num_writers = bifrost_reader_list_writers(gw->reader, writers, 32);
        int count = 0;
        
        for (uint32_t w = 0; w < num_writers && count < gw->batch_size; w++) {
            /* Check if we already sent this version */
            uint32_t id = writers[w].id;
            uint64_t ver = writers[w].version;
            
            if (id < 1024 && ver <= gw->writer_versions[id]) {
                continue; /* Nothing new */
            }

            size_t size;
            void* data = bifrost_reader_get_by_id(gw->reader, id, &size);
            if (!data || size == 0) continue;
            
            if (size > SLOT_SIZE) size = SLOT_SIZE;
            
            uint8_t* frame = (uint8_t*)gw->tx_buffer + count * FRAME_SIZE;
            build_packet_headers(frame, gw, size);
            memcpy(frame + TOTAL_HEADER_SIZE, data, size);
            
            /* Update version */
            if (id < 1024) gw->writer_versions[id] = ver;
            
            count++;
        }
        
        if (count > 0) {
            struct mmsghdr msgs[64];
            struct iovec iovecs[64];
            memset(msgs, 0, sizeof(msgs));
            
            for (int i = 0; i < count; i++) {
                iovecs[i].iov_base = (uint8_t*)gw->tx_buffer + i * FRAME_SIZE;
                iovecs[i].iov_len = TOTAL_HEADER_SIZE + SLOT_SIZE;
                msgs[i].msg_hdr.msg_iov = &iovecs[i];
                msgs[i].msg_hdr.msg_iovlen = 1;
            }
            
            int sent = sendmmsg(gw->socket_fd, msgs, count, 0);
            if (sent > 0) {
                atomic_fetch_add(&gw->shm_to_net_count, sent);
                processed += sent;
            } else {
                atomic_fetch_add(&gw->errors, 1);
            }
        }
    }
    
    /* Network → SHM */
    if ((gw->mode == BIFROST_GATEWAY_NET_TO_SHM || gw->mode == BIFROST_GATEWAY_BRIDGE) && gw->writer) {
        struct mmsghdr msgs[64];
        struct iovec iovecs[64];
        memset(msgs, 0, sizeof(msgs));
        
        int batch = gw->batch_size > 64 ? 64 : gw->batch_size;
        for (int i = 0; i < batch; i++) {
            iovecs[i].iov_base = (uint8_t*)gw->rx_buffer + i * FRAME_SIZE;
            iovecs[i].iov_len = FRAME_SIZE;
            msgs[i].msg_hdr.msg_iov = &iovecs[i];
            msgs[i].msg_hdr.msg_iovlen = 1;
        }
        
        int received = recvmmsg(gw->socket_fd, msgs, batch, MSG_DONTWAIT, NULL);
        if (received > 0) {
            for (int i = 0; i < received; i++) {
                uint8_t* frame = (uint8_t*)iovecs[i].iov_base;
                size_t len = msgs[i].msg_len;
                
                if (len < TOTAL_HEADER_SIZE) continue;
                
                struct ethhdr* eth = (struct ethhdr*)frame;
                if (ntohs(eth->h_proto) != ETH_P_IP) continue;
                
                struct iphdr* ip = (struct iphdr*)(frame + ETH_HEADER_SIZE);
                if (ip->protocol != IPPROTO_UDP) continue;
                
                struct udphdr* udp = (struct udphdr*)(frame + ETH_HEADER_SIZE + IP_HEADER_SIZE);
                if (ntohs(udp->dest) != gw->port) continue;
                
                void* payload = frame + TOTAL_HEADER_SIZE;
                size_t payload_len = len - TOTAL_HEADER_SIZE;
                if (payload_len > SLOT_SIZE) payload_len = SLOT_SIZE;
                
                void* slot = bifrost_writer_alloc(gw->writer, payload_len);
                if (slot) {
                    memcpy(slot, payload, payload_len);
                    bifrost_writer_publish(gw->writer);
                    atomic_fetch_add(&gw->net_to_shm_count, 1);
                    processed++;
                }
            }
        }
    }
    
    return processed;
}

int bifrost_gateway_run(bifrost_gateway_t* gw) {
    if (!gw) return -1;
    
    while (gw->running) {
        bifrost_gateway_process(gw);
    }
    
    return 0;
}

void bifrost_gateway_stop(bifrost_gateway_t* gw) {
    if (gw) gw->running = 0;
}

void bifrost_gateway_get_stats(bifrost_gateway_t* gw, bifrost_gateway_stats_t* stats_out) {
    if (!gw || !stats_out) return;
    
    stats_out->shm_to_net_count = atomic_load(&gw->shm_to_net_count);
    stats_out->net_to_shm_count = atomic_load(&gw->net_to_shm_count);
    stats_out->errors = atomic_load(&gw->errors);
    stats_out->uptime_ns = get_time_ns() - gw->start_time;
}
