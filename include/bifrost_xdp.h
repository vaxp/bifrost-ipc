/**
 * @file bifrost_xdp.h
 * @brief Bifrost XDP - Ultra-Low-Latency Kernel Bypass Networking
 * 
 * AF_XDP (eXpress Data Path) provides TRUE kernel bypass:
 * - Packets go directly from NIC to userspace (and vice versa)
 * - Zero kernel network stack overhead
 * - Latency: ~1-2 µs instead of 50+ µs
 * - Throughput: 10+ million packets/second
 * 
 * Architecture:
 * ┌────────────────────────────────────────────────────────────────────────┐
 * │                    Traditional UDP vs AF_XDP                          │
 * ├────────────────────────────────────────────────────────────────────────┤
 * │                                                                        │
 * │  Traditional UDP:                                                      │
 * │  App → syscall → kernel → UDP → IP → driver → NIC                     │
 * │                     ↑                                                  │
 * │                   SLOW! (50+ µs)                                       │
 * │                                                                        │
 * │  AF_XDP:                                                               │
 * │  App → shared ring buffer → NIC                                        │
 * │                     ↑                                                  │
 * │                   FAST! (~1 µs)                                        │
 * │                                                                        │
 * └────────────────────────────────────────────────────────────────────────┘
 * 
 * Requirements:
 * - Linux 4.18+ (5.3+ recommended for best features)
 * - Root privileges or CAP_NET_ADMIN + CAP_SYS_ADMIN
 * - libbpf and libxdp libraries
 */

#ifndef BIFROST_XDP_H
#define BIFROST_XDP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * Constants
 * ═══════════════════════════════════════════════════════════════════════════ */

#define BIFROST_XDP_FRAME_SIZE       2048
#define BIFROST_XDP_NUM_FRAMES       4096
#define BIFROST_XDP_BATCH_SIZE       64
#define BIFROST_XDP_HEADROOM         256     /* Space for headers */

/* ═══════════════════════════════════════════════════════════════════════════
 * XDP Mode
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    BIFROST_XDP_MODE_SKB     = 0,    /* Generic/SKB mode (works everywhere, slower) */
    BIFROST_XDP_MODE_NATIVE  = 1,    /* Native/DRV mode (requires driver support) */
    BIFROST_XDP_MODE_OFFLOAD = 2     /* Hardware offload (requires smart NIC) */
} bifrost_xdp_mode_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Configuration
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    const char*     interface;      /* Network interface (e.g., "eth0") */
    uint32_t        queue_id;       /* NIC queue to attach to */
    bifrost_xdp_mode_t   mode;           /* XDP mode */
    uint32_t        frame_size;     /* Frame size (default: 2048) */
    uint32_t        num_frames;     /* Number of UMEM frames */
    uint32_t        batch_size;     /* Batch size for send/recv */
    bool            zero_copy;      /* Use zero-copy mode if available */
    int             cpu_affinity;   /* CPU to pin (-1 = no pinning) */
} bifrost_xdp_config_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Statistics
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t    tx_packets;
    uint64_t    tx_bytes;
    uint64_t    rx_packets;
    uint64_t    rx_bytes;
    uint64_t    tx_dropped;
    uint64_t    rx_dropped;
    uint64_t    tx_invalid;
    uint64_t    rx_invalid;
    double      avg_tx_batch;
    double      avg_rx_batch;
} bifrost_xdp_stats_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Opaque Handle
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct bifrost_xdp_socket bifrost_xdp_socket_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Initialization
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Initialize default configuration
 */
void bifrost_xdp_config_init(bifrost_xdp_config_t* config);

/**
 * Check if AF_XDP is available on this system
 */
bool bifrost_xdp_is_available(void);

/**
 * Get available XDP mode for interface
 */
bifrost_xdp_mode_t bifrost_xdp_get_best_mode(const char* interface);

/**
 * Get mode name
 */
const char* bifrost_xdp_mode_name(bifrost_xdp_mode_t mode);

/* ═══════════════════════════════════════════════════════════════════════════
 * Socket Management
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Create AF_XDP socket
 * 
 * @param config    Socket configuration
 * @return          Socket handle or NULL on error
 */
bifrost_xdp_socket_t* bifrost_xdp_socket_create(const bifrost_xdp_config_t* config);

/**
 * Get socket file descriptor (for polling)
 */
int bifrost_xdp_socket_fd(bifrost_xdp_socket_t* xsk);

/**
 * Destroy socket
 */
void bifrost_xdp_socket_destroy(bifrost_xdp_socket_t* xsk);

/* ═══════════════════════════════════════════════════════════════════════════
 * Zero-Copy Send (TX)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Get TX buffer for zero-copy send
 * 
 * Returns pointer to frame buffer where you can write packet data.
 * Call bifrost_xdp_tx_submit() after writing.
 * 
 * @param xsk       Socket handle
 * @param len       Desired packet length
 * @return          Pointer to buffer or NULL if no space
 */
void* bifrost_xdp_tx_alloc(bifrost_xdp_socket_t* xsk, size_t len);

/**
 * Submit TX packets
 * 
 * Submits all packets allocated with bifrost_xdp_tx_alloc().
 * Non-blocking, returns immediately.
 * 
 * @param xsk       Socket handle
 * @return          Number of packets submitted
 */
int bifrost_xdp_tx_submit(bifrost_xdp_socket_t* xsk);

/**
 * Complete TX (reclaim buffers)
 * 
 * Call periodically to reclaim transmitted buffers.
 * 
 * @param xsk       Socket handle
 * @return          Number of completions processed
 */
int bifrost_xdp_tx_complete(bifrost_xdp_socket_t* xsk);

/* ═══════════════════════════════════════════════════════════════════════════
 * Zero-Copy Receive (RX)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Receive callback for batch processing
 */
typedef void (*bifrost_xdp_rx_callback_t)(const void* data, size_t len, void* user_data);

/**
 * Receive packets (zero-copy batch)
 * 
 * Calls callback for each received packet.
 * Packets are only valid during callback execution.
 * 
 * @param xsk       Socket handle
 * @param callback  Function to call for each packet
 * @param user_data User data passed to callback
 * @return          Number of packets received
 */
int bifrost_xdp_rx_batch(bifrost_xdp_socket_t* xsk, bifrost_xdp_rx_callback_t callback, void* user_data);

/**
 * Poll for RX packets with timeout
 * 
 * @param xsk       Socket handle
 * @param timeout_ms Timeout in milliseconds (-1 = infinite)
 * @return          1 if packets available, 0 on timeout, -1 on error
 */
int bifrost_xdp_rx_poll(bifrost_xdp_socket_t* xsk, int timeout_ms);

/* ═══════════════════════════════════════════════════════════════════════════
 * Statistics
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Get socket statistics
 */
void bifrost_xdp_get_stats(bifrost_xdp_socket_t* xsk, bifrost_xdp_stats_t* stats);

/**
 * Reset statistics
 */
void bifrost_xdp_reset_stats(bifrost_xdp_socket_t* xsk);

/* ═══════════════════════════════════════════════════════════════════════════
 * Utility
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Get timestamp in nanoseconds
 */
uint64_t bifrost_xdp_get_time_ns(void);

#ifdef __cplusplus
}
#endif

#endif /* BIFROST_XDP_H */
