/**
 * Bifrost Multi-Process Tests
 * 
 * Tests for multi-process communication.
 */

#include <bifrost.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>

#define TEST_POOL "mp_test_pool"

typedef struct {
    int counter;
    pid_t writer_pid;
    char message[64];
} shared_data_t;

/* ============================================================================
 * Writer Process
 * ============================================================================ */

int run_writer(int num_writes) {
    bifrost_writer_t* writer = bifrost_join_as_writer(TEST_POOL, "mp_writer");
    if (!writer) {
        fprintf(stderr, "Writer: Failed to join: %s\n", 
                bifrost_error_string(bifrost_get_last_error()));
        return 1;
    }
    
    printf("Writer [PID %d]: Joined successfully\n", getpid());
    
    for (int i = 0; i < num_writes; i++) {
        shared_data_t* data = bifrost_writer_alloc(writer, sizeof(shared_data_t));
        if (!data) {
            fprintf(stderr, "Writer: Alloc failed at %d\n", i);
            break;
        }
        
        /* Write directly */
        data->counter = i + 1;
        data->writer_pid = getpid();
        snprintf(data->message, sizeof(data->message), "Message #%d", i + 1);
        
        bifrost_writer_set_command(writer, i + 1);
        bifrost_writer_publish(writer);
        
        /* Small delay */
        usleep(10000);  /* 10ms */
    }
    
    printf("Writer [PID %d]: Finished %d writes\n", getpid(), num_writes);
    
    /* Keep writer alive briefly for reader to catch up */
    usleep(100000);
    
    bifrost_writer_leave(writer);
    return 0;
}

/* ============================================================================
 * Reader Process
 * ============================================================================ */

int run_reader(int expected_max) {
    bifrost_reader_t* reader = bifrost_join_as_reader(TEST_POOL, "mp_reader");
    if (!reader) {
        fprintf(stderr, "Reader: Failed to join: %s\n", 
                bifrost_error_string(bifrost_get_last_error()));
        return 1;
    }
    
    printf("Reader [PID %d]: Joined successfully\n", getpid());
    
    int reads = 0;
    int last_counter = 0;
    int attempts = 0;
    int max_attempts = 1000;  /* Timeout after ~10 seconds */
    
    while (attempts < max_attempts) {
        size_t size;
        shared_data_t* data = bifrost_reader_get(reader, "mp_writer", &size);
        
        if (data && data->counter > last_counter) {
            last_counter = data->counter;
            reads++;
            
            if (last_counter >= expected_max) {
                break;
            }
        }
        
        usleep(10000);  /* 10ms */
        attempts++;
    }
    
    printf("Reader [PID %d]: Read %d updates, last counter = %d\n", 
           getpid(), reads, last_counter);
    
    bifrost_reader_leave(reader);
    
    /* Success if we got at least some reads */
    return (reads > 0 && last_counter == expected_max) ? 0 : 1;
}

/* ============================================================================
 * Main Test
 * ============================================================================ */

int main(void) {
    printf("\nBifrost Multi-Process Test\n");
    printf("==============================\n");
    printf("Library version: %s\n\n", bifrost_version());
    
    /* Clean up any stale pool */
    if (bifrost_pool_exists(TEST_POOL)) {
        printf("Cleaning up stale test pool...\n");
        bifrost_pool_t* p = bifrost_open_pool(TEST_POOL);
        if (p) bifrost_destroy_pool(p);
    }
    
    /* Create pool in parent */
    printf("Creating shared memory pool...\n");
    bifrost_pool_config_t config;
    bifrost_config_init(&config);
    config.initial_size = 1024 * 1024;  /* 1MB */
    
    bifrost_pool_t* pool = bifrost_create_pool(TEST_POOL, &config);
    if (!pool) {
        fprintf(stderr, "Failed to create pool: %s\n", 
                bifrost_error_string(bifrost_get_last_error()));
        return 1;
    }
    
    printf("Pool created. Starting child processes...\n\n");
    
    int num_messages = 50;
    
    /* Fork writer */
    pid_t writer_pid = fork();
    if (writer_pid == 0) {
        /* Child - writer */
        bifrost_close_pool(pool);  /* Close parent's handle */
        exit(run_writer(num_messages));
    }
    
    /* Small delay to let writer join first */
    usleep(50000);
    
    /* Fork reader */
    pid_t reader_pid = fork();
    if (reader_pid == 0) {
        /* Child - reader */
        bifrost_close_pool(pool);
        exit(run_reader(num_messages));
    }
    
    /* Parent waits for children */
    int writer_status, reader_status;
    waitpid(writer_pid, &writer_status, 0);
    waitpid(reader_pid, &reader_status, 0);
    
    printf("\nProcess results:\n");
    printf("  Writer exit: %d\n", WEXITSTATUS(writer_status));
    printf("  Reader exit: %d\n", WEXITSTATUS(reader_status));
    
    /* Get final stats */
    bifrost_pool_stats_t stats;
    bifrost_get_pool_stats(pool, &stats);
    printf("\nPool statistics:\n");
    printf("  Total writes: %lu\n", stats.total_writes);
    printf("  Total reads:  %lu\n", stats.total_reads);
    printf("  Used memory:  %zu bytes\n", stats.used_size);
    
    /* Cleanup */
    bifrost_destroy_pool(pool);
    
    /* Check results */
    int success = (WEXITSTATUS(writer_status) == 0 && 
                   WEXITSTATUS(reader_status) == 0);
    
    printf("\n==============================\n");
    if (success) {
        printf("TEST \033[32mPASSED\033[0m\n");
    } else {
        printf("TEST \033[31mFAILED\033[0m\n");
    }
    
    return success ? 0 : 1;
}
