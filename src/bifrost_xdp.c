/**
 * @file bifrost_xdp.c
 * @brief Bifrost XDP Implementation
 * 
 * AF_XDP kernel bypass for ultra-low-latency networking.
 * 
 * This implementation provides a portable wrapper that:
 * 1. Uses AF_XDP when available (Linux 4.18+)
 * 2. Falls back to optimized UDP when AF_XDP not available
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <linux/if_link.h>
#include <net/if.h>
#include <stdatomic.h>

#include "bifrost_xdp.h"

/* Check for XDP support */
#if __has_include(<linux/if_xdp.h>)
#define BIFROST_HAS_XDP 1
#include <linux/if_xdp.h>
#else
#define BIFROST_HAS_XDP 0
#endif

#if BIFROST_HAS_XDP && __has_include(<xdp/xsk.h>)
#define BIFROST_HAS_LIBXDP 1
#include <xdp/xsk.h>
#else
#define BIFROST_HAS_LIBXDP 0
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * Constants
 * ═══════════════════════════════════════════════════════════════════════════ */

#define INVALID_UMEM_FRAME UINT64_MAX

/* Ring sizes (must be power of 2) */
#define XSK_RING_PROD_NUM_DESCS 2048
#define XSK_RING_CONS_NUM_DESCS 2048

/* ═══════════════════════════════════════════════════════════════════════════
 * Socket Structure
 * ═══════════════════════════════════════════════════════════════════════════ */

struct bifrost_xdp_socket {
    bifrost_xdp_config_t config;
    
#if BIFROST_HAS_LIBXDP
    /* XDP socket */
    struct xsk_socket*      xsk;
    struct xsk_umem*        umem;
    
    /* Rings */
    struct xsk_ring_prod    fq;     /* Fill queue (RX buffers to kernel) */
    struct xsk_ring_cons    cq;     /* Completion queue (TX done) */
    struct xsk_ring_prod    tx;     /* TX ring */
    struct xsk_ring_cons    rx;     /* RX ring */
    
    /* UMEM */
    void*                   umem_area;
    size_t                  umem_size;
    
    /* Frame management */
    uint64_t*               umem_frames;
    uint32_t                num_frames;
    uint32_t                free_frame_idx;
#endif
    
    /* Fallback socket (when XDP not available) */
    int                     fallback_fd;
    bool                    using_fallback;
    
    /* TX batch tracking */
    uint32_t                tx_batch_count;
    
