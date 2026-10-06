/**
 * Bifrost System Monitor - Shared Protocol Header
 * 
 * Defines the data structure shared between daemon (writer) and client (reader).
 * Based on the BifrostMemory template pattern.
 */

#ifndef SYSMON_PROTOCOL_H
#define SYSMON_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

// ═══════════════════════════════════════════════════════════════════════════
// 📡 Channel Configuration
// ═══════════════════════════════════════════════════════════════════════════

#define SYSMON_POOL_NAME    "sysmon_pool"
#define SYSMON_WRITER_NAME  "sysmon_daemon"
#define SYSMON_MAGIC        0x53595354  // "SYST"
#define SYSMON_MAX_CORES    16

// ═══════════════════════════════════════════════════════════════════════════
// 📨 Commands
// ═══════════════════════════════════════════════════════════════════════════

typedef enum {
    CMD_NONE = 0,
    CMD_STATS_UPDATE = 1,
    CMD_REFRESH = 2,
    CMD_SHUTDOWN = 99
} SysmonCmdType;

// ═══════════════════════════════════════════════════════════════════════════
// 📊 System Stats (Daemon writes, Clients read)
// ═══════════════════════════════════════════════════════════════════════════

typedef struct __attribute__((packed)) {
    // Header
    uint32_t magic;
    uint32_t version;
    
    // CPU Stats
    float cpu_usage_percent;
    float cpu_cores[SYSMON_MAX_CORES];
    uint32_t core_count;
    
    // Memory Stats
    uint32_t memory_used_mb;
    uint32_t memory_total_mb;
    
    // Uptime
    uint64_t uptime_seconds;
    
    // Metadata
    uint64_t update_counter;
    uint64_t timestamp_ns;
} SysmonState;

#endif // SYSMON_PROTOCOL_H
