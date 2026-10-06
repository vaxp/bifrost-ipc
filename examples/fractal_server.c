/**
 * Bifrost Fractal Explorer - Server
 * 
 * Demonstrates heavy computational load + high throughput streaming.
 * Generates Mandelbrot Set frames and streams them to the client.
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
#include <complex.h>

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define POOL_NAME       "fractal_pool"
#define SERVER_NAME     "fractal_server"

#define WIDTH           160     /* Console width chars */
#define HEIGHT          50      /* Console height chars */
#define MAX_ITER        100     /* Iterations per pixel */
#define ZOOM_SPEED      1.01    /* Zoom factor per frame */

/* ============================================================================
 * Data Structure
 * ============================================================================ */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t frame_id;
    uint64_t timestamp_ns;
    
    double center_x;
    double center_y;
    double zoom;
    
    /* The frame buffer - 1 byte per pixel (color index) */
    uint8_t buffer[WIDTH * HEIGHT];
    
    /* Server stats */
    double compute_time_ms;
    double fps;
} fractal_frame_t;

#define FRACTAL_MAGIC 0x4D425254 /* "MBRT" */

/* ============================================================================
 * Globals
 * ============================================================================ */

static bifrost_pool_t* g_pool = NULL;
static bifrost_writer_t* g_writer = NULL;
static fractal_frame_t* g_frame = NULL;
static volatile int g_running = 1;

/* Animation State */
static double g_cx = -0.743643887037158704752191506114774;
static double g_cy = 0.131825904205311970493132056385139;
static double g_zoom = 1.0;

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
 * Mandelbrot Calculation (The Heavy Work)
 * ============================================================================ */

static void render_frame(void) {
    double start_time = (double)get_time_ns();
    
    double w = 3.0 / g_zoom;
    double h = w * ((double)HEIGHT / WIDTH) * 2.0; /* Aspect ratio correction for text pixels (2:1 usually) */
    
    double x_start = g_cx - w / 2.0;
    double y_start = g_cy - h / 2.0;
    
    double dx = w / WIDTH;
    double dy = h / HEIGHT;
    
    for (int y = 0; y < HEIGHT; y++) {
        double cy = y_start + y * dy;
        for (int x = 0; x < WIDTH; x++) {
            double cx = x_start + x * dx;
            
            /* Optimized inner loop */
            double zx = 0.0;
            double zy = 0.0;
            double zx2 = 0.0;
            double zy2 = 0.0;
            int iter = 0;
            
            while (zx2 + zy2 <= 4.0 && iter < MAX_ITER) {
                zy = 2.0 * zx * zy + cy;
                zx = zx2 - zy2 + cx;
                zx2 = zx * zx;
                zy2 = zy * zy;
                iter++;
            }
            
            /* Map iterations to color index (0-255) */
            g_frame->buffer[y * WIDTH + x] = (iter == MAX_ITER) ? 0 : (iter % 255 + 1);
        }
    }
    
    /* Fill metadata */
    g_frame->magic = FRACTAL_MAGIC;
    g_frame->frame_id++;
    g_frame->timestamp_ns = get_time_ns();
    g_frame->center_x = g_cx;
    g_frame->center_y = g_cy;
    g_frame->zoom = g_zoom;
    
    double end_time = (double)get_time_ns();
    g_frame->compute_time_ms = (end_time - start_time) / 1e6;
    g_frame->fps = 1000.0 / (g_frame->compute_time_ms + 1e-6);
    
    /* Update Zoom for next frame */
    g_zoom *= ZOOM_SPEED;
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║   🌌 Bifrost Fractal Server (Heavy Compute)                ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n\n");
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* Create/Open Pool */
    if (bifrost_pool_exists(POOL_NAME)) {
        g_pool = bifrost_open_pool(POOL_NAME);
    } else {
        bifrost_pool_config_t config;
        bifrost_config_init(&config);
        config.initial_size = 10 * 1024 * 1024; /* 10 MB */
        config.max_participants = 16;
        g_pool = bifrost_create_pool(POOL_NAME, &config);
    }
    
    if (!g_pool) {
        fprintf(stderr, "❌ Failed to init pool\n");
        return 1;
    }
    
    /* Join Writer */
    g_writer = bifrost_join_as_writer(POOL_NAME, SERVER_NAME);
    
    /* Alloc Buffer */
    g_frame = bifrost_writer_alloc(g_writer, sizeof(fractal_frame_t));
    if (!g_frame) {
        fprintf(stderr, "❌ Failed to alloc buffer\n");
        return 1;
    }
    
    printf("🚀 Calculating Mandelbrot set at 160x50 resolution...\n");
    printf("⚡ Streaming frames to shared memory (Lock-Free)...\n");
    
    while (g_running) {
        render_frame();
        
        bifrost_writer_publish(g_writer);
        
        printf("\rFrame: %6u | Zoom: %.2e | Compute: %5.2f ms | FPS: %5.1f    ",
               g_frame->frame_id, g_zoom, g_frame->compute_time_ms, g_frame->fps);
        fflush(stdout);
        
        /* No artificial cap - max speed! */
        // if (g_frame->fps > 100) usleep(10000);
    }
    
    printf("\n👋 Exiting...\n");
    bifrost_writer_leave(g_writer);
    
    /* Cleanup if last */
    bifrost_pool_stats_t s;
    bifrost_get_pool_stats(g_pool, &s);
    if (s.writer_count == 0 && s.reader_count == 0) {
        bifrost_destroy_pool(g_pool);
    } else {
        bifrost_close_pool(g_pool);
    }
    
    return 0;
}
