/**
 * @file bifrost_netproto.c
 * @brief Bifrost Network Protocol Implementation
 * 
 * High-performance network I/O with multiple transport backends:
 * 1. io_uring (Linux 5.1+) - Async I/O with minimal syscalls
 * 2. sendmmsg/recvmmsg - Batched syscalls (fallback)
 * 3. Standard UDP (universal fallback)
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <stdatomic.h>

#include "bifrost_netproto.h"
#include "bifrost.h"

/* Check for io_uring support */
#if __has_include(<liburing.h>)
#define BIFROST_HAS_IO_URING 1
#include <liburing.h>
#else
#define BIFROST_HAS_IO_URING 0
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * Internal Structures
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Message buffer for batching */
typedef struct __attribute__((aligned(64))) {
    bifrost_net_header_t header;
    uint8_t         payload[BIFROST_NET_MAX_PAYLOAD];
} bifrost_net_msg_buffer_t;

/* Sender structure */
struct bifrost_net_sender {
    int                 socket_fd;
    bifrost_net_config_t     config;
    bifrost_net_mode_t       mode;
    
    /* Destination addresses */
    struct sockaddr_in  destinations[16];
    int                 num_destinations;
    
    /* Batching buffers */
    bifrost_net_msg_buffer_t* batch_buffer;
    struct mmsghdr*     mmsg_vec;
    struct iovec*       iov_vec;
    int                 batch_count;
    
    /* Sequence */
    _Atomic uint64_t    sequence;
    
    /* Stats */
    _Atomic uint64_t    msgs_sent;
    _Atomic uint64_t    bytes_sent;
    _Atomic uint64_t    batches_sent;
    _Atomic uint64_t    syscalls;
    
#if BIFROST_HAS_IO_URING
    struct io_uring     ring;
    bool                use_uring;
#endif
};

/* Receiver structure */
struct bifrost_net_receiver {
    int                 socket_fd;
    bifrost_net_config_t     config;
    bifrost_net_mode_t       mode;
    
    /* Receive buffers */
    bifrost_net_msg_buffer_t* recv_buffer;
    struct mmsghdr*     mmsg_vec;
    struct iovec*       iov_vec;
    
    /* Sequence tracking */
    uint64_t            last_sequence;
    _Atomic uint64_t    gaps_detected;
    
    /* Stats */
    _Atomic uint64_t    msgs_received;
    _Atomic uint64_t    bytes_received;
    _Atomic uint64_t    batches_received;
    _Atomic uint64_t    syscalls;
    
    /* Latency tracking */
    _Atomic double      total_latency;
    _Atomic double      min_latency;
    _Atomic double      max_latency;
    _Atomic uint64_t    latency_samples;
    
#if BIFROST_HAS_IO_URING
    struct io_uring     ring;
    bool                use_uring;
#endif
};

/* Gateway structure */
struct bifrost_net_gateway {
    bifrost_net_config_t     config;
    bifrost_net_sender_t*    sender;
    bifrost_net_receiver_t*  receiver;
    
    /* Bifrost integration */
    bifrost_reader_t*        bifrost_reader;
    bifrost_writer_t*        bifrost_writer;
    
    /* Thread control */
    pthread_t           thread;
    _Atomic bool        running;
    
