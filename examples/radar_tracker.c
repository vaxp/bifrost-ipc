/**
 * Bifrost Radar Tracker - Defense System with INTERCEPTOR MISSILES
 * 
 * Tracks 10 hypersonic missiles in real-time.
 * Launches interceptor missiles when targets are in range.
 * 
 * Features:
 * - Real-time tracking of 10 concurrent missile writers
 * - Automatic interceptor launch when in engagement envelope
 * - Realistic intercept probability based on speed/maneuvering
 * - Statistics: intercepted vs escaped vs impacted
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <float.h>
#include <bifrost.h>
#include "hypersonic_protocol.h"

// ═══════════════════════════════════════════════════════════════════════════
// Interceptor State
// ═══════════════════════════════════════════════════════════════════════════

typedef struct {
    bool intercepted;           // Successfully destroyed
    bool escaped;               // Got through defenses
    bool impacted;              // Hit the base
    int intercept_attempts;     // How many times we tried
    uint64_t last_attempt_ns;   // For cooldown
    double best_intercept_chance; // Highest chance we had
} InterceptorState;

// ═══════════════════════════════════════════════════════════════════════════
// Global State
// ═══════════════════════════════════════════════════════════════════════════

static bifrost_reader_t* g_reader = NULL;
static volatile int g_running = 1;
static MissileTrack g_tracks[HYPER_MAX_MISSILES] = {0};
static InterceptorState g_interceptors[HYPER_MAX_MISSILES] = {0};

// Defense statistics
static int g_missiles_intercepted = 0;
static int g_missiles_escaped = 0;
static int g_missiles_impacted = 0;
static int g_total_intercept_attempts = 0;
static int g_interceptors_launched = 0;

// Overall statistics
static uint64_t g_total_reads = 0;
static double g_min_latency = DBL_MAX;
static double g_max_latency = 0;
static double g_sum_latency = 0;
static uint64_t g_latency_count = 0;

// ═══════════════════════════════════════════════════════════════════════════
// Utilities
// ═══════════════════════════════════════════════════════════════════════════

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static double rand_01(void) {
    return (double)rand() / RAND_MAX;
}

// ═══════════════════════════════════════════════════════════════════════════
// Intercept Calculation
// ═══════════════════════════════════════════════════════════════════════════

static void calculate_intercept(MissileTrack* track) {
    MissileState* m = &track->last_state;
    
    // Distance to base
    Vec3 to_base = {-m->position.x, -m->position.y, -m->position.z};
    track->distance_to_base = vec3_length(to_base);
    
    // Closing speed
    Vec3 dir_to_base = vec3_normalize(to_base);
    track->closing_speed = m->velocity.x * dir_to_base.x + 
                           m->velocity.y * dir_to_base.y + 
                           m->velocity.z * dir_to_base.z;
    
    // Time to impact
    if (track->closing_speed > 0) {
        track->time_to_impact = track->distance_to_base / track->closing_speed;
    } else {
        track->time_to_impact = 999999;
    }
    
    // Intercept point
    double intercept_time = track->time_to_impact * 0.5;
    track->intercept_point.x = m->position.x + m->velocity.x * intercept_time;
    track->intercept_point.y = m->position.y + m->velocity.y * intercept_time;
    track->intercept_point.z = m->position.z + m->velocity.z * intercept_time;
    
    track->intercept_window = track->time_to_impact - intercept_time;
}

// ═══════════════════════════════════════════════════════════════════════════
// INTERCEPTOR SYSTEM
// ═══════════════════════════════════════════════════════════════════════════

static double calculate_intercept_probability(MissileState* m, double distance_km) {
    double chance = INTERCEPT_BASE_CHANCE;
    
    // Mach penalty (harder to hit faster missiles)
    if (m->mach_number > 5) {
        chance -= (m->mach_number - 5) * INTERCEPT_MACH_PENALTY;
    }
    
    // Maneuvering penalty
    if (m->is_maneuvering) {
        chance -= INTERCEPT_MANEUVER_PENALTY;
        // Higher G = harder to hit
        chance -= m->maneuver_g * 0.01;
    }
    
    // Distance penalty (further = less accurate)
    chance -= distance_km * INTERCEPT_DISTANCE_FACTOR;
    
    // Terminal phase bonus (predictable trajectory)
    if (m->phase == MISSILE_TERMINAL) {
        chance += 0.10;  // +10% in terminal phase
    }
    
    // Clamp
    if (chance < 0.05) chance = 0.05;  // Minimum 5% chance
    if (chance > 0.95) chance = 0.95;  // Maximum 95% chance
    
    return chance;
}

static void attempt_intercept(int id, MissileState* m) {
    InterceptorState* inter = &g_interceptors[id];
    MissileTrack* track = &g_tracks[id];
    
    // Skip if already resolved
    if (inter->intercepted || inter->escaped || inter->impacted) {
        return;
    }
    
    // Check if missile is still active
    if (m->phase == MISSILE_INTERCEPTED) {
        inter->intercepted = true;
        g_missiles_intercepted++;
        return;
    }
    
    if (m->phase == MISSILE_IMPACT) {
        inter->impacted = true;
        g_missiles_impacted++;
        return;
    }
    
    double distance_km = track->distance_to_base / 1000.0;
    
    // Check if in engagement envelope
    if (distance_km < INTERCEPT_MIN_DISTANCE_KM) {
        // Too close! Missile escaped
        if (!inter->escaped) {
            inter->escaped = true;
            g_missiles_escaped++;
        }
        return;
    }
    
    if (distance_km > INTERCEPT_MAX_DISTANCE_KM) {
        // Too far, wait
        return;
    }
    
    // Check cooldown (500ms between attempts)
    uint64_t now = get_time_ns();
    if (now - inter->last_attempt_ns < INTERCEPT_COOLDOWN_MS * 1000000ULL) {
        return;
    }
    
    // Calculate intercept probability
    double chance = calculate_intercept_probability(m, distance_km);
    if (chance > inter->best_intercept_chance) {
        inter->best_intercept_chance = chance;
    }
    
    // LAUNCH INTERCEPTOR!
    inter->intercept_attempts++;
    inter->last_attempt_ns = now;
    g_total_intercept_attempts++;
    g_interceptors_launched++;
    
    // Roll the dice
    double roll = rand_01();
    
    if (roll < chance) {
        // SUCCESS!
        inter->intercepted = true;
        g_missiles_intercepted++;
        // The missile simulator will see this via shared state
    }
    // If failed, we'll try again after cooldown
}

// ═══════════════════════════════════════════════════════════════════════════
// Track Processing
// ═══════════════════════════════════════════════════════════════════════════

static void process_missile(int id, const char* writer_name) {
    uint64_t t_start = get_time_ns();
    
    size_t size;
    MissileState* m = bifrost_reader_get(g_reader, writer_name, &size);
    
    uint64_t t_end = get_time_ns();
    double latency_us = (t_end - t_start) / 1000.0;
    
    if (!m || m->magic != HYPER_MAGIC) {
        return;
    }
    
    MissileTrack* track = &g_tracks[id];
    
    // Update latency stats
    if (latency_us < track->min_latency_us || track->min_latency_us == 0) {
        track->min_latency_us = latency_us;
    }
    if (latency_us > track->max_latency_us) {
        track->max_latency_us = latency_us;
    }
    track->avg_latency_us = (track->avg_latency_us * track->updates_received + latency_us) 
                            / (track->updates_received + 1);
    
    // Global stats
    if (latency_us < g_min_latency) g_min_latency = latency_us;
    if (latency_us > g_max_latency) g_max_latency = latency_us;
    g_sum_latency += latency_us;
    g_latency_count++;
    
    // Copy state
    track->last_state = *m;
    track->missile_id = id;
    track->updates_received++;
    track->last_update_ns = t_end;
    
    // Calculate intercept solution
    calculate_intercept(track);
    
    // ATTEMPT INTERCEPT!
    attempt_intercept(id, m);
    
    g_total_reads++;
}

// ═══════════════════════════════════════════════════════════════════════════
// Display
// ═══════════════════════════════════════════════════════════════════════════

static void display_status(void) {
    printf("\033[2J\033[H");
    
    printf("╔═══════════════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║  🎯 Bifrost DEFENSE SYSTEM          Reads: %-12lu  Interceptors: %-4d     ║\n", 
           (unsigned long)g_total_reads, g_interceptors_launched);
    printf("╠═══════════════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║  \033[92m✓ INTERCEPTED: %-3d\033[0m  │  \033[93m⚠ ESCAPED: %-3d\033[0m  │  \033[91m✗ IMPACTED: %-3d\033[0m  │  Attempts: %-4d   ║\n",
           g_missiles_intercepted, g_missiles_escaped, g_missiles_impacted, g_total_intercept_attempts);
    printf("╠═══════════════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║  ID │ Status    │ Mach  │ Dist km │ ETA sec │ Threat │ Attempts │ P(kill) │ Lat µs  ║\n");
    printf("╠═══════════════════════════════════════════════════════════════════════════════════════╣\n");
    
    int active_threats = 0;
    
    for (int i = 0; i < HYPER_MAX_MISSILES; i++) {
        MissileTrack* t = &g_tracks[i];
        MissileState* m = &t->last_state;
        InterceptorState* inter = &g_interceptors[i];
        
        if (m->magic != HYPER_MAGIC || t->updates_received == 0) {
            printf("║  %02d │ \033[90mWAITING\033[0m   │ ---   │ ---     │ ---     │ ---    │ ---      │ ---     │ ---     ║\n", i);
            continue;
        }
        
        const char* status;
        const char* color;
        
        if (inter->intercepted) {
            status = "DESTROYED";
            color = "\033[92m";  // Green
        } else if (inter->impacted) {
            status = "IMPACT!";
            color = "\033[91m";  // Red
        } else if (inter->escaped) {
            status = "ESCAPED";
            color = "\033[93m";  // Yellow
        } else {
            active_threats++;
            if (m->phase == MISSILE_TERMINAL) {
                status = "TERMINAL";
                color = "\033[91m";
            } else if (m->is_maneuvering) {
                status = "EVASIVE";
                color = "\033[95m";  // Magenta
            } else {
                status = "TRACKING";
                color = "\033[96m";  // Cyan
            }
        }
        
        double distance_km = t->distance_to_base / 1000.0;
        double p_kill = calculate_intercept_probability(m, distance_km);
        
        printf("║  %s%02d%s │ %s%-9s%s │ %5.1f │ %7.1f │ %7.1f │   %s%2d%s    │    %2d    │  %5.1f%% │ %6.3f  ║\n",
               color, i, "\033[0m",
               color, status, "\033[0m",
               m->mach_number,
               distance_km,
               m->eta_seconds,
               threat_color(m->threat_level),
               m->threat_level,
               "\033[0m",
               inter->intercept_attempts,
               p_kill * 100,
               t->avg_latency_us);
    }
    
    printf("╠═══════════════════════════════════════════════════════════════════════════════════════╣\n");
    
    if (active_threats > 0) {
        printf("║  \033[91m🚨 ACTIVE THREATS: %d\033[0m                                                               ║\n", active_threats);
    } else if (g_missiles_intercepted + g_missiles_escaped + g_missiles_impacted == HYPER_MAX_MISSILES) {
        printf("║  ✅ ALL THREATS NEUTRALIZED                                                           ║\n");
    } else {
        printf("║  ⏳ Acquiring targets...                                                              ║\n");
    }
    
    printf("╠═══════════════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║  📊 Latency: min=%.3f µs, avg=%.3f µs, max=%.3f µs                                   ║\n",
           g_min_latency < DBL_MAX ? g_min_latency : 0,
           g_latency_count > 0 ? g_sum_latency / g_latency_count : 0,
           g_max_latency);
    printf("║  ⚡ Throughput: %.2f M reads/sec                                                        ║\n",
           g_latency_count > 0 ? g_latency_count / g_sum_latency : 0);
    printf("╚═══════════════════════════════════════════════════════════════════════════════════════╝\n");
    printf("   Engagement envelope: %.0f - %.0f km | Ctrl+C to exit\n",
           INTERCEPT_MIN_DISTANCE_KM, INTERCEPT_MAX_DISTANCE_KM);
}

// ═══════════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════════

int main(void) {
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║  🎯 Bifrost DEFENSE SYSTEM - Interceptor Enabled           ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n\n");
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    srand(time(NULL));
    
    // Wait for pool
    printf("🔍 Looking for missile simulation...\n");
    int attempts = 0;
    while (!bifrost_pool_exists(HYPER_POOL_NAME) && attempts < 30) {
        sleep(1);
        attempts++;
        printf("\r⏳ Waiting for missile_simulator... (%d/30)", attempts);
        fflush(stdout);
    }
    
    if (!bifrost_pool_exists(HYPER_POOL_NAME)) {
        printf("\n❌ Simulation not found! Start missile_simulator first.\n");
        return 1;
    }
    printf("\n✅ Threat detected!\n\n");
    
    // Join as reader
    g_reader = bifrost_join_as_reader(HYPER_POOL_NAME, "defense_system");
    if (!g_reader) {
        printf("❌ Failed to join: %s\n", bifrost_error_string(bifrost_get_last_error()));
        return 1;
    }
    printf("✅ Defense system ONLINE!\n");
    printf("🚀 Interceptor missiles ARMED!\n");
    printf("📡 Tracking %d channels...\n\n", HYPER_MAX_MISSILES);
    
    sleep(1);
    
    // Pre-build writer names
    char writer_names[HYPER_MAX_MISSILES][64];
    for (int i = 0; i < HYPER_MAX_MISSILES; i++) {
        snprintf(writer_names[i], 64, "missile_%02d", i);
    }
    
    uint64_t last_display = 0;
    uint64_t display_interval = 100000000;  // 100ms
    
    // Main tracking loop
    while (g_running) {
        for (int i = 0; i < HYPER_MAX_MISSILES; i++) {
            process_missile(i, writer_names[i]);
        }
        
        uint64_t now = get_time_ns();
        if (now - last_display > display_interval) {
            display_status();
            last_display = now;
        }
    }
    
    // Final statistics
    printf("\n\n");
    printf("╔═══════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║  📊 FINAL DEFENSE REPORT                                                      ║\n");
    printf("╠═══════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║  \033[92m✓ MISSILES INTERCEPTED: %-3d\033[0m                                                  ║\n", g_missiles_intercepted);
    printf("║  \033[93m⚠ MISSILES ESCAPED:     %-3d\033[0m                                                  ║\n", g_missiles_escaped);
    printf("║  \033[91m✗ BASE IMPACTS:         %-3d\033[0m                                                  ║\n", g_missiles_impacted);
    printf("╠═══════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║  Interceptors launched: %-4d                                                  ║\n", g_interceptors_launched);
    printf("║  Kill ratio: %.1f%%                                                            ║\n",
           g_interceptors_launched > 0 ? 100.0 * g_missiles_intercepted / g_interceptors_launched : 0);
    printf("╠═══════════════════════════════════════════════════════════════════════════════╣\n");
    printf("║  Total reads:  %-12lu                                                    ║\n", (unsigned long)g_total_reads);
    printf("║  Avg latency:  %.3f µs                                                       ║\n", 
           g_latency_count > 0 ? g_sum_latency / g_latency_count : 0);
    printf("║  Throughput:   %.2f M reads/sec                                              ║\n",
           g_latency_count > 0 ? g_latency_count / g_sum_latency : 0);
    printf("╚═══════════════════════════════════════════════════════════════════════════════╝\n");
    
    bifrost_reader_leave(g_reader);
    printf("\n👋 Defense system offline.\n");
    
    return 0;
}
