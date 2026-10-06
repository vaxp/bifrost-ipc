/**
 * Bifrost Example - Desktop Environment Reader
 * 
 * Example of a DE component that reads data from daemons.
 * Simulates a status bar widget reading battery info.
 */

#include <bifrost.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>

/* Commands (must match writer) */
#define CMD_BATTERY_UPDATE    1

/* Data structure (must match writer) */
typedef struct {
    int percentage;
    int charging;
    int time_remaining;
    char status[32];
    uint64_t timestamp;
} battery_info_t;

static volatile int running = 1;

void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

void print_battery_bar(int percentage, int charging) {
    int filled = percentage / 10;
    printf("[");
    for (int i = 0; i < 10; i++) {
        if (i < filled) {
            printf("█");
        } else {
            printf("░");
        }
    }
    printf("] %s", charging ? "⚡" : "🔋");
}

int main(int argc, char* argv[]) {
    const char* pool_name = "demo_pool";
    const char* reader_name = "status_bar";
    const char* writer_name = "battery_daemon";
    int poll_ms = 100;
    
    if (argc > 1) pool_name = argv[1];
    if (argc > 2) reader_name = argv[2];
    if (argc > 3) writer_name = argv[3];
    if (argc > 4) poll_ms = atoi(argv[4]);
    
    printf("Bifrost DE Reader Example\n");
    printf("============================\n");
    printf("Pool:       %s\n", pool_name);
    printf("Reader:     %s\n", reader_name);
    printf("Watching:   %s\n", writer_name);
    printf("Poll rate:  %d ms\n\n", poll_ms);
    
    /* Join pool as reader */
    bifrost_reader_t* reader = bifrost_join_as_reader(pool_name, reader_name);
    if (!reader) {
        printf("Error joining pool: %s\n", bifrost_error_string(bifrost_get_last_error()));
        return 1;
    }
    
    printf("Joined pool successfully!\n");
    printf("Press Ctrl+C to exit.\n\n");
    
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    
    uint64_t last_version = 0;
    int updates_received = 0;
    
    /* First, list all available writers */
    printf("Available writers:\n");
    bifrost_writer_info_t writers[16];
    uint32_t writer_count = bifrost_reader_list_writers(reader, writers, 16);
    
    if (writer_count == 0) {
        printf("  (none yet - waiting for writers to join)\n");
    } else {
        for (uint32_t i = 0; i < writer_count; i++) {
            printf("  - %s (ID: %u, cmd: %u, ver: %lu)\n",
                   writers[i].name, writers[i].id,
                   writers[i].current_command, writers[i].version);
        }
    }
    printf("\n");
    
    /* Main polling loop */
    while (running) {
        /* Check for command first */
        uint32_t cmd = bifrost_reader_get_command(reader, writer_name);
        
        if (cmd == CMD_BATTERY_UPDATE) {
            /* Get data version to detect changes */
            uint64_t version = bifrost_reader_get_version(reader, writer_name);
            
            if (version > last_version) {
                /* New data available! Read directly - NO memcpy! */
                size_t data_size;
                battery_info_t* info = bifrost_reader_get(reader, writer_name, &data_size);
                
                if (info) {
                    last_version = version;
                    updates_received++;
                    
                    /* Display the data */
                    printf("\r");
                    print_battery_bar(info->percentage, info->charging);
                    printf(" %3d%% | %s | ETA: %d:%02d | Updates: %d   ",
                           info->percentage,
                           info->status,
                           info->time_remaining / 60,
                           info->time_remaining % 60,
                           updates_received);
                    fflush(stdout);
                }
            }
        } else if (cmd == 0) {
            /* Writer not found or no command yet */
            static int dots = 0;
            printf("\rWaiting for %s", writer_name);
            for (int i = 0; i < (dots % 4); i++) printf(".");
            printf("    ");
            fflush(stdout);
            dots++;
        }
        
        usleep(poll_ms * 1000);
    }
    
    printf("\n\nTotal updates received: %d\n", updates_received);
    printf("Leaving pool...\n");
    bifrost_reader_leave(reader);
    printf("Done.\n");
    
    return 0;
}
