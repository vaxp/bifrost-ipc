/**
 * Bifrost - Command Layer & Foundation Fix Tests
 */

#define _GNU_SOURCE
#include "bifrost.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/mman.h>

#define POOL "cmd_test_pool"

static int tests_run = 0, tests_failed = 0;
static const char* current = "";

#define TEST(name) do { current = name; tests_run++; printf("  [TEST] %-52s", name); fflush(stdout); } while (0)
#define CHECK(cond) do { if (!(cond)) { printf("\033[31mFAIL\033[0m  (%s:%d: %s) err=%s\n", \
        __FILE__, __LINE__, #cond, bifrost_error_string(bifrost_get_last_error())); tests_failed++; goto out; } } while (0)
#define PASS() printf("\033[32mPASS\033[0m\n")

static uint64_t now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void cleanup_pool(void) {
    /* bifrost_destroy_pool() only unlinks for the creator; remove leftovers directly */
    shm_unlink("/bifrost_" POOL);
}

static bifrost_pool_t* make_pool(void (*tweak)(bifrost_pool_config_t*)) {
    cleanup_pool();
    bifrost_pool_config_t c; bifrost_config_init(&c);
    c.max_participants = 32;
    if (tweak) tweak(&c);
    return bifrost_create_pool(POOL, &c);
}

static bifrost_cmd_desc_t desc(uint32_t id, const char* name, uint32_t amin, uint32_t amax) {
    bifrost_cmd_desc_t d; memset(&d, 0, sizeof(d));
    d.id = id; d.arg_min = amin; d.arg_max = amax; d.reply_max = 64;
    snprintf(d.name, sizeof(d.name), "%s", name);
    snprintf(d.desc, sizeof(d.desc), "test command %s", name);
    return d;
}

/* ============================================================================
 * Foundation fixes
 * ============================================================================ */

static void test_ids_and_get_by_id(void) {
    TEST("Unified IDs + get_by_id returns the right writer");
    bifrost_pool_t* pool = make_pool(NULL);
    bifrost_writer_t* a = bifrost_join_as_writer(POOL, "alpha");
    bifrost_writer_t* b = bifrost_join_as_writer(POOL, "beta");
    bifrost_reader_t* r = bifrost_join_as_reader(POOL, "r");
    CHECK(pool && a && b && r);
    
    *(uint32_t*)bifrost_writer_alloc(a, 4) = 0xAAAA; bifrost_writer_set_command(a, 11); bifrost_writer_publish(a);
    *(uint32_t*)bifrost_writer_alloc(b, 4) = 0xBBBB; bifrost_writer_set_command(b, 22); bifrost_writer_publish(b);
    
    CHECK(bifrost_reader_find_writer(r, "alpha") == bifrost_writer_get_id(a));
    CHECK(bifrost_reader_find_writer(r, "beta") == bifrost_writer_get_id(b));
    
    bifrost_writer_info_t info[4];
    uint32_t n = bifrost_reader_list_writers(r, info, 4);
    CHECK(n == 2);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t expect = strcmp(info[i].name, "alpha") == 0 ? bifrost_writer_get_id(a) : bifrost_writer_get_id(b);
        CHECK(info[i].id == expect);
        CHECK(info[i].generation != 0);
    }
    
    size_t sz;
    uint32_t* pb = bifrost_reader_get_by_id(r, bifrost_writer_get_id(b), &sz);
    uint32_t* pa = bifrost_reader_get_by_id(r, bifrost_writer_get_id(a), &sz);
    CHECK(pa && *pa == 0xAAAA);
    CHECK(pb && *pb == 0xBBBB);
    CHECK(bifrost_reader_get_command_by_id(r, bifrost_writer_get_id(a)) == 11);
    CHECK(bifrost_reader_get_command_by_id(r, bifrost_writer_get_id(b)) == 22);
    CHECK(bifrost_reader_get_by_id(r, 999, &sz) == NULL);
    PASS();
