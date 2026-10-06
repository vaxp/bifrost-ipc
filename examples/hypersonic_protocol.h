/**
 * Bifrost Hypersonic Missile Tracking Simulation
 * 
 * Protocol for simulating radar tracking of hypersonic missiles.
 * Each missile is a writer, the defense system is a reader.
 * 
 * Hypersonic missiles travel at Mach 5-25 (1.7-8.5 km/s)
 * At these speeds, microseconds matter for interception!
 */

#ifndef HYPERSONIC_PROTOCOL_H
#define HYPERSONIC_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <math.h>

// ═══════════════════════════════════════════════════════════════════════════
// Configuration
// ═══════════════════════════════════════════════════════════════════════════

#define HYPER_POOL_NAME     "hypersonic_radar"
#define HYPER_MAX_MISSILES  10
#define HYPER_MAGIC         0x48595045  // "HYPE"

// Mach speeds (m/s at sea level, ~340 m/s = Mach 1)
#define MACH_1              340.0
#define MACH_5              (MACH_1 * 5)    // 1,700 m/s
#define MACH_10             (MACH_1 * 10)   // 3,400 m/s
#define MACH_25             (MACH_1 * 25)   // 8,500 m/s

// Radar range (meters)
#define RADAR_RANGE_KM      500.0
#define RADAR_RANGE_M       (RADAR_RANGE_KM * 1000.0)

// Defense base position
#define BASE_X              0.0
#define BASE_Y              0.0
#define BASE_Z              0.0

// ═══════════════════════════════════════════════════════════════════════════
// 3D Vector
// ═══════════════════════════════════════════════════════════════════════════

typedef struct {
    double x;
    double y;
    double z;
} Vec3;

static inline double vec3_length(Vec3 v) {
    return sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
}

static inline Vec3 vec3_sub(Vec3 a, Vec3 b) {
    return (Vec3){a.x - b.x, a.y - b.y, a.z - b.z};
}

static inline Vec3 vec3_normalize(Vec3 v) {
    double len = vec3_length(v);
    if (len > 0.0001) {
        return (Vec3){v.x/len, v.y/len, v.z/len};
    }
    return (Vec3){0, 0, 0};
}

// ═══════════════════════════════════════════════════════════════════════════
// Missile State (written by each missile simulator)
// ═══════════════════════════════════════════════════════════════════════════

typedef enum {
    MISSILE_INACTIVE = 0,
    MISSILE_LAUNCHED,
    MISSILE_CRUISING,
    MISSILE_TERMINAL,       // Final attack phase
    MISSILE_INTERCEPTED,
    MISSILE_IMPACT
} MissilePhase;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t missile_id;
    
    // Position (meters from radar base)
    Vec3 position;
    
    // Velocity vector (m/s)
    Vec3 velocity;
    
    // Speed info
    double speed_mps;       // meters per second
    double mach_number;
    
    // Targeting
    Vec3 target;            // Where it's heading
    double distance_to_target;
    double eta_seconds;     // Estimated time to impact
    
    // State
    MissilePhase phase;
    uint64_t launch_time_ns;
    uint64_t update_counter;
    uint64_t timestamp_ns;
    
    // Threat assessment
    uint8_t threat_level;   // 1-10
    bool is_maneuvering;
    double maneuver_g;      // G-forces during maneuver
    
    uint8_t _padding[16];
} MissileState;

// ═══════════════════════════════════════════════════════════════════════════
// Radar/Defense System Commands
// ═══════════════════════════════════════════════════════════════════════════

typedef enum {
    CMD_MISSILE_UPDATE = 1,
    CMD_MISSILE_LAUNCHED,
    CMD_MISSILE_MANEUVER,
    CMD_MISSILE_TERMINAL,
    CMD_MISSILE_INTERCEPTED,
    CMD_MISSILE_IMPACT,
    CMD_INTERCEPT_ATTEMPT     // Radar sends this to attempt interception
} MissileCommand;

// ═══════════════════════════════════════════════════════════════════════════
// Interception Parameters
// ═══════════════════════════════════════════════════════════════════════════

// Intercept success probability factors
#define INTERCEPT_BASE_CHANCE       0.70    // 70% base success rate
#define INTERCEPT_MACH_PENALTY      0.02    // -2% per Mach over 5
#define INTERCEPT_MANEUVER_PENALTY  0.20    // -20% if maneuvering
#define INTERCEPT_DISTANCE_FACTOR   0.0005  // Penalty per km distance
#define INTERCEPT_MIN_DISTANCE_KM   5.0     // Minimum engagement distance (5km)
#define INTERCEPT_MAX_DISTANCE_KM   400.0   // Maximum engagement distance (400km)
#define INTERCEPT_COOLDOWN_MS       200     // Cooldown between attempts (faster)

// ═══════════════════════════════════════════════════════════════════════════
// Defense System Tracking (reader side calculations)
// ═══════════════════════════════════════════════════════════════════════════

typedef struct {
    uint32_t missile_id;
    MissileState last_state;
    
    // Tracking calculations
    double distance_to_base;
    double closing_speed;       // Speed towards base (negative = approaching)
    double time_to_impact;
    
    // Intercept solution
    Vec3 predicted_position;    // Where missile will be
    Vec3 intercept_point;       // Best intercept location
    double intercept_window;    // Seconds available for intercept
    
    // Statistics
    uint64_t updates_received;
    uint64_t last_update_ns;
    double avg_update_interval_us;
    double min_latency_us;
    double max_latency_us;
    double avg_latency_us;
} MissileTrack;

// ═══════════════════════════════════════════════════════════════════════════
// Utility Functions
// ═══════════════════════════════════════════════════════════════════════════

static inline const char* phase_to_string(MissilePhase phase) {
    switch (phase) {
        case MISSILE_INACTIVE:    return "INACTIVE";
        case MISSILE_LAUNCHED:    return "LAUNCHED";
        case MISSILE_CRUISING:    return "CRUISING";
        case MISSILE_TERMINAL:    return "TERMINAL";
        case MISSILE_INTERCEPTED: return "INTERCEPT";
        case MISSILE_IMPACT:      return "IMPACT";
        default:                  return "UNKNOWN";
    }
}

static inline const char* threat_color(uint8_t level) {
    if (level >= 8) return "\033[91m";      // Red
    if (level >= 5) return "\033[93m";      // Yellow
    if (level >= 3) return "\033[96m";      // Cyan
    return "\033[92m";                       // Green
}

#endif // HYPERSONIC_PROTOCOL_H
