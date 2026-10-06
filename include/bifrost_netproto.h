/**
 * @file bifrost_netproto.h
 * @brief Bifrost Network Protocol - Zero-Copy Network Gateway
 * 
 * High-Performance Network Protocol that bridges Bifrost shared memory
 * with network I/O using kernel bypass techniques.
 * 
 * Architecture:
 * ┌────────────────────────────────────────────────────────────────────────┐
 * │                     Bifrost Network Protocol                        │
 * ├────────────────────────────────────────────────────────────────────────┤
 * │                                                                        │
 * │  Local Process    Bifrost     Network Gateway      Remote Client    │
 * │  ┌──────────┐    ┌──────────┐    ┌──────────────┐    ┌──────────┐      │
 * │  │ Producer │───▶│ Shared   │───▶│ io_uring /   │───▶│ Consumer │      │
 * │  │          │    │ Memory   │    │ sendmmsg     │    │          │      │
 * │  └──────────┘    └──────────┘    └──────────────┘    └──────────┘      │
 * │       │               │                │                   │           │
 * │       └───────────────┴────────────────┴───────────────────┘           │
 * │                         ZERO-COPY PATH                                 │
 * └────────────────────────────────────────────────────────────────────────┘
 * 
 * Features:
 * - io_uring for async network I/O (kernel 5.1+)
 * - sendmmsg/recvmmsg batching (fallback for older kernels)
 * - UDP multicast support for market data distribution
 * - Sequence numbers for gap detection
 * - Direct NIC access when available (AF_XDP)
*/

#ifndef BIFROST_NETPROTO_H
#define BIFROST_NETPROTO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * Protocol Constants
 * ═══════════════════════════════════════════════════════════════════════════ */

#define BIFROST_NET_VERSION          1
#define BIFROST_NET_MAGIC            0x56484E50  /* "VHNP" */
#define BIFROST_NET_PORT_DEFAULT     19850
#define BIFROST_NET_MTU              1500
#define BIFROST_NET_MAX_PAYLOAD      (BIFROST_NET_MTU - 28)  /* MTU - IP/UDP headers */
#define BIFROST_NET_BATCH_SIZE       64      /* Messages per batch */
#define BIFROST_NET_RING_SIZE        4096    /* io_uring ring size */

/* ═══════════════════════════════════════════════════════════════════════════
 * Message Header (16 bytes - fits in L1 cache line)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct __attribute__((packed)) {
    uint32_t magic;             /* 4: BIFROST_NET_MAGIC */
    uint8_t  version;           /* 1: Protocol version */
    uint8_t  msg_type;          /* 1: Message type */
    uint16_t payload_len;       /* 2: Payload length */
    uint64_t sequence;          /* 8: Sequence number */
} bifrost_net_header_t;

_Static_assert(sizeof(bifrost_net_header_t) == 16, "Header must be 16 bytes");

/* ═══════════════════════════════════════════════════════════════════════════
 * Message Types
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    BIFROST_NET_MSG_HEARTBEAT    = 0x00,
    BIFROST_NET_MSG_DATA         = 0x01,
    BIFROST_NET_MSG_ACK          = 0x02,
    BIFROST_NET_MSG_NACK         = 0x03,     /* Request retransmit */
    BIFROST_NET_MSG_SUBSCRIBE    = 0x10,
    BIFROST_NET_MSG_UNSUBSCRIBE  = 0x11
} bifrost_net_msg_type_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Transport Mode
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    BIFROST_NET_MODE_UDP         = 0,    /* Standard UDP (fallback) */
    BIFROST_NET_MODE_UDP_BATCH   = 1,    /* sendmmsg/recvmmsg batching */
    BIFROST_NET_MODE_IO_URING    = 2,    /* io_uring async I/O */
    BIFROST_NET_MODE_XDP         = 3     /* AF_XDP kernel bypass (requires root) */
} bifrost_net_mode_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Configuration
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    char            bind_addr[64];      /* Local bind address */
    uint16_t        bind_port;          /* Local port */
    char            multicast_group[64]; /* Multicast group (optional) */
    bifrost_net_mode_t   mode;               /* Transport mode */
    uint32_t        batch_size;         /* Messages per batch */
    uint32_t        send_buffer_size;   /* Socket send buffer */
    uint32_t        recv_buffer_size;   /* Socket receive buffer */
    bool            enable_timestamps;  /* Hardware timestamps if available */
    int             cpu_affinity;       /* CPU to pin (-1 = no pinning) */
    
    /* Bifrost integration */
    const char*     bifrost_pool_name;       /* Bifrost pool to bridge */
    const char*     bifrost_channel_name;    /* Channel to read/write */
} bifrost_net_config_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Statistics
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t    msgs_sent;
    uint64_t    msgs_received;
    uint64_t    bytes_sent;
    uint64_t    bytes_received;
    uint64_t    batches_sent;
    uint64_t    batches_received;
    uint64_t    syscalls;           /* Number of syscalls made */
    uint64_t    gaps_detected;
    double      avg_batch_size;
    double      avg_latency_ns;
    double      min_latency_ns;
    double      max_latency_ns;
} bifrost_net_stats_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Opaque Handles
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct bifrost_net_gateway bifrost_net_gateway_t;
typedef struct bifrost_net_sender  bifrost_net_sender_t;
typedef struct bifrost_net_receiver bifrost_net_receiver_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Initialization
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Initialize default configuration
 */