out:
    if (r) bifrost_reader_leave(r);
    if (a) bifrost_writer_leave(a);
    if (b) bifrost_writer_leave(b);
    if (pool) bifrost_destroy_pool(pool);
}

static atomic_int g_join_ok, g_join_go, g_join_done;
static void* join_racer(void* arg) {
    (void)arg;
    while (!atomic_load(&g_join_go)) {}
    bifrost_writer_t* w = bifrost_join_as_writer(POOL, "same_name");
    
    atomic_fetch_add(&g_join_done, 1);
    
    if (w) {
        atomic_fetch_add(&g_join_ok, 1);
        while (atomic_load(&g_join_done) < 8) {
            usleep(100);
        }
        bifrost_writer_leave(w);
    }
    return NULL;
}

static void test_join_race(void) {
    TEST("Concurrent joins with the same name -> exactly one");
    bifrost_pool_t* pool = make_pool(NULL);
    CHECK(pool);
    for (int round = 0; round < 20; round++) {
        atomic_store(&g_join_ok, 0); atomic_store(&g_join_go, 0); atomic_store(&g_join_done, 0);
        pthread_t t[8];
        for (int i = 0; i < 8; i++) pthread_create(&t[i], NULL, join_racer, NULL);
        atomic_store(&g_join_go, 1);
        for (int i = 0; i < 8; i++) pthread_join(t[i], NULL);
        CHECK(atomic_load(&g_join_ok) == 1);
    }
    PASS();
out:
    if (pool) bifrost_destroy_pool(pool);
}

/* ============================================================================
 * Catalog
 * ============================================================================ */

static void test_catalog(void) {
    TEST("Catalog register / list / find / unregister");
    bifrost_pool_t* pool = make_pool(NULL);
    bifrost_writer_t* w = bifrost_join_as_writer(POOL, "svc");
    bifrost_reader_t* r = bifrost_join_as_reader(POOL, "cli");
    CHECK(pool && w && r);
    
    bifrost_cmd_desc_t d1 = desc(1, "set_brightness", 1, 1);
    bifrost_cmd_desc_t d2 = desc(2, "get_status", 0, 0);
    bifrost_cmd_desc_t d3 = desc(3, "reboot", 0, 0);
    CHECK(bifrost_writer_register_command(w, &d1) == BIFROST_OK);
    CHECK(bifrost_writer_register_command(w, &d2) == BIFROST_OK);
    CHECK(bifrost_writer_register_command(w, &d3) == BIFROST_OK);
    
    bifrost_cmd_desc_t dup = desc(1, "other", 0, 0);
    CHECK(bifrost_writer_register_command(w, &dup) == BIFROST_ERR_EXISTS);
    bifrost_cmd_desc_t dupn = desc(9, "reboot", 0, 0);
    CHECK(bifrost_writer_register_command(w, &dupn) == BIFROST_ERR_EXISTS);
    bifrost_cmd_desc_t big = desc(10, "too_big", 0, 100000);
    CHECK(bifrost_writer_register_command(w, &big) == BIFROST_ERR_INVALID);
    bifrost_cmd_desc_t zero = desc(0, "zero", 0, 0);
    CHECK(bifrost_writer_register_command(w, &zero) == BIFROST_ERR_INVALID);
    
    bifrost_writer_ref_t ref = bifrost_reader_resolve_writer(r, "svc");
    CHECK(ref.id == bifrost_writer_get_id(w) && ref.gen != 0);
    
    bifrost_cmd_info_t list[8];
    CHECK(bifrost_reader_list_commands(r, ref, list, 8) == 3);
    CHECK(strcmp(list[0].name, "set_brightness") == 0);
    CHECK(strcmp(list[0].desc, "test command set_brightness") == 0);
    
    bifrost_cmd_info_t info;
    CHECK(bifrost_reader_find_command(r, ref, "get_status", &info) == 2);
    CHECK(info.arg_max == 0);
    CHECK(bifrost_reader_find_command(r, ref, "nope", NULL) == 0);
    
    CHECK(bifrost_writer_unregister_command(w, 1) == BIFROST_OK);
    CHECK(bifrost_reader_list_commands(r, ref, list, 8) == 2);
    CHECK(bifrost_reader_find_command(r, ref, "set_brightness", NULL) == 0);
    CHECK(bifrost_reader_find_command(r, ref, "reboot", NULL) == 3);
    PASS();
out:
    if (r) bifrost_reader_leave(r);
    if (w) bifrost_writer_leave(w);
    if (pool) bifrost_destroy_pool(pool);
}

