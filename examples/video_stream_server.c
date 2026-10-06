/**
 * Bifrost 4K Video Stream Server
 * 
 * Simulates a Raw 4K Camera source (3840x2160 RGBA).
 * Pushes ~33MB frames as fast as possible to test extreme throughput.
 */

#define _GNU_SOURCE
#include <bifrost.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define POOL_NAME       "video_pool"
#define SERVER_NAME     "camera_8k"

#define WIDTH           7680
#define HEIGHT          4320
#define CHANNELS        4       /* RGBA */
#define FRAME_SIZE      (WIDTH * HEIGHT * CHANNELS)

/* ============================================================================
 * Data Structure
 * ============================================================================ */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t frame_id;
    uint64_t timestamp_ns;
    
    /* The raw video frame (approx 33 MB) */
    uint8_t buffer[FRAME_SIZE];
} video_frame_t;

#define VIDEO_MAGIC 0x5649444F /* "VIDO" */

/* ============================================================================
 * Globals
 * ============================================================================ */

static bifrost_pool_t* g_pool = NULL;
static bifrost_writer_t* g_writer = NULL;
static video_frame_t* g_frame = NULL;
static volatile int g_running = 1;

/* ============================================================================
 * Utils
 * ============================================================================ */

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║   🎥 Bifrost 8K Video Stream Server (Raw RGBA)             ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n\n");
    
    printf("📋 Specs: 8K (7680x4320) @ 32-bit RGBA\n");
    printf("   Frame Size: %.2f MB\n", (double)FRAME_SIZE / 1024 / 1024);
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* Create Pool - Need large pool for 4K video buffering */
    /* We assume single buffer for now to show raw memcpy speed, 
       but normally you'd want double/triple buffering */
    
    if (bifrost_pool_exists(POOL_NAME)) {
        g_pool = bifrost_open_pool(POOL_NAME);
    } else {
        bifrost_pool_config_t config;
        bifrost_config_init(&config);
        config.initial_size = FRAME_SIZE + 1024 * 1024; /* Frame + Overhead */
        config.max_participants = 4;
        g_pool = bifrost_create_pool(POOL_NAME, &config);
    }
    
    if (!g_pool) {
        fprintf(stderr, "❌ Failed to init pool: %s\n", bifrost_error_string(bifrost_get_last_error()));
        return 1;
    }
    
    /* Join Writer */
    g_writer = bifrost_join_as_writer(POOL_NAME, SERVER_NAME);
    
    /* Alloc Buffer */
    g_frame = bifrost_writer_alloc(g_writer, sizeof(video_frame_t));
    if (!g_frame) {
        fprintf(stderr, "❌ Failed to alloc buffer (Pool too small?)\n");
        bifrost_writer_leave(g_writer);
        return 1;
    }
    
    printf("✅ Buffer allocated. Streaming started...\n\n");
    
    uint64_t frame_count = 0;
    uint32_t pixel_val = 0;
    
    while (g_running) {
        /* Generate content (Simulate camera capture) */
        /* We just fill the first few lines to save CPU, but update ID/Timestamp */
        /* In a real benchmark, we might verify data integrity, but here we want max throughput */
        
        g_frame->magic = VIDEO_MAGIC;
        g_frame->frame_id = ++frame_count;
        g_frame->timestamp_ns = get_time_ns();
        
        /* Force memory write of the FULL buffer (33 MB) to stress bandwidth */
        /* Using memset is efficient but still touches all cache lines */
        memset(g_frame->buffer, (pixel_val++) & 0xFF, FRAME_SIZE);
        
        bifrost_writer_publish(g_writer);
        
        if (frame_count % 100 == 0) {
            printf("\r🚀 Streamed Frames: %lu      ", frame_count);
            fflush(stdout);
        }
        
        /* NO SLEEP! Maximum throughput! */
    }
    
    printf("\n👋 Exiting...\n");
    bifrost_writer_leave(g_writer);
    
    /* Cleanup */
    bifrost_pool_stats_t s;
    bifrost_get_pool_stats(g_pool, &s);
    if (s.writer_count == 0 && s.reader_count == 0) {
        bifrost_destroy_pool(g_pool);
    } else {
        bifrost_close_pool(g_pool);
    }
    
    return 0;
}
