/**
 * Bifrost Fractal Explorer - Client (Renderer)
 * 
 * Reads fractal frames from shared memory and renders them in ASCII art.
 * Measures end-to-end latency from calculation to display.
 */

#define _GNU_SOURCE
#include <bifrost.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <math.h>

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define POOL_NAME       "fractal_pool"
#define SERVER_NAME     "fractal_server"
#define CLIENT_NAME     "fractal_renderer"

#define WIDTH           160
#define HEIGHT          50

/* ============================================================================
 * Data Structure (Must match Server)
 * ============================================================================ */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t frame_id;
    uint64_t timestamp_ns;
    
    double center_x;
    double center_y;
    double zoom;
    
    uint8_t buffer[WIDTH * HEIGHT];
    
    double compute_time_ms;
    double fps;
} fractal_frame_t;

#define FRACTAL_MAGIC 0x4D425254

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

/* ANSI Colors for Heatmap (Blue -> Green -> Yellow -> Red -> White) */
static const char* get_color_ansi(uint8_t val) {
    if (val == 0) return "\033[40m ";       /* Black (Inside set) */
    if (val < 10) return "\033[44m ";       /* Dark Blue */
    if (val < 20) return "\033[104m ";      /* Light Blue */
    if (val < 40) return "\033[46m ";       /* Cyan */
    if (val < 60) return "\033[42m ";       /* Green */
    if (val < 100) return "\033[43m ";      /* Yellow */
    if (val < 150) return "\033[41m ";      /* Red */
    if (val < 200) return "\033[101m ";     /* Light Red */
    return "\033[47m ";                     /* White */
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* Unique Client Name */
    char name[64];
    snprintf(name, sizeof(name), "%s_%d", CLIENT_NAME, getpid());

    /* Wait for pool */
    printf("⏳ Waiting for Fractal Server...\n");
    while (!bifrost_pool_exists(POOL_NAME) && g_running) usleep(100000);
    
    if (!g_running) return 0;
    
    /* Connect */
    g_reader = bifrost_join_as_reader(POOL_NAME, name);
    if (!g_reader) {
        fprintf(stderr, "❌ Connection failed\n");
        return 1;
    }
    
    printf("✅ Connected! Rendering stream...\n");
    
    /* Buffer for double buffering to prevent flickering */
    char render_buf[WIDTH * HEIGHT * 20]; /* Plenty of space for ANSI codes */
    
    uint64_t last_frame = 0;
    
    while (g_running) {
        size_t size;
        fractal_frame_t* frame = bifrost_reader_get(g_reader, SERVER_NAME, &size);
        
        if (!frame || frame->magic != FRACTAL_MAGIC) {
            __asm__ __volatile__("pause" ::: "memory"); /* Spin wait */
            continue;
        }
        
        uint64_t current_id = frame->frame_id;
        if (current_id > last_frame) {
            uint64_t now = get_time_ns();
            double latency_us = (now - frame->timestamp_ns) / 1000.0;
            
            /* Build render buffer */
            char* ptr = render_buf;
            ptr += sprintf(ptr, "\033[H"); /* Home cursor */
            
            /* Add Header Stats */
            ptr += sprintf(ptr, "\033[0mFrame: %u | Zoom: %.2e | Compute: %.2fms | \033[1;33mLatency: %.2f µs\033[0m\n", 
                           frame->frame_id, frame->zoom, frame->compute_time_ms, latency_us);
            
            /* Render pixels */
            for (int y = 0; y < HEIGHT; y++) {
                for (int x = 0; x < WIDTH; x++) {
                    uint8_t pixel = frame->buffer[y * WIDTH + x];
                    const char* ansi = get_color_ansi(pixel);
                    while (*ansi) *ptr++ = *ansi++;
                }
                *ptr++ = '\033'; *ptr++ = '['; *ptr++ = '0'; *ptr++ = 'm'; /* Reset at end of line */
                *ptr++ = '\n';
            }
            *ptr = '\0';
            
            /* Flush to screen */
            printf("%s", render_buf);
            
            last_frame = current_id;
        }
        /* No sleep - push as fast as terminal allows */
    }
    
    printf("\033[0m\n👋 Disconnected.\n");
    bifrost_reader_leave(g_reader);
    return 0;
}