/* ============================================================================
 * Request / reply
 * ============================================================================ */

static void test_roundtrip_and_validation(void) {
    TEST("Send / poll / done / reply + auto validation");
    bifrost_pool_t* pool = make_pool(NULL);
    bifrost_writer_t* w = bifrost_join_as_writer(POOL, "svc");
    bifrost_reader_t* r = bifrost_join_as_reader(POOL, "cli");
    CHECK(pool && w && r);
    bifrost_cmd_desc_t d = desc(7, "echo", 1, 64);
    CHECK(bifrost_writer_register_command(w, &d) == BIFROST_OK);
    bifrost_writer_ref_t ref = bifrost_reader_resolve_writer(r, "svc");
    
    CHECK(bifrost_writer_poll_command(w) == NULL);   /* empty */
    
    uint64_t t_ok, t_unknown, t_badlen;
    CHECK(bifrost_reader_send_command(r, ref, 7, "hello", 5, 0, &t_ok) == BIFROST_OK);
    CHECK(bifrost_reader_send_command(r, ref, 99, NULL, 0, 0, &t_unknown) == BIFROST_OK);
    CHECK(bifrost_reader_send_command(r, ref, 7, NULL, 0, 0, &t_badlen) == BIFROST_OK);
    CHECK(bifrost_reader_send_command(r, ref, 7, "x", 1, BIFROST_SEND_F_NO_REPLY, NULL) == BIFROST_OK);
    
    const bifrost_cmd_request_t* q = bifrost_writer_poll_command(w);
    CHECK(q && q->cmd_id == 7 && q->arg_len == 5 && memcmp(q->args, "hello", 5) == 0);
    CHECK(q->ticket == t_ok && q->sender_pid == getpid());
    char sname[64];
    CHECK(bifrost_writer_get_sender_name(w, q, sname, sizeof sname) == BIFROST_OK && strcmp(sname, "cli") == 0);
    CHECK(bifrost_writer_poll_command(w) == NULL && bifrost_get_last_error() == BIFROST_ERR_BUSY);
    CHECK(bifrost_writer_command_done(w, q, 42, q->args, q->arg_len) == BIFROST_OK);   /* echo from cell */
    
    /* Unknown + bad length were auto-answered; next valid one is the no-reply */
    q = bifrost_writer_poll_command(w);
    CHECK(q && q->flags & BIFROST_SEND_F_NO_REPLY);
    CHECK(bifrost_writer_command_done(w, q, 0, NULL, 0) == BIFROST_OK);
    CHECK(bifrost_writer_poll_command(w) == NULL);
    
    const bifrost_cmd_reply_t* rep = bifrost_reader_poll_reply(r);
    CHECK(rep && rep->ticket == t_ok && rep->status == 42 && rep->len == 5 &&
          memcmp(rep->data, "hello", 5) == 0 && rep->writer_id == ref.id);
    bifrost_reader_reply_done(r, rep);
    rep = bifrost_reader_poll_reply(r);
    CHECK(rep && rep->ticket == t_unknown && rep->status == BIFROST_ERR_NOTFOUND);
    bifrost_reader_reply_done(r, rep);
    rep = bifrost_reader_poll_reply(r);
    CHECK(rep && rep->ticket == t_badlen && rep->status == BIFROST_ERR_INVALID);
    bifrost_reader_reply_done(r, rep);
    CHECK(bifrost_reader_poll_reply(r) == NULL);     /* no reply for fire-and-forget */
    
    /* Reader-side validation */
    CHECK(bifrost_reader_send_command(r, ref, 7, NULL, 100000, 0, NULL) == BIFROST_ERR_INVALID);
    bifrost_writer_ref_t bogus = { ref.id, ref.gen + 1 };
    CHECK(bifrost_reader_send_command(r, bogus, 7, "a", 1, 0, NULL) == BIFROST_ERR_NOTFOUND);
    PASS();
out:
    if (r) bifrost_reader_leave(r);
    if (w) bifrost_writer_leave(w);
    if (pool) bifrost_destroy_pool(pool);
}