void bifrost_net_config_init(bifrost_net_config_t* config);

/**
 * Detect best available transport mode
 */
bifrost_net_mode_t bifrost_net_detect_best_mode(void);

/**
 * Get mode name string
 */
const char* bifrost_net_mode_name(bifrost_net_mode_t mode);

/* ═══════════════════════════════════════════════════════════════════════════
 * Gateway (Bifrost <-> Network Bridge)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Create network gateway that bridges Bifrost to network
 * 
 * Reads from Bifrost shared memory and sends to network,
 * or receives from network and writes to Bifrost.
 */
bifrost_net_gateway_t* bifrost_net_gateway_create(const bifrost_net_config_t* config);

/**
 * Start gateway (spawns background thread)
 */
int bifrost_net_gateway_start(bifrost_net_gateway_t* gw);

/**
 * Stop gateway
 */
void bifrost_net_gateway_stop(bifrost_net_gateway_t* gw);

/**
 * Get gateway statistics
 */
void bifrost_net_gateway_get_stats(bifrost_net_gateway_t* gw, bifrost_net_stats_t* stats);

/**
 * Destroy gateway
 */
void bifrost_net_gateway_destroy(bifrost_net_gateway_t* gw);

/* ═══════════════════════════════════════════════════════════════════════════
 * Sender (Direct Network Send - bypasses Bifrost)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Create direct network sender
 */
bifrost_net_sender_t* bifrost_net_sender_create(const bifrost_net_config_t* config);

/**
 * Add destination address
 */
int bifrost_net_sender_add_dest(bifrost_net_sender_t* sender, const char* addr, uint16_t port);

/**
 * Send data (batched, zero-copy when possible)
 * @return sequence number
 */
uint64_t bifrost_net_sender_send(bifrost_net_sender_t* sender, const void* data, size_t len);

/**
 * Flush pending messages (force send batch)
 */
int bifrost_net_sender_flush(bifrost_net_sender_t* sender);

/**
 * Get sender statistics
 */
void bifrost_net_sender_get_stats(bifrost_net_sender_t* sender, bifrost_net_stats_t* stats);

/**
 * Destroy sender
 */
void bifrost_net_sender_destroy(bifrost_net_sender_t* sender);

/* ═══════════════════════════════════════════════════════════════════════════
 * Receiver (Direct Network Receive)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Create network receiver
 */
bifrost_net_receiver_t* bifrost_net_receiver_create(const bifrost_net_config_t* config);

/**
 * Receive data (non-blocking, batched)
 * @return number of messages received, data written to callback
 */
typedef void (*bifrost_net_recv_callback_t)(const void* data, size_t len, 
                                        uint64_t sequence, void* user_data);

int bifrost_net_receiver_poll(bifrost_net_receiver_t* recv, 
                          bifrost_net_recv_callback_t callback,
                          void* user_data);

/**
 * Get receiver statistics
 */
void bifrost_net_receiver_get_stats(bifrost_net_receiver_t* recv, bifrost_net_stats_t* stats);

/**
 * Destroy receiver
 */
void bifrost_net_receiver_destroy(bifrost_net_receiver_t* recv);

/* ═══════════════════════════════════════════════════════════════════════════
 * Utilities
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Get timestamp in nanoseconds
 */
uint64_t bifrost_net_get_time_ns(void);

/**
 * Validate message header
 */
bool bifrost_net_validate_header(const bifrost_net_header_t* header);

#ifdef __cplusplus
}
#endif

#endif /* BIFROST_NETPROTO_H */
