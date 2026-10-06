/**
 * Command Layer Example - Client (Reader)
 *
 * Discovers every writer in the pool, prints the commands each one offers,
 * then invokes some of them on "power_daemon" and reads the data path.
 */

#define _GNU_SOURCE
#include "bifrost.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define POOL_NAME   "cmd_demo_pool"
#define WRITER_NAME "power_daemon"
#define TIMEOUT_NS  (500ull * 1000 * 1000)

typedef struct {
    uint32_t brightness;
    uint32_t battery;
    char     profile[16];
    uint64_t tick;
} power_state_t;

static void show(const char* what, bifrost_error_t e, int32_t st) {
    printf("  → %-28s transport=%-10s status=%s\n", what, bifrost_error_string(e),
           e == BIFROST_OK ? bifrost_error_string((bifrost_error_t)st) : "-");
}

int main(void) {
    char name[32];
    snprintf(name, sizeof name, "cmd_client_%d", (int)getpid());
    bifrost_reader_t* r = bifrost_join_as_reader(POOL_NAME, name);
    if (!r) { fprintf(stderr, "join: %s\n", bifrost_error_string(bifrost_get_last_error())); return 1; }
    
    /* 1. Discover writers and their command catalogs */
    bifrost_writer_info_t writers[16];
    uint32_t nw = bifrost_reader_list_writers(r, writers, 16);
    printf("📋 %u writer(s) in pool '%s'\n", nw, POOL_NAME);
    for (uint32_t i = 0; i < nw; i++) {
        bifrost_writer_ref_t ref = { writers[i].id, writers[i].generation };
        bifrost_cmd_info_t cmds[32];
        uint32_t nc = bifrost_reader_list_commands(r, ref, cmds, 32);
        printf("  [%u] %s - %u command(s)\n", writers[i].id, writers[i].name, nc);
        for (uint32_t j = 0; j < nc; j++) {
            printf("       #%-3u %-16s args %u..%u  %s\n", cmds[j].id, cmds[j].name,
                   cmds[j].arg_min, cmds[j].arg_max, cmds[j].desc);
        }
    }
    
    /* 2. Invoke commands by name */
    bifrost_writer_ref_t pd = bifrost_reader_resolve_writer(r, WRITER_NAME);
    if (!pd.id) { fprintf(stderr, "%s not running\n", WRITER_NAME); bifrost_reader_leave(r); return 1; }
    
    uint32_t set_b = bifrost_reader_find_command(r, pd, "set_brightness", NULL);
    uint32_t get_s = bifrost_reader_find_command(r, pd, "get_state", NULL);
    uint32_t set_p = bifrost_reader_find_command(r, pd, "set_profile", NULL);
    
    printf("\n⚡ Invoking commands on %s\n", WRITER_NAME);
    int32_t st;
    uint8_t level = 75;
    bifrost_error_t e = bifrost_reader_call(r, pd, set_b, &level, 1, NULL, NULL, &st, TIMEOUT_NS);
    show("set_brightness(75)", e, st);
    
    e = bifrost_reader_call(r, pd, set_p, "eco", 3, NULL, NULL, &st, TIMEOUT_NS);
    show("set_profile(eco)", e, st);
    
    char err[64] = {0};
    uint32_t elen = sizeof err - 1;
    e = bifrost_reader_call(r, pd, set_p, "turbo", 5, err, &elen, &st, TIMEOUT_NS);
    show("set_profile(turbo)", e, st);
    if (e == BIFROST_OK && elen) printf("       writer said: \"%.*s\"\n", (int)elen, err);
    
    power_state_t s;
    uint32_t slen = sizeof s;
    e = bifrost_reader_call(r, pd, get_s, NULL, 0, &s, &slen, &st, TIMEOUT_NS);
    show("get_state()", e, st);
    if (e == BIFROST_OK && st == BIFROST_OK)
        printf("       brightness=%u battery=%u%% profile=%s tick=%llu\n",
               s.brightness, s.battery, s.profile, (unsigned long long)s.tick);
    
    /* 3. Pipelined (asynchronous) usage: fire several, collect replies later */
    uint64_t tickets[4];
    for (int i = 0; i < 4; i++) {
        uint8_t lv = (uint8_t)(10 + i * 20);
        bifrost_reader_send_command(r, pd, set_b, &lv, 1, 0, &tickets[i]);
    }
    int got = 0;
    while (got < 4 && bifrost_reader_wait_reply(r, TIMEOUT_NS) == BIFROST_OK) {
        const bifrost_cmd_reply_t* rep;
        while ((rep = bifrost_reader_poll_reply(r))) {
            printf("  ↩ async reply ticket=%llx status=%s\n",
                   (unsigned long long)rep->ticket, bifrost_error_string((bifrost_error_t)rep->status));
            bifrost_reader_reply_done(r, rep);
            got++;
        }
    }
    
    /* 4. The data path is unaffected: read the latest published state */
    usleep(50000);
    size_t sz;
    power_state_t* live = bifrost_reader_get(r, WRITER_NAME, &sz);
    if (live) printf("\n📡 data path: brightness=%u profile=%s tick=%llu\n",
                     live->brightness, live->profile, (unsigned long long)live->tick);
    
    bifrost_reader_leave(r);
    return 0;
}