static void small_inbox(bifrost_pool_config_t* c) { c->cmd_inbox_depth = 4; c->cmd_reply_depth = 4; }

static void test_inbox_full(void) {
    TEST("Backpressure: inbox/credits full -> BIFROST_ERR_FULL");
    bifrost_reader_t* r2 = NULL;
    bifrost_pool_t* pool = make_pool(small_inbox);
    bifrost_writer_t* w = bifrost_join_as_writer(POOL, "svc");
    bifrost_reader_t* r = bifrost_join_as_reader(POOL, "cli");
    CHECK(pool && w && r);
    bifrost_cmd_desc_t d = desc(1, "noop", 0, 0);
    bifrost_writer_register_command(w, &d);
    bifrost_writer_ref_t ref = bifrost_reader_resolve_writer(r, "svc");
    for (int i = 0; i < 4; i++) CHECK(bifrost_reader_send_command(r, ref, 1, NULL, 0, 0, NULL) == BIFROST_OK);
    CHECK(bifrost_reader_send_command(r, ref, 1, NULL, 0, 0, NULL) == BIFROST_ERR_FULL);
    
    const bifrost_cmd_request_t* q;
    int handled = 0;
    while ((q = bifrost_writer_poll_command(w))) { bifrost_writer_command_done(w, q, 0, NULL, 0); handled++; }
    CHECK(handled == 4);
    CHECK(bifrost_writer_replies_dropped(w) == 0);
    
    /* Inbox is empty, but 4 replies are still owed to us: no more credits */
    CHECK(bifrost_reader_send_command(r, ref, 1, NULL, 0, 0, NULL) == BIFROST_ERR_FULL);
    /* Fire-and-forget needs no credit */
    CHECK(bifrost_reader_send_command(r, ref, 1, NULL, 0, BIFROST_SEND_F_NO_REPLY, NULL) == BIFROST_OK);
    const bifrost_cmd_reply_t* rep = bifrost_reader_poll_reply(r);
    CHECK(rep); bifrost_reader_reply_done(r, rep);
    CHECK(bifrost_reader_send_command(r, ref, 1, NULL, 0, 0, NULL) == BIFROST_OK);  /* credit back */
    
    /* Only remaining drop path: the reader left before the reply */
    r2 = bifrost_join_as_reader(POOL, "ghost");
    CHECK(r2);
    CHECK(bifrost_reader_send_command(r2, bifrost_reader_resolve_writer(r2, "svc"), 1, NULL, 0, 0, NULL) == BIFROST_OK);
    bifrost_reader_leave(r2); r2 = NULL;
    while ((q = bifrost_writer_poll_command(w))) bifrost_writer_command_done(w, q, 0, NULL, 0);
    CHECK(bifrost_writer_replies_dropped(w) == 1);
    PASS();
out:
    if (r2) bifrost_reader_leave(r2);
    if (r) bifrost_reader_leave(r);
    if (w) bifrost_writer_leave(w);
    if (pool) bifrost_destroy_pool(pool);
}

/* --- Threaded service using dispatch + futex wait --- */

static atomic_int g_stop;

