/**
 * Bifrost Basic Tests
 * 
 * Basic functionality tests for the library.
 */

#include <bifrost.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define TEST_POOL "test_pool"

/* Test counters */
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    printf("  [TEST] %s... ", name); \
    tests_run++;

#define PASS() \
    printf("\033[32mPASS\033[0m\n"); \
    tests_passed++;

#define FAIL(msg) \
    printf("\033[31mFAIL\033[0m: %s\n", msg); \
    tests_failed++;

/* ============================================================================
 * Test Cases
 * ============================================================================ */

void test_pool_create_destroy(void) {
    TEST("Pool create/destroy");
    
    /* Clean up any existing pool */
    if (bifrost_pool_exists(TEST_POOL)) {
        bifrost_pool_t* p = bifrost_open_pool(TEST_POOL);
        if (p) bifrost_destroy_pool(p);
    }
    
    bifrost_pool_t* pool = bifrost_create_pool(TEST_POOL, NULL);
    if (!pool) {
        FAIL("Failed to create pool");
        return;
    }
    
    if (!bifrost_pool_exists(TEST_POOL)) {
        bifrost_destroy_pool(pool);
        FAIL("Pool exists check failed");
        return;
    }
    
    bifrost_destroy_pool(pool);
    
    /* After destroy, pool should not exist */
    if (bifrost_pool_exists(TEST_POOL)) {
        FAIL("Pool still exists after destroy");
        return;
    }
    
    PASS();
}

void test_pool_stats(void) {
    TEST("Pool statistics");
    
    bifrost_pool_t* pool = bifrost_create_pool(TEST_POOL, NULL);
    if (!pool) {
        FAIL("Failed to create pool");
        return;
    }
    
    bifrost_pool_stats_t stats;
    bifrost_error_t err = bifrost_get_pool_stats(pool, &stats);
    if (err != BIFROST_OK) {
        bifrost_destroy_pool(pool);
        FAIL("Failed to get stats");
        return;
    }
    
    if (stats.total_size == 0) {
        bifrost_destroy_pool(pool);
        FAIL("Total size is 0");
        return;
    }
    
    if (stats.writer_count != 0 || stats.reader_count != 0) {
        bifrost_destroy_pool(pool);
        FAIL("Initial counts should be 0");
        return;
    }
    
    bifrost_destroy_pool(pool);
    PASS();
}

void test_writer_join_leave(void) {
    TEST("Writer join/leave");
    
    bifrost_pool_t* pool = bifrost_create_pool(TEST_POOL, NULL);
    if (!pool) {
        FAIL("Failed to create pool");
        return;
    }
    // Keep pool open while writer uses it
    
    bifrost_writer_t* writer = bifrost_join_as_writer(TEST_POOL, "test_writer");
    if (!writer) {
        bifrost_destroy_pool(pool);
        FAIL("Failed to join as writer");
        return;
    }
    
    uint32_t id = bifrost_writer_get_id(writer);
    if (id == 0) {
        bifrost_writer_leave(writer);
        bifrost_destroy_pool(pool);
        FAIL("Writer ID should not be 0");
        return;
    }
    
    /* Check stats */
    bifrost_pool_stats_t stats;
    bifrost_get_pool_stats(pool, &stats);
    if (stats.writer_count != 1) {
        bifrost_writer_leave(writer);
        bifrost_destroy_pool(pool);
        FAIL("Writer count should be 1");
        return;
    }
    
    bifrost_writer_leave(writer);
    
    /* Check stats after leave */
    bifrost_get_pool_stats(pool, &stats);
    if (stats.writer_count != 0) {
        bifrost_destroy_pool(pool);
        FAIL("Writer count should be 0 after leave");
        return;
    }
    
    bifrost_destroy_pool(pool);
    PASS();
}

void test_reader_join_leave(void) {
    TEST("Reader join/leave");
    
    bifrost_pool_t* pool = bifrost_create_pool(TEST_POOL, NULL);
    if (!pool) {
        FAIL("Failed to create pool");
        return;
    }
    
    bifrost_reader_t* reader = bifrost_join_as_reader(TEST_POOL, "test_reader");
    if (!reader) {
        bifrost_destroy_pool(pool);
        FAIL("Failed to join as reader");
        return;
    }
    
    bifrost_pool_stats_t stats;
    bifrost_get_pool_stats(pool, &stats);
    if (stats.reader_count != 1) {
        bifrost_reader_leave(reader);
        bifrost_destroy_pool(pool);
        FAIL("Reader count should be 1");
        return;
    }
    
    bifrost_reader_leave(reader);
    
    bifrost_get_pool_stats(pool, &stats);
    if (stats.reader_count != 0) {
        bifrost_destroy_pool(pool);
        FAIL("Reader count should be 0 after leave");
        return;
    }
    
    bifrost_destroy_pool(pool);
    PASS();
}

