/**
 * @file bifrost_gateway.h
 * @brief Bifrost Network Gateway API
 * 
 * Bridges Bifrost shared memory with network protocol.
 * Enables seamless data transfer between local SHM and remote systems.
 * 
 * Architecture:
 * ┌─────────────────────────────────────────────────────────────────────────┐
 * │    Local Process         Gateway              Remote System             │
 * │    ┌─────────────┐     ┌─────────────┐      ┌─────────────────┐        │
 * │    │   Writer    │────▶│   SHM ─▶ NET│─────▶│  XDP Receiver   │        │
 * │    └─────────────┘     │             │      └─────────────────┘        │
 * │                        │             │                                  │
 * │    ┌─────────────┐     │             │      ┌─────────────────┐        │
 * │    │   Reader    │◀────│   NET ─▶ SHM│◀─────│   XDP Sender    │        │
 * │    └─────────────┘     └─────────────┘      └─────────────────┘        │
 * └─────────────────────────────────────────────────────────────────────────┘
 */

#ifndef BIFROST_GATEWAY_H
#define BIFROST_GATEWAY_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * Types
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Gateway operation mode
 */
typedef enum {
    BIFROST_GATEWAY_SHM_TO_NET = 0,  /**< Read from SHM, send to network */
    BIFROST_GATEWAY_NET_TO_SHM = 1,  /**< Receive from network, write to SHM */
    BIFROST_GATEWAY_BRIDGE     = 2   /**< Bidirectional */
} bifrost_gateway_mode_t;

/**
 * @brief Gateway configuration
 */
typedef struct {
    const char*         interface;      /**< Network interface name */
    const char*         remote_mac;     /**< Remote MAC address (xx:xx:xx:xx:xx:xx) */
    const char*         remote_ip;      /**< Remote IP address */
    const char*         pool_name;      /**< Bifrost pool name */
    uint16_t            port;           /**< Network port (default: 19850) */
    bifrost_gateway_mode_t   mode;           /**< Operation mode */
    int                 batch_size;     /**< Packets per batch (default: 64) */
} bifrost_gateway_config_t;

/**
 * @brief Gateway statistics
 */
typedef struct {
    uint64_t            shm_to_net_count;   /**< Messages sent to network */
    uint64_t            net_to_shm_count;   /**< Messages received from network */
    uint64_t            errors;             /**< Error count */
    uint64_t            uptime_ns;          /**< Gateway uptime in nanoseconds */
} bifrost_gateway_stats_t;

/* Opaque gateway handle */
typedef struct bifrost_gateway bifrost_gateway_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Configuration
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Get default gateway configuration
 * @return Default configuration with sensible values
 */
bifrost_gateway_config_t bifrost_gateway_config_default(void);

/* ═══════════════════════════════════════════════════════════════════════════
 * Gateway Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Create and start a gateway
 * @param config Gateway configuration
 * @return Gateway handle, or NULL on failure
 * @note Requires root privileges for raw socket access
 */
bifrost_gateway_t* bifrost_gateway_create(const bifrost_gateway_config_t* config);

/**
 * @brief Stop and destroy a gateway
 * @param gateway Gateway handle
 */
void bifrost_gateway_destroy(bifrost_gateway_t* gateway);

/**
 * @brief Process one batch of messages
 * @param gateway Gateway handle
 * @return Number of messages processed
 * 
 * For SHM_TO_NET mode: reads from SHM and sends to network
 * For NET_TO_SHM mode: receives from network and writes to SHM
 * For BRIDGE mode: does both
 */
int bifrost_gateway_process(bifrost_gateway_t* gateway);

/**
 * @brief Run gateway main loop (blocking)
 * @param gateway Gateway handle
 * @return 0 on clean exit, -1 on error
 * 
 * Runs until bifrost_gateway_stop() is called from another thread.
 */
int bifrost_gateway_run(bifrost_gateway_t* gateway);

/**
 * @brief Stop gateway from running
 * @param gateway Gateway handle
 */
void bifrost_gateway_stop(bifrost_gateway_t* gateway);

/* ═══════════════════════════════════════════════════════════════════════════
 * Statistics
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Get gateway statistics
 * @param gateway Gateway handle
 * @param stats_out Output structure for statistics
 */
void bifrost_gateway_get_stats(bifrost_gateway_t* gateway, bifrost_gateway_stats_t* stats_out);

#ifdef __cplusplus
}
#endif

#endif /* BIFROST_GATEWAY_H */