static int32_t h_add(const bifrost_cmd_request_t* req, void* reply, uint32_t* len, void* ctx) {
    (void)ctx;
    uint64_t a, b; memcpy(&a, req->args, 8); memcpy(&b, req->args + 8, 8);
    uint64_t s = a + b; memcpy(reply, &s, 8); *len = 8;
    return BIFROST_OK;
}

static int32_t deny_cli2(const bifrost_cmd_request_t* req, const bifrost_cmd_desc_t* d, void* ctx) {
    bifrost_writer_t* w = ctx; (void)d;
    char name[64];
    bifrost_writer_get_sender_name(w, req, name, sizeof name);
    return strcmp(name, "cli2") == 0 ? BIFROST_ERR_PERMISSION : BIFROST_OK;
}

static void* service_thread(void* arg) {
    bifrost_writer_t* w = arg;
    while (!atomic_load(&g_stop)) {
        if (bifrost_writer_wait_command(w, 50 * 1000 * 1000) == BIFROST_OK) bifrost_writer_dispatch(w, 64);
    }
    bifrost_writer_dispatch(w, UINT32_MAX);
    return NULL;
}

static void test_call_dispatch_filter(void) {
    TEST("bifrost_reader_call + dispatch + futex wake + filter");
    pthread_t th; bool started = false;
    bifrost_pool_t* pool = make_pool(NULL);
    bifrost_writer_t* w = bifrost_join_as_writer(POOL, "calc");
    bifrost_reader_t* r = bifrost_join_as_reader(POOL, "cli");
    bifrost_reader_t* r2 = bifrost_join_as_reader(POOL, "cli2");
    CHECK(pool && w && r && r2);
    bifrost_cmd_desc_t d = desc(1, "add", 16, 16);
    bifrost_cmd_desc_t d2 = desc(2, "unhandled", 0, 0);
    CHECK(bifrost_writer_register_command(w, &d) == BIFROST_OK);
    CHECK(bifrost_writer_register_command(w, &d2) == BIFROST_OK);
    CHECK(bifrost_writer_on_command(w, 1, h_add, NULL) == BIFROST_OK);
    bifrost_writer_set_command_filter(w, deny_cli2, w);
    
    atomic_store(&g_stop, 0);
    pthread_create(&th, NULL, service_thread, w); started = true;
    usleep(100000);  /* let the service fall asleep on the futex */
    
    bifrost_writer_ref_t ref = bifrost_reader_resolve_writer(r, "calc");
    uint64_t args[2] = { 40, 2 }, sum = 0;
    uint32_t len = sizeof sum; int32_t st = -1;
    uint64_t t0 = now_ns();
    CHECK(bifrost_reader_call(r, ref, 1, args, 16, &sum, &len, &st, 1000000000ull) == BIFROST_OK);
    uint64_t wake_us = (now_ns() - t0) / 1000;
    CHECK(st == BIFROST_OK && len == 8 && sum == 42);
    CHECK(wake_us < 100000);   /* woken by futex, not by the 50ms poll timeout */
    
    for (uint64_t i = 0; i < 1000; i++) {
        args[0] = i; args[1] = i * 3; len = 8;
        CHECK(bifrost_reader_call(r, ref, 1, args, 16, &sum, &len, &st, 1000000000ull) == BIFROST_OK);
        CHECK(st == BIFROST_OK && sum == i * 4);
    }
    
    len = 8;
    CHECK(bifrost_reader_call(r, ref, 2, NULL, 0, NULL, NULL, &st, 1000000000ull) == BIFROST_OK);
    CHECK(st == BIFROST_ERR_NOTFOUND);   /* no handler bound */
    
    bifrost_writer_ref_t ref2 = bifrost_reader_resolve_writer(r2, "calc");
    CHECK(bifrost_reader_call(r2, ref2, 1, args, 16, &sum, &len, &st, 1000000000ull) == BIFROST_OK);
    CHECK(st == BIFROST_ERR_PERMISSION);
    PASS();
out:
    atomic_store(&g_stop, 1);
    if (started) pthread_join(th, NULL);
    if (r) bifrost_reader_leave(r);
    if (r2) bifrost_reader_leave(r2);
    if (w) bifrost_writer_leave(w);
    if (pool) bifrost_destroy_pool(pool);
}

