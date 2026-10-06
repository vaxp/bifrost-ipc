/**
 * Bifrost 4K Video Stream Client (Meter)
 * 
 * Reads 33MB 4K frames and calculates realtime FPS/Bandwidth/Latency.
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
#include <float.h>

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define POOL_NAME       "video_pool"
#define SERVER_NAME     "camera_8k"
#define CLIENT_NAME     "video_meter_8k"

#define WIDTH           7680
#define HEIGHT          4320
#define CHANNELS        4
#define FRAME_SIZE      (WIDTH * HEIGHT * CHANNELS)

/* ============================================================================
 * Data Structure
 * ============================================================================ */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t frame_id;
    uint64_t timestamp_ns;
    uint8_t buffer[FRAME_SIZE];
} video_frame_t;

#define VIDEO_MAGIC 0x5649444F

/* ============================================================================
 * Globals
 * ============================================================================ */

static bifrost_reader_t* g_reader = NULL;
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
    printf("║   📉 Bifrost 8K Bandwidth Meter                            ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n\n");
    
    signal(SIGINT, signal_handler);
    
    char name[64];
    snprintf(name, sizeof(name), "%s_%d", CLIENT_NAME, getpid());

    while (!bifrost_pool_exists(POOL_NAME) && g_running) {
        printf("\r⏳ Waiting for 8K Camera... ");
        fflush(stdout);
        usleep(100000);
    }
    printf("\n");
    
    g_reader = bifrost_join_as_reader(POOL_NAME, name);
    if (!g_reader) {
        fprintf(stderr, "❌ Connection failed\n");
        return 1;
    }
    
    printf("✅ Connected. Measuring...\n\n");
    
    uint64_t last_frame_id = 0;
    uint64_t frames_in_sec = 0;
    uint64_t bytes_in_sec = 0;
    uint64_t start_sec = get_time_ns();
    
    double lat_min = DBL_MAX, lat_max = 0, lat_sum = 0;
    uint64_t lat_count = 0;
    
    while (g_running) {
        size_t size;
        /* WARNING: Accessing FRAME_SIZE (33MB) directly from shared memory (Zero Copy) */
        video_frame_t* frame = bifrost_reader_get(g_reader, SERVER_NAME, &size);
        
        if (!frame || frame->magic != VIDEO_MAGIC) {
            __asm__ __volatile__("pause" ::: "memory");
            continue;
        }
        
        if (frame->frame_id > last_frame_id) {
            last_frame_id = frame->frame_id;
            
            /* FORCE MEMORY READ: Calculate checksum of the frame */
            /* This ensures we are measuring true memory bandwidth */
            uint64_t sum = 0;
            const uint64_t* ptr = (const uint64_t*)frame->buffer;
            size_t words = FRAME_SIZE / 8;
            
            /* Unrolled loop for speed */
            for (size_t i = 0; i < words; i += 8) {
                sum += ptr[i];
                if (i + 7 < words) { /* Prefetch hint could go here */
                    sum += ptr[i+1] + ptr[i+2] + ptr[i+3];
                    sum += ptr[i+4] + ptr[i+5] + ptr[i+6] + ptr[i+7];
                }
            }
            volatile uint64_t dummy = sum; /* Prevent optimization */
            (void)dummy;

            /* Measure Latency */
            uint64_t now = get_time_ns();
            /* Handle potential clock skew or wrapping */
            double lat_us = (now > frame->timestamp_ns) ? 
                           (double)(now - frame->timestamp_ns) / 1000.0 : 0.0;
            
            if (lat_us < lat_min) lat_min = lat_us;
            if (lat_us > lat_max) lat_max = lat_us;
            lat_sum += lat_us;
            lat_count++;
            
            /* Update counters */
            frames_in_sec++;
            bytes_in_sec += FRAME_SIZE;
            
            /* Report every second */
            if (now - start_sec >= 1000000000ULL) {
                double bw_gb = (double)bytes_in_sec / 1024.0 / 1024.0 / 1024.0;
                double avg_lat = lat_count > 0 ? lat_sum / lat_count : 0.0;
                
                printf("\rFPS: %4lu | Bandwidth: %6.2f GB/s | Latency: %6.2f µs (Avg) %6.2f µs (Min)   ",
                       frames_in_sec, bw_gb, avg_lat, lat_min);
                fflush(stdout);
                
                /* Reset */
                frames_in_sec = 0;
                bytes_in_sec = 0;
                start_sec = now;
                lat_sum = 0; lat_count = 0; lat_min = DBL_MAX; lat_max = 0;
            }
        }
    }
    
    printf("\n👋 Disconnected.\n");
    bifrost_reader_leave(g_reader);
    return 0;
}
