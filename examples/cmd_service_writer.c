/**
 * Command Layer Example - Service (Writer)
 *
 * A "power daemon" that keeps publishing its state on the fast data path AND
 * offers commands that readers can discover and invoke.
 *
 *   ./pool_director       (or let this example create the pool)
 *   ./cmd_service_writer
 *   ./cmd_client_reader
 */

#define _GNU_SOURCE
#include "bifrost.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#define POOL_NAME   "cmd_demo_pool"
#define WRITER_NAME "power_daemon"

enum { CMD_SET_BRIGHTNESS = 1, CMD_GET_STATE = 2, CMD_SET_PROFILE = 3 };

typedef struct {
    uint32_t brightness;    /* 0..100 */
    uint32_t battery;       /* 0..100 */
    char     profile[16];
    uint64_t tick;
} power_state_t;

static volatile sig_atomic_t g_run = 1;
static void on_sig(int s) { (void)s; g_run = 0; }

static power_state_t g_local;   /* authoritative state, owned by the writer */

/* --- Command handlers (run in the writer thread, between publishes) --- */

static int32_t h_set_brightness(const bifrost_cmd_request_t* req, void* reply, uint32_t* len, void* ctx) {
    (void)reply; (void)ctx;
    uint8_t level = req->args[0];
    if (level > 100) { *len = 0; return BIFROST_ERR_INVALID; }
    g_local.brightness = level;
    *len = 0;
    printf("  ← set_brightness(%u) from pid %d\n", level, req->sender_pid);
    return BIFROST_OK;
}

static int32_t h_get_state(const bifrost_cmd_request_t* req, void* reply, uint32_t* len, void* ctx) {
    (void)req; (void)ctx;
    memcpy(reply, &g_local, sizeof g_local);
    *len = sizeof g_local;
    return BIFROST_OK;
}

static int32_t h_set_profile(const bifrost_cmd_request_t* req, void* reply, uint32_t* len, void* ctx) {
    (void)ctx;
    char p[16] = {0};
    memcpy(p, req->args, req->arg_len < 15 ? req->arg_len : 15);
    if (strcmp(p, "eco") && strcmp(p, "balanced") && strcmp(p, "performance")) {
        const char* msg = "unknown profile";
        *len = (uint32_t)strlen(msg);
        memcpy(reply, msg, *len);
        return BIFROST_ERR_INVALID;
    }
    snprintf(g_local.profile, sizeof g_local.profile, "%s", p);
    *len = 0;
    printf("  ← set_profile(%s)\n", p);
    return BIFROST_OK;
}

int main(void) {
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    
    bifrost_pool_t* pool = NULL;
    if (!bifrost_pool_exists(POOL_NAME)) {
        pool = bifrost_create_pool(POOL_NAME, NULL);   /* defaults: commands enabled */
        if (!pool) { fprintf(stderr, "create: %s\n", bifrost_error_string(bifrost_get_last_error())); return 1; }
    }
    
    bifrost_writer_t* w = bifrost_join_as_writer(POOL_NAME, WRITER_NAME);
    if (!w) { fprintf(stderr, "join: %s\n", bifrost_error_string(bifrost_get_last_error())); return 1; }
    
    /* 1. Publish the catalog readers will browse */
    bifrost_cmd_desc_t cmds[] = {
        { .id = CMD_SET_BRIGHTNESS, .arg_min = 1, .arg_max = 1,
          .name = "set_brightness", .desc = "Set screen brightness. arg: uint8 0-100" },
        { .id = CMD_GET_STATE, .arg_min = 0, .arg_max = 0, .reply_max = sizeof(power_state_t),
          .name = "get_state", .desc = "Return the full power_state_t snapshot" },
        { .id = CMD_SET_PROFILE, .arg_min = 1, .arg_max = 15, .reply_max = 32,
          .name = "set_profile", .desc = "eco | balanced | performance" },
    };
    bifrost_cmd_handler_fn fns[] = { h_set_brightness, h_get_state, h_set_profile };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        bifrost_writer_register_command(w, &cmds[i]);
        bifrost_writer_on_command(w, cmds[i].id, fns[i], NULL);
    }
    
    /* 2. Fast data path: allocate once, publish forever */
    power_state_t* shared = bifrost_writer_alloc(w, sizeof *shared);
    g_local = (power_state_t){ .brightness = 50, .battery = 100, .profile = "balanced" };
    
    printf("✅ %s ready (writer id %u) - %zu commands published\n",
           WRITER_NAME, bifrost_writer_get_id(w), sizeof cmds / sizeof cmds[0]);
    
    while (g_run) {
        /* Produce data without waiting for anyone */
        g_local.tick++;
        if (g_local.tick % 50 == 0 && g_local.battery > 0) g_local.battery--;
        *shared = g_local;
        bifrost_writer_publish(w);
        
        /* Serve commands: costs one atomic load when the inbox is empty.
         * wait_command() sleeps on a futex but wakes instantly on a request. */
        bifrost_writer_dispatch(w, 32);
        bifrost_writer_wait_command(w, 20 * 1000 * 1000);   /* ~50 Hz publish rate */
    }
    
    printf("\nShutting down (dropped replies: %llu)\n",
           (unsigned long long)bifrost_writer_replies_dropped(w));
    bifrost_writer_leave(w);
    if (pool) bifrost_destroy_pool(pool);
    return 0;
}