static void test_aba_and_leave(void) {
    TEST("ABA guard + leave answers queued requests");
    bifrost_pool_t* pool = make_pool(NULL);
    bifrost_writer_t* w = bifrost_join_as_writer(POOL, "svc");
    bifrost_reader_t* r = bifrost_join_as_reader(POOL, "cli");
    CHECK(pool && w && r);
    bifrost_cmd_desc_t d = desc(1, "noop", 0, 0);
    bifrost_writer_register_command(w, &d);
    bifrost_writer_ref_t old = bifrost_reader_resolve_writer(r, "svc");
    
    uint64_t t1, t2;
    CHECK(bifrost_reader_send_command(r, old, 1, NULL, 0, 0, &t1) == BIFROST_OK);
    CHECK(bifrost_reader_send_command(r, old, 1, NULL, 0, 0, &t2) == BIFROST_OK);
    bifrost_writer_leave(w); w = NULL;
    
    const bifrost_cmd_reply_t* rep = bifrost_reader_poll_reply(r);
    CHECK(rep && rep->ticket == t1 && rep->status == BIFROST_ERR_NOTFOUND); bifrost_reader_reply_done(r, rep);
    rep = bifrost_reader_poll_reply(r);
    CHECK(rep && rep->ticket == t2 && rep->status == BIFROST_ERR_NOTFOUND); bifrost_reader_reply_done(r, rep);
    
    /* Same name re-joins into the same slot with a new generation */
    w = bifrost_join_as_writer(POOL, "svc");
    CHECK(w);
    bifrost_writer_ref_t now = bifrost_reader_resolve_writer(r, "svc");
    CHECK(now.id == old.id && now.gen != old.gen);
    CHECK(bifrost_reader_send_command(r, old, 1, NULL, 0, 0, NULL) == BIFROST_ERR_NOTFOUND);
    bifrost_cmd_info_t list[4];
    CHECK(bifrost_reader_list_commands(r, now, list, 4) == 0);   /* catalog not inherited */
    CHECK(bifrost_reader_list_commands(r, old, list, 4) == 0 && bifrost_get_last_error() == BIFROST_ERR_NOTFOUND);
    PASS();
out:
    if (r) bifrost_reader_leave(r);
    if (w) bifrost_writer_leave(w);
    if (pool) bifrost_destroy_pool(pool);
}

static void big_payload(bifrost_pool_config_t* c) { c->cmd_max_args = 8192; c->cmd_max_reply = 8192; c->cmd_inbox_depth = 8; }

static void test_custom_sizes(void) {
    TEST("Per-pool sizes: 8 KB inline args and replies");
    bifrost_pool_t* pool = make_pool(big_payload);
    bifrost_writer_t* w = bifrost_join_as_writer(POOL, "svc");
    bifrost_reader_t* r = bifrost_join_as_reader(POOL, "cli");
    uint8_t* buf = malloc(8192);
    CHECK(pool && w && r && buf);
    for (int i = 0; i < 8192; i++) buf[i] = (uint8_t)(i * 7);
    bifrost_cmd_desc_t d = desc(1, "blob", 0, 8192);
    CHECK(bifrost_writer_register_command(w, &d) == BIFROST_OK);
    bifrost_writer_ref_t ref = bifrost_reader_resolve_writer(r, "svc");
    CHECK(bifrost_reader_send_command(r, ref, 1, buf, 8192, 0, NULL) == BIFROST_OK);
    const bifrost_cmd_request_t* q = bifrost_writer_poll_command(w);
    CHECK(q && q->arg_len == 8192 && memcmp(q->args, buf, 8192) == 0);
    bifrost_writer_command_done(w, q, 0, q->args, q->arg_len);
    const bifrost_cmd_reply_t* rep = bifrost_reader_poll_reply(r);
    CHECK(rep && rep->len == 8192 && memcmp(rep->data, buf, 8192) == 0);
    bifrost_reader_reply_done(r, rep);
    PASS();
out:
    free(buf);
    if (r) bifrost_reader_leave(r);
    if (w) bifrost_writer_leave(w);
    if (pool) bifrost_destroy_pool(pool);
}

