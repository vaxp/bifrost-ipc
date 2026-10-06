/**
 * Bifrost Example - Pool Director
 * 
 * This example shows how to create and manage a shared memory pool.
 * Run this first before starting any writers or readers.
 */

#include <bifrost.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

static volatile int running = 1;

void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

int main(int argc, char* argv[]) {
    const char* pool_name = "demo_pool";
    
    if (argc > 1) {
        pool_name = argv[1];
    }
    
    printf("Bifrost Pool Director\n");
    printf("========================\n");
    printf("Library version: %s\n\n", bifrost_version());
    
    /* Check if pool already exists */
    if (bifrost_pool_exists(pool_name)) {
        printf("Pool '%s' already exists. Opening existing pool...\n", pool_name);
        
        bifrost_pool_t* pool = bifrost_open_pool(pool_name);
        if (!pool) {
            printf("Error opening pool: %s\n", bifrost_error_string(bifrost_get_last_error()));
            return 1;
        }
        
        bifrost_pool_stats_t stats;
        bifrost_get_pool_stats(pool, &stats);
        
        printf("\nPool Statistics:\n");
        printf("  Total size: %zu bytes\n", stats.total_size);
        printf("  Used size:  %zu bytes\n", stats.used_size);
        printf("  Free size:  %zu bytes\n", stats.free_size);
        printf("  Writers:    %u\n", stats.writer_count);
        printf("  Readers:    %u\n", stats.reader_count);
        printf("  Total writes: %lu\n", stats.total_writes);
        printf("  Total reads:  %lu\n", stats.total_reads);
        
        bifrost_close_pool(pool);
        return 0;
    }
    
    /* Create new pool */
    printf("Creating new pool '%s'...\n", pool_name);
    
    bifrost_pool_config_t config;
    bifrost_config_init(&config);
    config.initial_size = 4 * 1024 * 1024;  /* 4 MB */
    config.max_participants = 128;
    
    bifrost_pool_t* pool = bifrost_create_pool(pool_name, &config);
    if (!pool) {
        printf("Error creating pool: %s\n", bifrost_error_string(bifrost_get_last_error()));
        return 1;
    }
    
    printf("Pool created successfully!\n");
    printf("  Initial size: %zu bytes\n", config.initial_size);
    printf("  Max participants: %u\n", config.max_participants);
    printf("\nPool is ready for writers and readers.\n");
    printf("Press Ctrl+C to destroy the pool and exit.\n\n");
    
    /* Set up signal handlers */
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    
    /* Monitor loop */
    while (running) {
        bifrost_pool_stats_t stats;
        bifrost_get_pool_stats(pool, &stats);
        
        printf("\r[Writers: %u | Readers: %u | Writes: %lu | Reads: %lu | Used: %zu/%zu]   ",
               stats.writer_count, stats.reader_count,
               stats.total_writes, stats.total_reads,
               stats.used_size, stats.total_size);
        fflush(stdout);
        
        sleep(1);
    }
    
    printf("\n\nShutting down...\n");
    
    /* Destroy pool */
    bifrost_destroy_pool(pool);
    printf("Pool destroyed.\n");
    
    return 0;
}
