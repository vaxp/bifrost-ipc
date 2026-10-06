/**
 * Bifrost Hypersonic Missile Simulator
 * 
 * Simulates 10 hypersonic missiles attacking a base.
 * Each missile is a separate WRITER publishing its state at maximum rate.
 * 
 * This creates EXTREME pressure on the shared memory system:
 * - 10 concurrent writers
 * - Updates as fast as possible (millions/sec per missile)
 * - Realistic physics simulation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <bifrost.h>
#include "hypersonic_protocol.h"

// ═══════════════════════════════════════════════════════════════════════════
// Configuration
// ═══════════════════════════════════════════════════════════════════════════

#define NUM_MISSILES        10
#define SIMULATION_TIME_S   30      // Run for 30 seconds

// ═══════════════════════════════════════════════════════════════════════════
// Global State
// ═══════════════════════════════════════════════════════════════════════════

static bifrost_pool_t* g_pool = NULL;
static volatile int g_running = 1;
static uint64_t g_total_updates[NUM_MISSILES] = {0};

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

static double rand_range(double min, double max) {
    return min + (max - min) * ((double)rand() / RAND_MAX);
}

// ═══════════════════════════════════════════════════════════════════════════
// Missile Thread
// ═══════════════════════════════════════════════════════════════════════════

typedef struct {
    int id;
    bifrost_writer_t* writer;
    MissileState* shared;
    MissileState state;
} MissileContext;

static void init_missile(MissileContext* ctx) {
    MissileState* m = &ctx->state;
    
    m->magic = HYPER_MAGIC;
    m->missile_id = ctx->id;
    
    // Random starting position (far from base, coming from different directions)
    double angle = (2.0 * M_PI * ctx->id) / NUM_MISSILES + rand_range(-0.3, 0.3);
    double distance = rand_range(300000, 450000);  // 300-450 km away
    double altitude = rand_range(20000, 80000);    // 20-80 km altitude
    
    m->position.x = cos(angle) * distance;
    m->position.y = sin(angle) * distance;
    m->position.z = altitude;
    
    // Target is the base (with some offset for realism)
    m->target.x = rand_range(-1000, 1000);
    m->target.y = rand_range(-1000, 1000);
    m->target.z = 0;
    
    // Calculate initial velocity towards target
    Vec3 to_target = vec3_sub(m->target, m->position);
    Vec3 dir = vec3_normalize(to_target);
    
    // Speed: Mach 8-15 for hypersonic
    #define MACH_15 (MACH_1 * 15)
    double speed = rand_range(MACH_10, MACH_15) + rand_range(-200, 200);
    
    m->speed_mps = speed;
    m->mach_number = speed / MACH_1;
    
    m->velocity.x = dir.x * speed;
    m->velocity.y = dir.y * speed;
    m->velocity.z = dir.z * speed;
    
    m->distance_to_target = vec3_length(to_target);
    m->eta_seconds = m->distance_to_target / speed;
    
    m->phase = MISSILE_LAUNCHED;
    m->launch_time_ns = get_time_ns();
    m->update_counter = 0;
    
    m->threat_level = (uint8_t)(5 + rand_range(0, 5));
    m->is_maneuvering = false;
    m->maneuver_g = 0;
}

static void update_missile_physics(MissileContext* ctx, double dt) {
    MissileState* m = &ctx->state;
    
    if (m->phase == MISSILE_INTERCEPTED || m->phase == MISSILE_IMPACT) {
        return;
    }
    
    // Update position based on velocity
    m->position.x += m->velocity.x * dt;
    m->position.y += m->velocity.y * dt;
    m->position.z += m->velocity.z * dt;
    
    // Recalculate target info
    Vec3 to_target = vec3_sub(m->target, m->position);
    m->distance_to_target = vec3_length(to_target);
    
    // Simple course correction (realistic missiles maneuver)
    if (m->distance_to_target > 1000) {
        Vec3 new_dir = vec3_normalize(to_target);
        
        // Add some random maneuvering
        if (rand() % 100 < 5) {  // 5% chance to maneuver
            m->is_maneuvering = true;
            m->maneuver_g = rand_range(5, 20);  // 5-20 G maneuver
            new_dir.x += rand_range(-0.02, 0.02);
            new_dir.y += rand_range(-0.02, 0.02);
            new_dir.z += rand_range(-0.01, 0.01);
            new_dir = vec3_normalize(new_dir);
        } else {
            m->is_maneuvering = false;
            m->maneuver_g = 0;
        }
        
        // Gradual velocity adjustment
        double lerp = 0.1;  // 10% correction per update
        m->velocity.x = m->velocity.x * (1-lerp) + new_dir.x * m->speed_mps * lerp;
        m->velocity.y = m->velocity.y * (1-lerp) + new_dir.y * m->speed_mps * lerp;
        m->velocity.z = m->velocity.z * (1-lerp) + new_dir.z * m->speed_mps * lerp;
    }
    
    // Update phase based on distance
    if (m->distance_to_target < 10000) {
        m->phase = MISSILE_TERMINAL;
        m->threat_level = 10;  // Maximum threat
    } else if (m->distance_to_target < 50000) {
        m->phase = MISSILE_CRUISING;
    }
    
    // Calculate ETA
    m->eta_seconds = m->distance_to_target / m->speed_mps;
    
    // Check for impact
    if (m->distance_to_target < 100) {
        m->phase = MISSILE_IMPACT;
    }
    
    // Update timestamp
    m->timestamp_ns = get_time_ns();
    m->update_counter++;
}

static void* missile_thread(void* arg) {
    MissileContext* ctx = (MissileContext*)arg;
    
    // Create writer name
    char writer_name[64];
    snprintf(writer_name, sizeof(writer_name), "missile_%02d", ctx->id);
    
    // Join as writer
    ctx->writer = bifrost_join_as_writer(HYPER_POOL_NAME, writer_name);
    if (!ctx->writer) {
        printf("❌ Missile %d failed to join!\n", ctx->id);
        return NULL;
    }
    
    // Allocate shared memory
    ctx->shared = bifrost_writer_alloc(ctx->writer, sizeof(MissileState));
    if (!ctx->shared) {
        printf("❌ Missile %d failed to allocate!\n", ctx->id);
        bifrost_writer_leave(ctx->writer);
        return NULL;
    }
    
    // Initialize missile
    init_missile(ctx);
    
    printf("🚀 Missile %02d launched! Mach %.1f, Distance: %.1f km, ETA: %.1f s\n",
           ctx->id, ctx->state.mach_number, 
           ctx->state.distance_to_target / 1000.0,
           ctx->state.eta_seconds);
    
    uint64_t last_time = get_time_ns();
    
    // Main simulation loop
    while (g_running && ctx->state.phase != MISSILE_IMPACT) {
        uint64_t now = get_time_ns();
        double dt = (now - last_time) / 1e9;  // Delta time in seconds
        last_time = now;
        
        // Limit dt to avoid jumps
        if (dt > 0.1) dt = 0.1;
        if (dt < 0.0000001) dt = 0.0000001;
        
        // Update physics
        update_missile_physics(ctx, dt);
        
        // Publish to shared memory
        *ctx->shared = ctx->state;
        
        // Set command based on phase
        MissileCommand cmd = CMD_MISSILE_UPDATE;
        if (ctx->state.is_maneuvering) cmd = CMD_MISSILE_MANEUVER;
        if (ctx->state.phase == MISSILE_TERMINAL) cmd = CMD_MISSILE_TERMINAL;
        
        bifrost_writer_set_command(ctx->writer, cmd);
        bifrost_writer_publish(ctx->writer);
        
        g_total_updates[ctx->id]++;
    }
    
    // Final state
    if (ctx->state.phase == MISSILE_IMPACT) {
        printf("💥 Missile %02d IMPACT! Total updates: %lu\n", 
               ctx->id, (unsigned long)g_total_updates[ctx->id]);
    }
    
    bifrost_writer_leave(ctx->writer);
    return NULL;
}

// ═══════════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════════

int main(void) {
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║  🚀 Bifrost Hypersonic Missile Simulator                   ║\n");
    printf("║  Simulating %d missiles at Mach 8-15                          ║\n", NUM_MISSILES);
    printf("╚═══════════════════════════════════════════════════════════════╝\n\n");
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    srand(time(NULL));
    
    // Create pool
    printf("📦 Creating radar tracking pool...\n");
    bifrost_pool_config_t config;
    bifrost_config_init(&config);
    config.initial_size = 4 * 1024 * 1024;  // 4 MB for multiple writers
    config.max_participants = 32;
    
    g_pool = bifrost_create_pool(HYPER_POOL_NAME, &config);
    if (!g_pool) {
        printf("❌ Failed to create pool: %s\n", bifrost_error_string(bifrost_get_last_error()));
        return 1;
    }
    printf("✅ Pool created: %s (4 MB)\n\n", HYPER_POOL_NAME);
    
    // Launch missile threads
    printf("🎯 Launching %d hypersonic missiles...\n\n", NUM_MISSILES);
    
    pthread_t threads[NUM_MISSILES];
    MissileContext contexts[NUM_MISSILES];
    
    for (int i = 0; i < NUM_MISSILES; i++) {
        contexts[i].id = i;
        pthread_create(&threads[i], NULL, missile_thread, &contexts[i]);
        usleep(50000);  // 50ms stagger
    }
    
    printf("\n⏱️  Simulation running for %d seconds...\n", SIMULATION_TIME_S);
    printf("   Start the radar_tracker in another terminal!\n\n");
    
    // Wait for simulation time or Ctrl+C
    for (int i = 0; i < SIMULATION_TIME_S && g_running; i++) {
        sleep(1);
        
        uint64_t total = 0;
        for (int j = 0; j < NUM_MISSILES; j++) {
            total += g_total_updates[j];
        }
        printf("\r⏰ Time: %2d/%d sec | Total updates: %lu (%.2f M/sec)    ",
               i+1, SIMULATION_TIME_S, (unsigned long)total, total / ((i+1) * 1e6));
        fflush(stdout);
    }
    
    g_running = 0;
    
    // Wait for threads
    for (int i = 0; i < NUM_MISSILES; i++) {
        pthread_join(threads[i], NULL);
    }
    
    // Statistics
    printf("\n\n╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║  📊 Simulation Complete                                       ║\n");
    printf("╠═══════════════════════════════════════════════════════════════╣\n");
    
    uint64_t total = 0;
    for (int i = 0; i < NUM_MISSILES; i++) {
        printf("║  Missile %02d: %12lu updates                               ║\n",
               i, (unsigned long)g_total_updates[i]);
        total += g_total_updates[i];
    }
    
    printf("╠═══════════════════════════════════════════════════════════════╣\n");
    printf("║  TOTAL: %lu updates                                    ║\n", (unsigned long)total);
    printf("║  Rate:  %.2f million updates/sec                             ║\n", total / (SIMULATION_TIME_S * 1e6));
    printf("╚═══════════════════════════════════════════════════════════════╝\n");
    
    bifrost_destroy_pool(g_pool);
    printf("\n👋 Simulation ended.\n");
    
    return 0;
}