static void disable_cmds(bifrost_pool_config_t* c) { c->cmd_inbox_depth = 0; }

static void test_disabled(void) {
    TEST("Commands disabled (depth 0): data path still works");
    bifrost_pool_t* pool = make_pool(disable_cmds);
    bifrost_writer_t* w = bifrost_join_as_writer(POOL, "svc");
    bifrost_reader_t* r = bifrost_join_as_reader(POOL, "cli");
    CHECK(pool && w && r);
    bifrost_cmd_desc_t d = desc(1, "x", 0, 0);
    CHECK(bifrost_writer_register_command(w, &d) == BIFROST_ERR_DISABLED);
    CHECK(bifrost_reader_send_command(r, bifrost_reader_resolve_writer(r, "svc"), 1, NULL, 0, 0, NULL) == BIFROST_ERR_DISABLED);
    *(int*)bifrost_writer_alloc(w, sizeof(int)) = 5; bifrost_writer_publish(w);
    size_t sz; int* v = bifrost_reader_get(r, "svc", &sz);
    CHECK(v && *v == 5);
    PASS();
out:
    if (r) bifrost_reader_leave(r);
    if (w) bifrost_writer_leave(w);
    if (pool) bifrost_destroy_pool(pool);
}

static void test_writer_crash(void) {
    TEST("Writer killed (-9) -> call fails fast with NOTFOUND");
    bifrost_pool_t* pool = make_pool(NULL);
    CHECK(pool);
    int pfd[2]; CHECK(pipe(pfd) == 0);
    pid_t pid = fork();
    if (pid == 0) {
        bifrost_writer_t* w = bifrost_join_as_writer(POOL, "doomed");
        bifrost_cmd_desc_t d = desc(1, "hang", 0, 0);
        bifrost_writer_register_command(w, &d);
        char c = 1; if (write(pfd[1], &c, 1) != 1) _exit(1);
        for (;;) pause();   /* never serves */
    }
    char c; CHECK(read(pfd[0], &c, 1) == 1);
    bifrost_reader_t* r = bifrost_join_as_reader(POOL, "cli");
    CHECK(r);
    bifrost_writer_ref_t ref = bifrost_reader_resolve_writer(r, "doomed");
    CHECK(ref.id != 0 && bifrost_reader_writer_alive(r, ref));
    kill(pid, SIGKILL); waitpid(pid, NULL, 0);
    CHECK(!bifrost_reader_writer_alive(r, ref));
    int32_t st;
    uint64_t t0 = now_ns();
    bifrost_error_t e = bifrost_reader_call(r, ref, 1, NULL, 0, NULL, NULL, &st, 5000000000ull);
    CHECK(e == BIFROST_ERR_NOTFOUND);
    CHECK(now_ns() - t0 < 1000000000ull);
    bifrost_reader_leave(r);
    PASS();
out:
    close(pfd[0]); close(pfd[1]);
    if (pool) bifrost_destroy_pool(pool);
}

/* ============================================================================
 * Multi-process stress: N reader processes x M calls against one writer
 * ============================================================================ */

#define STRESS_PROCS 4
#define STRESS_CALLS 50000

