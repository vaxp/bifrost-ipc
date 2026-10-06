/**
 * Bifrost Example - Daemon Writer
 * 
 * Example of a system daemon that writes data to shared memory.
 * Simulates a battery monitor daemon.
 */

#include <bifrost.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

/* Commands that this daemon provides */
#define CMD_BATTERY_UPDATE    1
#define CMD_POWER_STATUS      2
#define CMD_CHARGING_STATE    3

/* Data structure for battery info */
typedef struct {
    int percentage;           /* 0-100 */
    int charging;            /* 0 = discharging, 1 = charging */
    int time_remaining;      /* seconds */
    char status[32];         /* "Charging", "Discharging", "Full" */
    uint64_t timestamp;
} battery_info_t;

static volatile int running = 1;

void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

/* Simulate reading battery info */
static void read_battery(battery_info_t* info) {
    static int percentage = 75;
    static int direction = -1;  /* -1 = discharging, 1 = charging */
    
    /* Simulate battery changes */
    percentage += direction;
    if (percentage <= 20) {
        direction = 1;  /* Start charging */
    } else if (percentage >= 100) {
        direction = -1;  /* Unplug */
    }
    
    info->percentage = percentage;
    info->charging = (direction == 1);
    info->time_remaining = (direction == 1) 
        ? (100 - percentage) * 120  /* 2 min per percent when charging */
        : percentage * 180;          /* 3 min per percent when discharging */
    
    if (percentage == 100) {
        strcpy(info->status, "Full");
    } else if (direction == 1) {
        strcpy(info->status, "Charging");
    } else {
        strcpy(info->status, "Discharging");
    }
    
    /* Timestamp */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    info->timestamp = (uint64_t)ts.tv_sec;
}

int main(int argc, char* argv[]) {
    const char* pool_name = "demo_pool";
    const char* writer_name = "battery_daemon";
    int interval_ms = 500;  /* Update every 500ms */
    
    if (argc > 1) pool_name = argv[1];
    if (argc > 2) writer_name = argv[2];
    if (argc > 3) interval_ms = atoi(argv[3]);
    
    printf("Bifrost Daemon Writer Example\n");
    printf("================================\n");
    printf("Pool:     %s\n", pool_name);
    printf("Writer:   %s\n", writer_name);
    printf("Interval: %d ms\n\n", interval_ms);
    
    /* Join pool as writer */
    bifrost_writer_t* writer = bifrost_join_as_writer(pool_name, writer_name);
    if (!writer) {
        printf("Error joining pool: %s\n", bifrost_error_string(bifrost_get_last_error()));
        return 1;
    }
    
    printf("Joined pool successfully! Writer ID: %u\n", bifrost_writer_get_id(writer));
    printf("Press Ctrl+C to exit.\n\n");
    
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    
    /* Main loop */
    while (running) {
        /* Allocate space for our data - returns direct pointer */
        battery_info_t* info = bifrost_writer_alloc(writer, sizeof(battery_info_t));
        if (!info) {
            printf("Error: Failed to allocate space!\n");
            break;
        }
        
        /* Write directly to shared memory - NO memcpy! */
        read_battery(info);
        
        /* Set command to indicate what type of update this is */
        bifrost_writer_set_command(writer, CMD_BATTERY_UPDATE);
        
        /* Publish - makes data visible to readers */
        bifrost_writer_publish(writer);
        
        printf("\r[v%lu] Battery: %d%% | %s | %d sec remaining   ",
               bifrost_writer_get_version(writer),
               info->percentage,
               info->status,
               info->time_remaining);
        fflush(stdout);
        
        usleep(interval_ms * 1000);
    }
    
    printf("\n\nLeaving pool...\n");
    bifrost_writer_leave(writer);
    printf("Done.\n");
    
    return 0;
}