    /* Stats */
    bifrost_net_stats_t      stats;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Utilities
 * ═══════════════════════════════════════════════════════════════════════════ */

uint64_t bifrost_net_get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

bool bifrost_net_validate_header(const bifrost_net_header_t* header) {
    if (!header) return false;
    if (header->magic != BIFROST_NET_MAGIC) return false;
    if (header->version != BIFROST_NET_VERSION) return false;
    return true;
}

const char* bifrost_net_mode_name(bifrost_net_mode_t mode) {
    switch (mode) {
        case BIFROST_NET_MODE_UDP:       return "UDP (standard)";
        case BIFROST_NET_MODE_UDP_BATCH: return "UDP (sendmmsg batched)";
        case BIFROST_NET_MODE_IO_URING:  return "io_uring (async)";
        case BIFROST_NET_MODE_XDP:       return "AF_XDP (kernel bypass)";
        default:                    return "Unknown";
    }
}

static inline void pin_to_cpu(int cpu) {
    if (cpu < 0) return;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu % sysconf(_SC_NPROCESSORS_ONLN), &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Mode Detection
 * ═══════════════════════════════════════════════════════════════════════════ */

bifrost_net_mode_t bifrost_net_detect_best_mode(void) {
#if BIFROST_HAS_IO_URING
    /* Check if io_uring is available */
    struct io_uring ring;
    if (io_uring_queue_init(8, &ring, 0) == 0) {
        io_uring_queue_exit(&ring);
        return BIFROST_NET_MODE_IO_URING;
    }
#endif
    
    /* Try sendmmsg */
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0) {
        close(fd);
        return BIFROST_NET_MODE_UDP_BATCH;
    }
    
    return BIFROST_NET_MODE_UDP;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Configuration
 * ═══════════════════════════════════════════════════════════════════════════ */

void bifrost_net_config_init(bifrost_net_config_t* config) {
    if (!config) return;
    
    memset(config, 0, sizeof(*config));
    strncpy(config->bind_addr, "0.0.0.0", sizeof(config->bind_addr) - 1);
    config->bind_port = BIFROST_NET_PORT_DEFAULT;
    config->mode = bifrost_net_detect_best_mode();
    config->batch_size = BIFROST_NET_BATCH_SIZE;
    config->send_buffer_size = 16 * 1024 * 1024;  /* 16 MB */
    config->recv_buffer_size = 64 * 1024 * 1024;  /* 64 MB */
    config->enable_timestamps = true;
    config->cpu_affinity = -1;
    config->bifrost_pool_name = NULL;
    config->bifrost_channel_name = NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Sender Implementation
 * ═══════════════════════════════════════════════════════════════════════════ */

static int sender_init_socket(bifrost_net_sender_t* sender) {
    sender->socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sender->socket_fd < 0) return -1;
    
    /* Set non-blocking */
    int flags = fcntl(sender->socket_fd, F_GETFL, 0);
    fcntl(sender->socket_fd, F_SETFL, flags | O_NONBLOCK);
    
    /* Set send buffer size */
    setsockopt(sender->socket_fd, SOL_SOCKET, SO_SNDBUF, 
               &sender->config.send_buffer_size, sizeof(int));
    
    /* Disable fragmentation */
    int val = IP_PMTUDISC_DO;
    setsockopt(sender->socket_fd, IPPROTO_IP, IP_MTU_DISCOVER, &val, sizeof(val));
    
    return 0;
}

static int sender_init_batch_buffers(bifrost_net_sender_t* sender) {
    size_t batch = sender->config.batch_size;
    
    /* Allocate aligned batch buffer */
    sender->batch_buffer = aligned_alloc(64, batch * sizeof(bifrost_net_msg_buffer_t));
    if (!sender->batch_buffer) return -1;
    
    sender->mmsg_vec = calloc(batch, sizeof(struct mmsghdr));
    sender->iov_vec = calloc(batch, sizeof(struct iovec));
    if (!sender->mmsg_vec || !sender->iov_vec) return -1;
    
    /* Pre-initialize iovec structures */
    for (size_t i = 0; i < batch; i++) {
        sender->iov_vec[i].iov_base = &sender->batch_buffer[i];
        sender->iov_vec[i].iov_len = sizeof(bifrost_net_msg_buffer_t);
        
        sender->mmsg_vec[i].msg_hdr.msg_iov = &sender->iov_vec[i];
        sender->mmsg_vec[i].msg_hdr.msg_iovlen = 1;
    }
    
    sender->batch_count = 0;
    return 0;
}

bifrost_net_sender_t* bifrost_net_sender_create(const bifrost_net_config_t* config) {
    if (!config) return NULL;
    
    bifrost_net_sender_t* sender = calloc(1, sizeof(bifrost_net_sender_t));
    if (!sender) return NULL;
    
    memcpy(&sender->config, config, sizeof(bifrost_net_config_t));
    sender->mode = config->mode;
    atomic_store(&sender->sequence, 0);
    
    /* Initialize socket */
    if (sender_init_socket(sender) < 0) {
        free(sender);
        return NULL;
    }
    
    /* Initialize batch buffers */
    if (sender_init_batch_buffers(sender) < 0) {
        close(sender->socket_fd);
        free(sender);
        return NULL;
    }
    
#if BIFROST_HAS_IO_URING
    if (sender->mode == BIFROST_NET_MODE_IO_URING) {
        if (io_uring_queue_init(BIFROST_NET_RING_SIZE, &sender->ring, 0) == 0) {
            sender->use_uring = true;
        } else {
            sender->mode = BIFROST_NET_MODE_UDP_BATCH;
        }
    }
#endif
    
    /* CPU affinity */
    pin_to_cpu(config->cpu_affinity);
    
    return sender;
}

int bifrost_net_sender_add_dest(bifrost_net_sender_t* sender, const char* addr, uint16_t port) {
    if (!sender || !addr || sender->num_destinations >= 16) return -1;
    
    struct sockaddr_in* dest = &sender->destinations[sender->num_destinations];
    memset(dest, 0, sizeof(*dest));
    dest->sin_family = AF_INET;
    dest->sin_port = htons(port);
    
    if (inet_pton(AF_INET, addr, &dest->sin_addr) <= 0) {
        return -1;
    }
    
    sender->num_destinations++;
    return 0;
}

static inline void prepare_message(bifrost_net_sender_t* sender, 
                                    bifrost_net_msg_buffer_t* buf,
                                    const void* data, size_t len) {
    buf->header.magic = BIFROST_NET_MAGIC;
    buf->header.version = BIFROST_NET_VERSION;
    buf->header.msg_type = BIFROST_NET_MSG_DATA;
    buf->header.payload_len = (uint16_t)len;
    buf->header.sequence = atomic_fetch_add(&sender->sequence, 1) + 1;
    
    memcpy(buf->payload, data, len);
}

static int sender_flush_batch_mmsg(bifrost_net_sender_t* sender) {
    if (sender->batch_count == 0) return 0;
    
    /* Send to all destinations */
    for (int d = 0; d < sender->num_destinations; d++) {
        for (int i = 0; i < sender->batch_count; i++) {
            sender->mmsg_vec[i].msg_hdr.msg_name = &sender->destinations[d];
            sender->mmsg_vec[i].msg_hdr.msg_namelen = sizeof(struct sockaddr_in);
            
            /* Update iov length to actual message size */
            size_t msg_len = sizeof(bifrost_net_header_t) + 
                             sender->batch_buffer[i].header.payload_len;
            sender->iov_vec[i].iov_len = msg_len;
        }
        
        /* Single syscall for entire batch! */
        int sent = sendmmsg(sender->socket_fd, sender->mmsg_vec, 
                            sender->batch_count, 0);
        
        if (sent > 0) {
            atomic_fetch_add(&sender->msgs_sent, sent);
            atomic_fetch_add(&sender->batches_sent, 1);
            atomic_fetch_add(&sender->syscalls, 1);
            
            /* Calculate bytes sent */
            size_t bytes = 0;
            for (int i = 0; i < sent; i++) {
                bytes += sender->iov_vec[i].iov_len;
            }
            atomic_fetch_add(&sender->bytes_sent, bytes);
        }
    }
    
    sender->batch_count = 0;
    return 0;
}

#if BIFROST_HAS_IO_URING
static int sender_flush_batch_uring(bifrost_net_sender_t* sender) {
    if (sender->batch_count == 0) return 0;
    
    for (int d = 0; d < sender->num_destinations; d++) {
        for (int i = 0; i < sender->batch_count; i++) {
            struct io_uring_sqe* sqe = io_uring_get_sqe(&sender->ring);
            if (!sqe) break;
            
            size_t msg_len = sizeof(bifrost_net_header_t) + 
                             sender->batch_buffer[i].header.payload_len;
            
            sender->iov_vec[i].iov_len = msg_len;
            sender->mmsg_vec[i].msg_hdr.msg_name = &sender->destinations[d];
            sender->mmsg_vec[i].msg_hdr.msg_namelen = sizeof(struct sockaddr_in);
            
            io_uring_prep_sendmsg(sqe, sender->socket_fd, 
                                   &sender->mmsg_vec[i].msg_hdr, 0);
            io_uring_sqe_set_data(sqe, (void*)(uintptr_t)i);
        }
    }
    
    /* Submit all at once */
    io_uring_submit(&sender->ring);
    atomic_fetch_add(&sender->syscalls, 1);
    
    /* Wait for completions */
    struct io_uring_cqe* cqe;
    int completed = 0;
    while (io_uring_peek_cqe(&sender->ring, &cqe) == 0) {
        if (cqe->res > 0) {
            atomic_fetch_add(&sender->msgs_sent, 1);
            atomic_fetch_add(&sender->bytes_sent, cqe->res);
            completed++;
        }
        io_uring_cqe_seen(&sender->ring, cqe);
    }
    
    if (completed > 0) {
        atomic_fetch_add(&sender->batches_sent, 1);
    }
    
    sender->batch_count = 0;
    return 0;
}
#endif

uint64_t bifrost_net_sender_send(bifrost_net_sender_t* sender, const void* data, size_t len) {
    if (!sender || !data || len > BIFROST_NET_MAX_PAYLOAD) return 0;
    if (sender->num_destinations == 0) return 0;
    
    /* Add to batch */
    bifrost_net_msg_buffer_t* buf = &sender->batch_buffer[sender->batch_count];
    prepare_message(sender, buf, data, len);
    
    uint64_t seq = buf->header.sequence;
    sender->batch_count++;
    
    /* Flush if batch is full */
    if ((size_t)sender->batch_count >= sender->config.batch_size) {
        bifrost_net_sender_flush(sender);
    }
    
    return seq;
}

int bifrost_net_sender_flush(bifrost_net_sender_t* sender) {
    if (!sender) return -1;
    
#if BIFROST_HAS_IO_URING
    if (sender->use_uring) {
        return sender_flush_batch_uring(sender);
    }
#endif
    
    return sender_flush_batch_mmsg(sender);
}

void bifrost_net_sender_get_stats(bifrost_net_sender_t* sender, bifrost_net_stats_t* stats) {
    if (!sender || !stats) return;
    
    memset(stats, 0, sizeof(*stats));
    stats->msgs_sent = atomic_load(&sender->msgs_sent);
    stats->bytes_sent = atomic_load(&sender->bytes_sent);
    stats->batches_sent = atomic_load(&sender->batches_sent);
    stats->syscalls = atomic_load(&sender->syscalls);
    
    if (stats->batches_sent > 0) {
        stats->avg_batch_size = (double)stats->msgs_sent / stats->batches_sent;
    }
}

void bifrost_net_sender_destroy(bifrost_net_sender_t* sender) {
    if (!sender) return;
    
    /* Flush remaining */
    bifrost_net_sender_flush(sender);
    
#if BIFROST_HAS_IO_URING
    if (sender->use_uring) {
        io_uring_queue_exit(&sender->ring);
    }
#endif
    
    close(sender->socket_fd);
    free(sender->batch_buffer);
    free(sender->mmsg_vec);
    free(sender->iov_vec);
    free(sender);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Receiver Implementation
 * ═══════════════════════════════════════════════════════════════════════════ */

static int receiver_init_socket(bifrost_net_receiver_t* recv) {
    recv->socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (recv->socket_fd < 0) return -1;
    
    /* Allow address reuse */
    int optval = 1;
    setsockopt(recv->socket_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    
    /* Set receive buffer size */
    setsockopt(recv->socket_fd, SOL_SOCKET, SO_RCVBUF,
               &recv->config.recv_buffer_size, sizeof(int));
    
    /* Bind */
    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(recv->config.bind_port);
    inet_pton(AF_INET, recv->config.bind_addr, &bind_addr.sin_addr);
    
    if (bind(recv->socket_fd, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        close(recv->socket_fd);
        return -1;
    }
    
    /* Join multicast if configured */
    if (recv->config.multicast_group[0]) {
        struct ip_mreq mreq;
        inet_pton(AF_INET, recv->config.multicast_group, &mreq.imr_multiaddr);
        mreq.imr_interface.s_addr = INADDR_ANY;
        setsockopt(recv->socket_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    }
    
    /* Keep socket blocking for recvmmsg with MSG_WAITFORONE */
    /* This allows batch receive of messages that arrive together */
    
    return 0;
}

static int receiver_init_batch_buffers(bifrost_net_receiver_t* recv) {
    size_t batch = recv->config.batch_size;
    
    recv->recv_buffer = aligned_alloc(64, batch * sizeof(bifrost_net_msg_buffer_t));
    if (!recv->recv_buffer) return -1;
    
    recv->mmsg_vec = calloc(batch, sizeof(struct mmsghdr));
    recv->iov_vec = calloc(batch, sizeof(struct iovec));
    if (!recv->mmsg_vec || !recv->iov_vec) return -1;
    
    for (size_t i = 0; i < batch; i++) {
        recv->iov_vec[i].iov_base = &recv->recv_buffer[i];
        recv->iov_vec[i].iov_len = sizeof(bifrost_net_msg_buffer_t);
        
        recv->mmsg_vec[i].msg_hdr.msg_iov = &recv->iov_vec[i];
        recv->mmsg_vec[i].msg_hdr.msg_iovlen = 1;
    }
    
    return 0;
}

bifrost_net_receiver_t* bifrost_net_receiver_create(const bifrost_net_config_t* config) {
    if (!config) return NULL;
    
    bifrost_net_receiver_t* recv = calloc(1, sizeof(bifrost_net_receiver_t));
    if (!recv) return NULL;
    
    memcpy(&recv->config, config, sizeof(bifrost_net_config_t));
    recv->mode = config->mode;
    atomic_store(&recv->min_latency, 1e18);
    atomic_store(&recv->max_latency, 0);
    
    if (receiver_init_socket(recv) < 0) {
        free(recv);
        return NULL;
    }
    
    if (receiver_init_batch_buffers(recv) < 0) {
        close(recv->socket_fd);
        free(recv);
        return NULL;
    }
    
#if BIFROST_HAS_IO_URING
    if (recv->mode == BIFROST_NET_MODE_IO_URING) {
        if (io_uring_queue_init(BIFROST_NET_RING_SIZE, &recv->ring, 0) == 0) {
            recv->use_uring = true;
        } else {
            recv->mode = BIFROST_NET_MODE_UDP_BATCH;
        }
    }
#endif
    
    pin_to_cpu(config->cpu_affinity);
    
    return recv;
}

int bifrost_net_receiver_poll(bifrost_net_receiver_t* recv, 
                          bifrost_net_recv_callback_t callback,
                          void* user_data) {
    if (!recv) return -1;
    
    /* Use MSG_WAITFORONE: return as soon as first message arrives,
       but also pick up any additional messages already in the buffer */
    int received = recvmmsg(recv->socket_fd, recv->mmsg_vec, 
                             recv->config.batch_size, MSG_WAITFORONE, NULL);
    
    if (received <= 0) return 0;
    
    atomic_fetch_add(&recv->syscalls, 1);
    atomic_fetch_add(&recv->batches_received, 1);
    
    uint64_t recv_time = bifrost_net_get_time_ns();
    (void)recv_time;
    int processed = 0;
    
    for (int i = 0; i < received; i++) {
        bifrost_net_msg_buffer_t* buf = &recv->recv_buffer[i];
        
        if (!bifrost_net_validate_header(&buf->header)) continue;
        
        /* Gap detection */
        if (recv->last_sequence > 0 && 
            buf->header.sequence != recv->last_sequence + 1) {
            if (buf->header.sequence > recv->last_sequence + 1) {
                uint64_t gap = buf->header.sequence - recv->last_sequence - 1;
                atomic_fetch_add(&recv->gaps_detected, gap);
            }
        }
        recv->last_sequence = buf->header.sequence;
        
        /* Stats */
        atomic_fetch_add(&recv->msgs_received, 1);
        atomic_fetch_add(&recv->bytes_received, recv->mmsg_vec[i].msg_len);
        
        /* Callback */
        if (callback) {
            callback(buf->payload, buf->header.payload_len,
                    buf->header.sequence, user_data);
        }
        
        processed++;
    }
    
    return processed;
}

void bifrost_net_receiver_get_stats(bifrost_net_receiver_t* recv, bifrost_net_stats_t* stats) {
    if (!recv || !stats) return;
    
    memset(stats, 0, sizeof(*stats));
    stats->msgs_received = atomic_load(&recv->msgs_received);
    stats->bytes_received = atomic_load(&recv->bytes_received);
    stats->batches_received = atomic_load(&recv->batches_received);
    stats->syscalls = atomic_load(&recv->syscalls);
    stats->gaps_detected = atomic_load(&recv->gaps_detected);
    
    if (stats->batches_received > 0) {
        stats->avg_batch_size = (double)stats->msgs_received / stats->batches_received;
    }
}

void bifrost_net_receiver_destroy(bifrost_net_receiver_t* recv) {
    if (!recv) return;
    
#if BIFROST_HAS_IO_URING
    if (recv->use_uring) {
        io_uring_queue_exit(&recv->ring);
    }
#endif
    
    close(recv->socket_fd);
    free(recv->recv_buffer);
    free(recv->mmsg_vec);
    free(recv->iov_vec);
    free(recv);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Gateway Implementation (Bifrost <-> Network Bridge)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void* gateway_thread(void* arg) {
    bifrost_net_gateway_t* gw = (bifrost_net_gateway_t*)arg;
    
    pin_to_cpu(gw->config.cpu_affinity);
    
    while (atomic_load(&gw->running)) {
        if (gw->bifrost_reader && gw->sender) {
            /* Read from Bifrost, send to network */
            size_t size;
            const void* data = bifrost_reader_get(gw->bifrost_reader, 
                                              gw->config.bifrost_channel_name, &size);
            if (data && size > 0) {
                bifrost_net_sender_send(gw->sender, data, size);
            }
        }
        
        /* Brief pause to prevent busy spinning */
        __asm__ __volatile__ ("pause" ::: "memory");
    }
    
    return NULL;
}

bifrost_net_gateway_t* bifrost_net_gateway_create(const bifrost_net_config_t* config) {
    if (!config) return NULL;
    
    bifrost_net_gateway_t* gw = calloc(1, sizeof(bifrost_net_gateway_t));
    if (!gw) return NULL;
    
    memcpy(&gw->config, config, sizeof(bifrost_net_config_t));
    atomic_store(&gw->running, false);
    
    /* Create sender */
    gw->sender = bifrost_net_sender_create(config);
    
    /* Connect to Bifrost if configured */
    if (config->bifrost_pool_name && config->bifrost_channel_name) {
        gw->bifrost_reader = bifrost_join_as_reader(config->bifrost_pool_name, "netproto");
    }
    
    return gw;
}

int bifrost_net_gateway_start(bifrost_net_gateway_t* gw) {
    if (!gw) return -1;
    
    atomic_store(&gw->running, true);
    return pthread_create(&gw->thread, NULL, gateway_thread, gw);
}

void bifrost_net_gateway_stop(bifrost_net_gateway_t* gw) {
    if (!gw) return;
    
    atomic_store(&gw->running, false);
    pthread_join(gw->thread, NULL);
}

void bifrost_net_gateway_get_stats(bifrost_net_gateway_t* gw, bifrost_net_stats_t* stats) {
    if (!gw || !stats) return;
    
    if (gw->sender) {
        bifrost_net_sender_get_stats(gw->sender, stats);
    }
}

void bifrost_net_gateway_destroy(bifrost_net_gateway_t* gw) {
    if (!gw) return;
    
    bifrost_net_gateway_stop(gw);
    
    if (gw->sender) bifrost_net_sender_destroy(gw->sender);
    if (gw->receiver) bifrost_net_receiver_destroy(gw->receiver);
    if (gw->bifrost_reader) bifrost_reader_leave(gw->bifrost_reader);
    if (gw->bifrost_writer) bifrost_writer_leave(gw->bifrost_writer);
    
    free(gw);
}