static int stress_child(int k) {
    char name[32]; snprintf(name, sizeof name, "stress_%d", k);
    bifrost_reader_t* r = NULL;
    for (int i = 0; i < 200 && !r; i++) { r = bifrost_join_as_reader(POOL, name); if (!r) usleep(5000); }
    if (!r) return 2;
    bifrost_writer_ref_t ref = {0, 0};
    for (int i = 0; i < 200 && !ref.id; i++) { ref = bifrost_reader_resolve_writer(r, "calc"); if (!ref.id) usleep(5000); }
    if (!ref.id) return 3;
    
    /* Half synchronous calls, half pipelined sends */
    for (uint64_t i = 0; i < STRESS_CALLS / 2; i++) {
        uint64_t args[2] = { i, (uint64_t)k }, sum; uint32_t len = 8; int32_t st;
        if (bifrost_reader_call(r, ref, 1, args, 16, &sum, &len, &st, 5000000000ull) != BIFROST_OK ||
            st != BIFROST_OK || sum != i + (uint64_t)k) return 4;
    }
    uint64_t sent = 0, got = 0, expect_sum = 0, sum_seen = 0;
    while (got < STRESS_CALLS / 2) {
        if (sent < STRESS_CALLS / 2) {
            uint64_t args[2] = { sent, 1 };
            if (bifrost_reader_send_command(r, ref, 1, args, 16, 0, NULL) == BIFROST_OK) { expect_sum += sent + 1; sent++; }
        }
        const bifrost_cmd_reply_t* rep;
        while ((rep = bifrost_reader_poll_reply(r))) {
            uint64_t s; memcpy(&s, rep->data, 8); sum_seen += s; got++;
            bifrost_reader_reply_done(r, rep);
        }
        if (sent == STRESS_CALLS / 2 && got < sent) bifrost_reader_wait_reply(r, 1000000000ull);
    }
    bifrost_reader_leave(r);
    return expect_sum == sum_seen ? 0 : 5;
}

static void test_multiprocess_stress(void) {
    TEST("Multi-process stress (4 procs x 50k, call + pipeline)");
    bifrost_pool_config_t* unused = NULL; (void)unused;
    bifrost_pool_t* pool = make_pool(NULL);
    bifrost_writer_t* w = bifrost_join_as_writer(POOL, "calc");
    CHECK(pool && w);
    bifrost_cmd_desc_t d = desc(1, "add", 16, 16);
    bifrost_writer_register_command(w, &d);
    bifrost_writer_on_command(w, 1, h_add, NULL);
    
    pid_t kids[STRESS_PROCS];
    for (int k = 0; k < STRESS_PROCS; k++) {
        kids[k] = fork();
        if (kids[k] == 0) _exit(stress_child(k));
    }
    
    uint64_t t0 = now_ns();
    int done = 0, bad = 0;
    while (done < STRESS_PROCS) {
        uint32_t count = 0;
        while (bifrost_writer_dispatch(w, 1024) > 0) count++;
        if (count == 0) bifrost_writer_wait_command(w, 1000000ull);
        int status;
        pid_t p;
        while ((p = waitpid(-1, &status, WNOHANG)) > 0) {
            done++;
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                bad++;
                printf("\n      child exit=%d ", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            }
        }
    }
    double secs = (now_ns() - t0) / 1e9;
    CHECK(bad == 0);
    CHECK(bifrost_writer_replies_dropped(w) == 0);
    printf("\033[32mPASS\033[0m  (%.0f k cmd/s)\n", STRESS_PROCS * STRESS_CALLS / secs / 1000.0);
out:
    if (w) bifrost_writer_leave(w);
    if (pool) bifrost_destroy_pool(pool);
}

int main(void) {
    printf("\nBifrost Command Layer Tests (lib %s)\n", bifrost_version());
    printf("===========================================\n");
    
    test_ids_and_get_by_id();
    test_join_race();
    test_catalog();
    test_roundtrip_and_validation();
    test_inbox_full();
    test_call_dispatch_filter();
    test_aba_and_leave();
    test_custom_sizes();
    test_disabled();
    test_writer_crash();
    test_multiprocess_stress();
    
    cleanup_pool();
    printf("-------------------------------------------\n");
    printf("Tests: %d  Passed: \033[32m%d\033[0m  Failed: \033[31m%d\033[0m\n",
           tests_run, tests_run - tests_failed, tests_failed);
    return tests_failed ? 1 : 0;
}
