/**
 * Bifrost - Command Layer Benchmark
 *
 *  1. poll on an empty inbox            (cost added to a writer's loop)
 *  2. same-thread send->poll->done->reply (pure library overhead)
 *  3. cross-thread bifrost_reader_call RTT   (spinning service, p50/p99)
 *  4. cross-thread pipelined throughput (1 writer, N reader threads)
 */

#define _GNU_SOURCE
#include "bifrost.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

#define POOL "cmd_bench_pool"

static inline uint64_t now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void pin(int cpu) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n <= 0) return;
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu % n, &s);
    pthread_setaffinity_np(pthread_self(), sizeof s, &s);
}

static int cmp_u64(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y;
}

static int32_t h_echo(const bifrost_cmd_request_t* req, void* reply, uint32_t* len, void* ctx) {
    (void)ctx;
    memcpy(reply, req->args, req->arg_len);
    *len = req->arg_len;
    return BIFROST_OK;
}

static atomic_int g_stop;

static void* spin_service(void* arg) {
    bifrost_writer_t* w = arg;
    pin(1);
    while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
        if (!bifrost_writer_dispatch(w, 64)) __builtin_ia32_pause();
    }
    return NULL;
}

typedef struct { int idx; uint64_t ops; double secs; } pipe_args_t;
#define PIPE_OPS 2000000ull

static void* pipe_client(void* arg) {
    pipe_args_t* a = arg;
    pin(2 + a->idx);
    char name[32]; snprintf(name, sizeof name, "pc%d", a->idx);
    bifrost_reader_t* r = bifrost_join_as_reader(POOL, name);
    bifrost_writer_ref_t ref = bifrost_reader_resolve_writer(r, "svc");
    uint64_t v = 0, sent = 0, got = 0;
    uint64_t t0 = now_ns();
    while (got < PIPE_OPS) {
        while (sent < PIPE_OPS && bifrost_reader_send_command(r, ref, 1, &v, 8, 0, NULL) == BIFROST_OK) sent++;
        const bifrost_cmd_reply_t* rep;
        while ((rep = bifrost_reader_poll_reply(r))) { got++; bifrost_reader_reply_done(r, rep); }
    }
    a->secs = (now_ns() - t0) / 1e9;
    a->ops = got;
    bifrost_reader_leave(r);
    return NULL;
}

int main(void) {
    if (bifrost_pool_exists(POOL)) { bifrost_pool_t* o = bifrost_open_pool(POOL); if (o) bifrost_destroy_pool(o); }
    bifrost_pool_config_t cfg; bifrost_config_init(&cfg);
    cfg.cmd_inbox_depth = 1024;
    cfg.cmd_reply_depth = 256;
    cfg.cmd_max_args = 64;
    cfg.cmd_max_reply = 64;
    bifrost_pool_t* pool = bifrost_create_pool(POOL, &cfg);
    bifrost_writer_t* w = bifrost_join_as_writer(POOL, "svc");
    bifrost_reader_t* r = bifrost_join_as_reader(POOL, "cli");
    if (!pool || !w || !r) { fprintf(stderr, "setup failed: %s\n", bifrost_error_string(bifrost_get_last_error())); return 1; }
    
    bifrost_cmd_desc_t d; memset(&d, 0, sizeof d);
    d.id = 1; d.arg_max = 64; d.reply_max = 64; strcpy(d.name, "echo");
    bifrost_writer_register_command(w, &d);
    bifrost_writer_on_command(w, 1, h_echo, NULL);
    bifrost_writer_ref_t ref = bifrost_reader_resolve_writer(r, "svc");
    
    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║        Bifrost Command Layer Benchmark             ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");
    pin(0);
    
    /* 1. Empty poll */
    const int N1 = 50000000;
    uint64_t t0 = now_ns();
    for (int i = 0; i < N1; i++) {
        const bifrost_cmd_request_t* q = bifrost_writer_poll_command(w);
        __asm__ __volatile__("" :: "r"(q));
    }
    printf("  1. poll (empty inbox)            : %7.2f ns\n", (double)(now_ns() - t0) / N1);
    
    /* 2. Same-thread full cycle */
    const int N2 = 5000000;
    uint64_t v = 7;
    t0 = now_ns();
    for (int i = 0; i < N2; i++) {
        bifrost_reader_send_command(r, ref, 1, &v, 8, 0, NULL);
        const bifrost_cmd_request_t* q = bifrost_writer_poll_command(w);
        bifrost_writer_command_done(w, q, BIFROST_OK, q->args, 8);
        const bifrost_cmd_reply_t* rep = bifrost_reader_poll_reply(r);
        bifrost_reader_reply_done(r, rep);
    }
    printf("  2. send+poll+done+reply (1 thr)  : %7.2f ns\n", (double)(now_ns() - t0) / N2);
    
    /* 3. Cross-thread call RTT */
    pthread_t th;
    atomic_store(&g_stop, 0);
    pthread_create(&th, NULL, spin_service, w);
    usleep(10000);
    const int N3 = 1000000;
    uint64_t* lat = malloc(sizeof(uint64_t) * N3);
    for (int i = 0; i < 10000; i++) {   /* warm-up */
        uint64_t out; uint32_t len = 8; int32_t st;
        bifrost_reader_call(r, ref, 1, &v, 8, &out, &len, &st, UINT64_MAX);
    }
    t0 = now_ns();
    for (int i = 0; i < N3; i++) {
        uint64_t out; uint32_t len = 8; int32_t st;
        uint64_t a = now_ns();
        bifrost_reader_call(r, ref, 1, &v, 8, &out, &len, &st, UINT64_MAX);
        lat[i] = now_ns() - a;
    }
    double tot = (now_ns() - t0) / 1e9;
    qsort(lat, N3, sizeof *lat, cmp_u64);
    printf("  3. bifrost_reader_call RTT (2 thr)    : p50 %5.0f ns | p99 %6.0f ns | %.2f M call/s\n",
           (double)lat[N3 / 2], (double)lat[(size_t)(N3 * 0.99)], N3 / tot / 1e6);
    free(lat);
    bifrost_reader_leave(r);
    
    /* 4. Pipelined throughput */
    for (int nthr = 1; nthr <= 4; nthr *= 4) {
        pthread_t ct[4]; pipe_args_t pa[4];
        uint64_t s0 = now_ns();
        for (int i = 0; i < nthr; i++) { pa[i].idx = i; pthread_create(&ct[i], NULL, pipe_client, &pa[i]); }
        uint64_t ops = 0;
        for (int i = 0; i < nthr; i++) { pthread_join(ct[i], NULL); ops += pa[i].ops; }
        double secs = (now_ns() - s0) / 1e9;
        printf("  4. pipelined, %d reader thr       : %7.2f M cmd/s (with replies)\n", nthr, ops / secs / 1e6);
    }
    
    atomic_store(&g_stop, 1);
    pthread_join(th, NULL);
    printf("  replies dropped                  : %llu\n\n", (unsigned long long)bifrost_writer_replies_dropped(w));
    bifrost_writer_leave(w);
    bifrost_destroy_pool(pool);
    return 0;
}