    /* Stats */
    _Atomic uint64_t        tx_packets;
    _Atomic uint64_t        tx_bytes;
    _Atomic uint64_t        rx_packets;
    _Atomic uint64_t        rx_bytes;
    _Atomic uint64_t        tx_batches;
    _Atomic uint64_t        rx_batches;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Utilities
 * ═══════════════════════════════════════════════════════════════════════════ */

uint64_t bifrost_xdp_get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static inline void pin_to_cpu(int cpu) {
    if (cpu < 0) return;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu % sysconf(_SC_NPROCESSORS_ONLN), &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
}

const char* bifrost_xdp_mode_name(bifrost_xdp_mode_t mode) {
    switch (mode) {
        case BIFROST_XDP_MODE_SKB:     return "SKB (generic)";
        case BIFROST_XDP_MODE_NATIVE:  return "Native (driver)";
        case BIFROST_XDP_MODE_OFFLOAD: return "Offload (hardware)";
        default:                  return "Unknown";
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Configuration
 * ═══════════════════════════════════════════════════════════════════════════ */

void bifrost_xdp_config_init(bifrost_xdp_config_t* config) {
    if (!config) return;
    
    memset(config, 0, sizeof(*config));
    config->interface = "eth0";
    config->queue_id = 0;
    config->mode = BIFROST_XDP_MODE_SKB;
    config->frame_size = BIFROST_XDP_FRAME_SIZE;
    config->num_frames = BIFROST_XDP_NUM_FRAMES;
    config->batch_size = BIFROST_XDP_BATCH_SIZE;
    config->zero_copy = true;
    config->cpu_affinity = -1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Availability Check
 * ═══════════════════════════════════════════════════════════════════════════ */

bool bifrost_xdp_is_available(void) {
#if BIFROST_HAS_XDP
    /* Try to create an AF_XDP socket */
    int fd = socket(AF_XDP, SOCK_RAW, 0);
    if (fd >= 0) {
        close(fd);
        return true;
    }
#endif
    return false;
}

bifrost_xdp_mode_t bifrost_xdp_get_best_mode(const char* interface) {
    (void)interface;
    
#if BIFROST_HAS_LIBXDP
    /* Check for native mode support */
    /* For now, default to SKB which works everywhere */
    return BIFROST_XDP_MODE_SKB;
#else
    return BIFROST_XDP_MODE_SKB;
#endif
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Socket Creation
 * ═══════════════════════════════════════════════════════════════════════════ */

#if BIFROST_HAS_LIBXDP
static int init_umem(bifrost_xdp_socket_t* xsk) {
    size_t frame_size = xsk->config.frame_size;
    uint32_t num_frames = xsk->config.num_frames;
    
    xsk->umem_size = frame_size * num_frames;
    
    /* Allocate UMEM area (must be page-aligned and huge-page friendly) */
    xsk->umem_area = mmap(NULL, xsk->umem_size,
                           PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                           -1, 0);
    
    if (xsk->umem_area == MAP_FAILED) {
        /* Fallback to regular pages */
        xsk->umem_area = mmap(NULL, xsk->umem_size,
                               PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS,
                               -1, 0);
        if (xsk->umem_area == MAP_FAILED) {
            return -1;
        }
    }
    
    /* Lock memory to prevent swap */
    mlock(xsk->umem_area, xsk->umem_size);
    
    /* Configure UMEM */
    struct xsk_umem_config cfg = {
        .fill_size = XSK_RING_PROD_NUM_DESCS,
        .comp_size = XSK_RING_CONS_NUM_DESCS,
        .frame_size = frame_size,
        .frame_headroom = BIFROST_XDP_HEADROOM,
        .flags = 0
    };
    
    int ret = xsk_umem__create(&xsk->umem, xsk->umem_area, xsk->umem_size,
                                &xsk->fq, &xsk->cq, &cfg);
    if (ret) {
        munmap(xsk->umem_area, xsk->umem_size);
        return ret;
    }
    
    /* Initialize frame tracking */
    xsk->umem_frames = calloc(num_frames, sizeof(uint64_t));
    if (!xsk->umem_frames) {
        xsk_umem__delete(xsk->umem);
        munmap(xsk->umem_area, xsk->umem_size);
        return -ENOMEM;
    }
    
    xsk->num_frames = num_frames;
    xsk->free_frame_idx = 0;
    
    for (uint32_t i = 0; i < num_frames; i++) {
        xsk->umem_frames[i] = i * frame_size;
    }
    
    return 0;
}

static int init_xsk_socket(bifrost_xdp_socket_t* xsk) {
    struct xsk_socket_config cfg = {
        .rx_size = XSK_RING_CONS_NUM_DESCS,
        .tx_size = XSK_RING_PROD_NUM_DESCS,
        .libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD,
        .xdp_flags = 0,
        .bind_flags = 0
    };
    
    /* Set mode flags */
    switch (xsk->config.mode) {
        case BIFROST_XDP_MODE_NATIVE:
            cfg.xdp_flags = XDP_FLAGS_DRV_MODE;
            break;
        case BIFROST_XDP_MODE_OFFLOAD:
            cfg.xdp_flags = XDP_FLAGS_HW_MODE;
            break;
        default:
            cfg.xdp_flags = XDP_FLAGS_SKB_MODE;
            break;
    }
    
    /* Zero-copy mode */
    if (xsk->config.zero_copy) {
        cfg.bind_flags |= XDP_ZEROCOPY;
    }
    
    int ifindex = if_nametoindex(xsk->config.interface);
    if (!ifindex) {
        fprintf(stderr, "BIFROST_XDP: Interface '%s' not found\n", xsk->config.interface);
        return -errno;
    }
    
    int ret = xsk_socket__create(&xsk->xsk, xsk->config.interface,
                                  xsk->config.queue_id, xsk->umem,
                                  &xsk->rx, &xsk->tx, &cfg);
    
    if (ret) {
        fprintf(stderr, "BIFROST_XDP: xsk_socket__create failed (err=%d): %s\n", ret, strerror(-ret));
        /* If zero-copy failed, try copy mode */
        if (xsk->config.zero_copy) {
            fprintf(stderr, "BIFROST_XDP: Trying copy mode...\n");
            cfg.bind_flags &= ~XDP_ZEROCOPY;
            cfg.bind_flags |= XDP_COPY;
            ret = xsk_socket__create(&xsk->xsk, xsk->config.interface,
                                      xsk->config.queue_id, xsk->umem,
                                      &xsk->rx, &xsk->tx, &cfg);
            if (ret) {
                fprintf(stderr, "BIFROST_XDP: Copy mode also failed (err=%d): %s\n", ret, strerror(-ret));
            }
        }
    }
    
    return ret;
}

static void populate_fill_ring(bifrost_xdp_socket_t* xsk) {
    uint32_t idx;
    uint32_t num = xsk->num_frames / 2;  /* Half for RX */
    
    if (xsk_ring_prod__reserve(&xsk->fq, num, &idx) != num) {
        return;
    }
    
    for (uint32_t i = 0; i < num; i++) {
        *xsk_ring_prod__fill_addr(&xsk->fq, idx++) = xsk->umem_frames[xsk->free_frame_idx++];
    }
    
    xsk_ring_prod__submit(&xsk->fq, num);
}
#endif /* BIFROST_HAS_LIBXDP */

bifrost_xdp_socket_t* bifrost_xdp_socket_create(const bifrost_xdp_config_t* config) {
    if (!config) return NULL;
    
    bifrost_xdp_socket_t* xsk = calloc(1, sizeof(bifrost_xdp_socket_t));
    if (!xsk) return NULL;
    
    memcpy(&xsk->config, config, sizeof(bifrost_xdp_config_t));
    xsk->fallback_fd = -1;
    
    pin_to_cpu(config->cpu_affinity);
    
#if BIFROST_HAS_LIBXDP
    if (bifrost_xdp_is_available()) {
        /* Initialize UMEM */
        if (init_umem(xsk) < 0) {
            fprintf(stderr, "BIFROST_XDP: UMEM init failed, falling back to UDP\n");
            goto fallback;
        }
        
        /* Initialize socket */
        if (init_xsk_socket(xsk) < 0) {
            fprintf(stderr, "BIFROST_XDP: Socket init failed, falling back to UDP\n");
            xsk_umem__delete(xsk->umem);
            munmap(xsk->umem_area, xsk->umem_size);
            free(xsk->umem_frames);
            goto fallback;
        }
        
        /* Populate fill ring for RX */
        populate_fill_ring(xsk);
        
        xsk->using_fallback = false;
        return xsk;
    }
    
fallback:
#endif
    
    /* Fallback: use optimized UDP */
    xsk->using_fallback = true;
    xsk->fallback_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (xsk->fallback_fd < 0) {
        free(xsk);
        return NULL;
    }
    
    /* Optimize fallback socket */
    int sndbuf = 16 * 1024 * 1024;
    int rcvbuf = 64 * 1024 * 1024;
    setsockopt(xsk->fallback_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    setsockopt(xsk->fallback_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    
    fprintf(stderr, "BIFROST_XDP: Using UDP fallback (XDP not available)\n");
    
    return xsk;
}

int bifrost_xdp_socket_fd(bifrost_xdp_socket_t* xsk) {
    if (!xsk) return -1;
    
#if BIFROST_HAS_LIBXDP
    if (!xsk->using_fallback && xsk->xsk) {
        return xsk_socket__fd(xsk->xsk);
    }
#endif
    
    return xsk->fallback_fd;
}

void bifrost_xdp_socket_destroy(bifrost_xdp_socket_t* xsk) {
    if (!xsk) return;
    
#if BIFROST_HAS_LIBXDP
    if (!xsk->using_fallback) {
        if (xsk->xsk) xsk_socket__delete(xsk->xsk);
        if (xsk->umem) xsk_umem__delete(xsk->umem);
        if (xsk->umem_area) munmap(xsk->umem_area, xsk->umem_size);
        if (xsk->umem_frames) free(xsk->umem_frames);
    }
#endif
    
    if (xsk->fallback_fd >= 0) {
        close(xsk->fallback_fd);
    }
    
    free(xsk);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * TX Operations
 * ═══════════════════════════════════════════════════════════════════════════ */

#if BIFROST_HAS_LIBXDP
static inline uint64_t alloc_frame(bifrost_xdp_socket_t* xsk) {
    if (xsk->free_frame_idx >= xsk->num_frames) {
        return INVALID_UMEM_FRAME;
    }
    return xsk->umem_frames[xsk->free_frame_idx++];
}

static inline void free_frame(bifrost_xdp_socket_t* xsk, uint64_t addr) {
    if (xsk->free_frame_idx > 0) {
        xsk->umem_frames[--xsk->free_frame_idx] = addr;
    }
}
#endif

void* bifrost_xdp_tx_alloc(bifrost_xdp_socket_t* xsk, size_t len) {
    if (!xsk || len > xsk->config.frame_size - BIFROST_XDP_HEADROOM) {
        return NULL;
    }
    
#if BIFROST_HAS_LIBXDP
    if (!xsk->using_fallback) {
        uint64_t addr = alloc_frame(xsk);
        if (addr == INVALID_UMEM_FRAME) {
            return NULL;
        }
        
        uint32_t idx;
        if (xsk_ring_prod__reserve(&xsk->tx, 1, &idx) != 1) {
            free_frame(xsk, addr);
            return NULL;
        }
        
        struct xdp_desc* desc = xsk_ring_prod__tx_desc(&xsk->tx, idx);
        desc->addr = addr + BIFROST_XDP_HEADROOM;
        desc->len = len;
        
        xsk->tx_batch_count++;
        
        return (uint8_t*)xsk->umem_area + addr + BIFROST_XDP_HEADROOM;
    }
#endif
    
    /* Fallback: allocate from heap (not ideal) */
    return malloc(len);
}

int bifrost_xdp_tx_submit(bifrost_xdp_socket_t* xsk) {
    if (!xsk) return -1;
    
#if BIFROST_HAS_LIBXDP
    if (!xsk->using_fallback) {
        uint32_t count = xsk->tx_batch_count;
        
        xsk_ring_prod__submit(&xsk->tx, count);
        
        /* Kick the kernel to send */
        sendto(xsk_socket__fd(xsk->xsk), NULL, 0, MSG_DONTWAIT, NULL, 0);
        
        atomic_fetch_add(&xsk->tx_packets, count);
        atomic_fetch_add(&xsk->tx_batches, 1);
        
        xsk->tx_batch_count = 0;
        return count;
    }
#endif
    
    return 0;
}

int bifrost_xdp_tx_complete(bifrost_xdp_socket_t* xsk) {
    if (!xsk) return -1;
    
#if BIFROST_HAS_LIBXDP
    if (!xsk->using_fallback) {
        uint32_t idx;
        uint32_t completed = xsk_ring_cons__peek(&xsk->cq, xsk->config.batch_size, &idx);
        
        if (completed > 0) {
            for (uint32_t i = 0; i < completed; i++) {
                uint64_t addr = *xsk_ring_cons__comp_addr(&xsk->cq, idx++);
                free_frame(xsk, addr);
            }
            xsk_ring_cons__release(&xsk->cq, completed);
        }
        
        return completed;
    }
#endif
    
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * RX Operations
 * ═══════════════════════════════════════════════════════════════════════════ */

int bifrost_xdp_rx_poll(bifrost_xdp_socket_t* xsk, int timeout_ms) {
    if (!xsk) return -1;
    
    int fd = bifrost_xdp_socket_fd(xsk);
    if (fd < 0) return -1;
    
    struct pollfd pfd = {
        .fd = fd,
        .events = POLLIN
    };
    
    return poll(&pfd, 1, timeout_ms);
}

int bifrost_xdp_rx_batch(bifrost_xdp_socket_t* xsk, bifrost_xdp_rx_callback_t callback, void* user_data) {
    if (!xsk || !callback) return -1;
    (void)user_data;
    
#if BIFROST_HAS_LIBXDP
    if (!xsk->using_fallback) {
        uint32_t idx;
        uint32_t received = xsk_ring_cons__peek(&xsk->rx, xsk->config.batch_size, &idx);
        
        if (received == 0) return 0;
        
        for (uint32_t i = 0; i < received; i++) {
            const struct xdp_desc* desc = xsk_ring_cons__rx_desc(&xsk->rx, idx++);
            void* data = (uint8_t*)xsk->umem_area + desc->addr;
            
            callback(data, desc->len, user_data);
            
            atomic_fetch_add(&xsk->rx_packets, 1);
            atomic_fetch_add(&xsk->rx_bytes, desc->len);
        }
        
        xsk_ring_cons__release(&xsk->rx, received);
        atomic_fetch_add(&xsk->rx_batches, 1);
        
        /* Replenish fill ring */
        uint32_t fill_idx;
        if (xsk_ring_prod__reserve(&xsk->fq, received, &fill_idx) == received) {
            for (uint32_t i = 0; i < received; i++) {
                uint64_t addr = alloc_frame(xsk);
                if (addr != INVALID_UMEM_FRAME) {
                    *xsk_ring_prod__fill_addr(&xsk->fq, fill_idx++) = addr;
                }
            }
            xsk_ring_prod__submit(&xsk->fq, received);
        }
        
        return received;
    }
#endif
    
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Statistics
 * ═══════════════════════════════════════════════════════════════════════════ */

void bifrost_xdp_get_stats(bifrost_xdp_socket_t* xsk, bifrost_xdp_stats_t* stats) {
    if (!xsk || !stats) return;
    
    memset(stats, 0, sizeof(*stats));
    stats->tx_packets = atomic_load(&xsk->tx_packets);
    stats->tx_bytes = atomic_load(&xsk->tx_bytes);
    stats->rx_packets = atomic_load(&xsk->rx_packets);
    stats->rx_bytes = atomic_load(&xsk->rx_bytes);
    
    uint64_t tx_batches = atomic_load(&xsk->tx_batches);
    uint64_t rx_batches = atomic_load(&xsk->rx_batches);
    
    if (tx_batches > 0) {
        stats->avg_tx_batch = (double)stats->tx_packets / tx_batches;
    }
    if (rx_batches > 0) {
        stats->avg_rx_batch = (double)stats->rx_packets / rx_batches;
    }
}

void bifrost_xdp_reset_stats(bifrost_xdp_socket_t* xsk) {
    if (!xsk) return;
    
    atomic_store(&xsk->tx_packets, 0);
    atomic_store(&xsk->tx_bytes, 0);
    atomic_store(&xsk->rx_packets, 0);
    atomic_store(&xsk->rx_bytes, 0);
    atomic_store(&xsk->tx_batches, 0);
    atomic_store(&xsk->rx_batches, 0);
}