void test_write_read_data(void) {
    TEST("Write and read data");
    
    bifrost_pool_t* pool = bifrost_create_pool(TEST_POOL, NULL);
    if (!pool) {
        FAIL("Failed to create pool");
        return;
    }
    
    /* Join as writer */
    bifrost_writer_t* writer = bifrost_join_as_writer(TEST_POOL, "data_writer");
    if (!writer) {
        bifrost_destroy_pool(pool);
        FAIL("Failed to join as writer");
        return;
    }
    
    /* Allocate and write data directly */
    typedef struct {
        int value;
        char message[32];
    } test_data_t;
    
    test_data_t* data = bifrost_writer_alloc(writer, sizeof(test_data_t));
    if (!data) {
        bifrost_writer_leave(writer);
        bifrost_destroy_pool(pool);
        FAIL("Failed to allocate");
        return;
    }
    
    /* Write directly - no memcpy */
    data->value = 42;
    strcpy(data->message, "Hello Bifrost!");
    
    bifrost_writer_set_command(writer, 100);
    bifrost_writer_publish(writer);
    
    /* Join as reader */
    bifrost_reader_t* reader = bifrost_join_as_reader(TEST_POOL, "data_reader");
    if (!reader) {
        bifrost_writer_leave(writer);
        bifrost_destroy_pool(pool);
        FAIL("Failed to join as reader");
        return;
    }
    
    /* Read command */
    uint32_t cmd = bifrost_reader_get_command(reader, "data_writer");
    if (cmd != 100) {
        bifrost_reader_leave(reader);
        bifrost_writer_leave(writer);
        bifrost_destroy_pool(pool);
        FAIL("Command mismatch");
        return;
    }
    
    /* Read data directly */
    size_t size;
    test_data_t* read_data = bifrost_reader_get(reader, "data_writer", &size);
    if (!read_data) {
        bifrost_reader_leave(reader);
        bifrost_writer_leave(writer);
        bifrost_destroy_pool(pool);
        FAIL("Failed to read data");
        return;
    }
    
    if (read_data->value != 42) {
        bifrost_reader_leave(reader);
        bifrost_writer_leave(writer);
        bifrost_destroy_pool(pool);
        FAIL("Value mismatch");
        return;
    }
    
    if (strcmp(read_data->message, "Hello Bifrost!") != 0) {
        bifrost_reader_leave(reader);
        bifrost_writer_leave(writer);
        bifrost_destroy_pool(pool);
        FAIL("Message mismatch");
        return;
    }
    
    bifrost_reader_leave(reader);
    bifrost_writer_leave(writer);
    bifrost_destroy_pool(pool);
    PASS();
}

void test_version_increment(void) {
    TEST("Version increment on publish");
    
    bifrost_pool_t* pool = bifrost_create_pool(TEST_POOL, NULL);
    if (!pool) {
        FAIL("Failed to create pool");
        return;
    }
    
    bifrost_writer_t* writer = bifrost_join_as_writer(TEST_POOL, "ver_writer");
    if (!writer) {
        bifrost_destroy_pool(pool);
        FAIL("Failed to join as writer");
        return;
    }
    
    uint64_t v1 = bifrost_writer_get_version(writer);
    
    int* data = bifrost_writer_alloc(writer, sizeof(int));
    *data = 1;
    bifrost_writer_publish(writer);
    
    uint64_t v2 = bifrost_writer_get_version(writer);
    
    if (v2 <= v1) {
        bifrost_writer_leave(writer);
        bifrost_destroy_pool(pool);
        FAIL("Version should increment");
        return;
    }
    
    bifrost_writer_leave(writer);
    bifrost_destroy_pool(pool);
    PASS();
}

void test_multiple_writers(void) {
    TEST("Multiple writers");
    
    bifrost_pool_t* pool = bifrost_create_pool(TEST_POOL, NULL);
    if (!pool) {
        FAIL("Failed to create pool");
        return;
    }
    
    bifrost_writer_t* w1 = bifrost_join_as_writer(TEST_POOL, "writer_1");
    bifrost_writer_t* w2 = bifrost_join_as_writer(TEST_POOL, "writer_2");
    bifrost_writer_t* w3 = bifrost_join_as_writer(TEST_POOL, "writer_3");
    
    if (!w1 || !w2 || !w3) {
        if (w1) bifrost_writer_leave(w1);
        if (w2) bifrost_writer_leave(w2);
        if (w3) bifrost_writer_leave(w3);
        bifrost_destroy_pool(pool);
        FAIL("Failed to create all writers");
        return;
    }
    
    bifrost_pool_stats_t stats;
    bifrost_get_pool_stats(pool, &stats);
    if (stats.writer_count != 3) {
        bifrost_writer_leave(w1);
        bifrost_writer_leave(w2);
        bifrost_writer_leave(w3);
        bifrost_destroy_pool(pool);
        FAIL("Writer count should be 3");
        return;
    }
    
    /* Each writer writes different data */
    int* d1 = bifrost_writer_alloc(w1, sizeof(int)); *d1 = 111;
    int* d2 = bifrost_writer_alloc(w2, sizeof(int)); *d2 = 222;
    int* d3 = bifrost_writer_alloc(w3, sizeof(int)); *d3 = 333;
    
    bifrost_writer_publish(w1);
    bifrost_writer_publish(w2);
    bifrost_writer_publish(w3);
    
    /* Reader verifies */
    bifrost_reader_t* reader = bifrost_join_as_reader(TEST_POOL, "multi_reader");
    
    int* r1 = bifrost_reader_get(reader, "writer_1", NULL);
    int* r2 = bifrost_reader_get(reader, "writer_2", NULL);
    int* r3 = bifrost_reader_get(reader, "writer_3", NULL);
    
    if (!r1 || !r2 || !r3 || *r1 != 111 || *r2 != 222 || *r3 != 333) {
        bifrost_reader_leave(reader);
        bifrost_writer_leave(w1);
        bifrost_writer_leave(w2);
        bifrost_writer_leave(w3);
        bifrost_destroy_pool(pool);
        FAIL("Data mismatch from multiple writers");
        return;
    }
    
    bifrost_reader_leave(reader);
    bifrost_writer_leave(w1);
    bifrost_writer_leave(w2);
    bifrost_writer_leave(w3);
    bifrost_destroy_pool(pool);
    PASS();
}

void test_list_writers(void) {
    TEST("List writers");
    
    bifrost_pool_t* pool = bifrost_create_pool(TEST_POOL, NULL);
    if (!pool) {
        FAIL("Failed to create pool");
        return;
    }
    
    bifrost_writer_t* w1 = bifrost_join_as_writer(TEST_POOL, "alpha");
    bifrost_writer_t* w2 = bifrost_join_as_writer(TEST_POOL, "beta");
    
    bifrost_reader_t* reader = bifrost_join_as_reader(TEST_POOL, "lister");
    
    bifrost_writer_info_t info[10];
    uint32_t count = bifrost_reader_list_writers(reader, info, 10);
    
    if (count != 2) {
        bifrost_reader_leave(reader);
        bifrost_writer_leave(w1);
        bifrost_writer_leave(w2);
        bifrost_destroy_pool(pool);
        FAIL("Should find 2 writers");
        return;
    }
    
    bifrost_reader_leave(reader);
    bifrost_writer_leave(w1);
    bifrost_writer_leave(w2);
    bifrost_destroy_pool(pool);
    PASS();
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    printf("\nBifrost Basic Tests\n");
    printf("======================\n");
    printf("Library version: %s\n\n", bifrost_version());
    
    /* Clean up any stale pool */
    if (bifrost_pool_exists(TEST_POOL)) {
        printf("Cleaning up stale test pool...\n");
        bifrost_pool_t* p = bifrost_open_pool(TEST_POOL);
        if (p) bifrost_destroy_pool(p);
    }
    
    /* Run tests */
    test_pool_create_destroy();
    test_pool_stats();
    test_writer_join_leave();
    test_reader_join_leave();
    test_write_read_data();
    test_version_increment();
    test_multiple_writers();
    test_list_writers();
    
    /* Summary */
    printf("\n--------------------\n");
    printf("Tests run:    %d\n", tests_run);
    printf("Tests passed: \033[32m%d\033[0m\n", tests_passed);
    printf("Tests failed: \033[31m%d\033[0m\n", tests_failed);
    printf("--------------------\n");
    
    return tests_failed > 0 ? 1 : 0;
}
