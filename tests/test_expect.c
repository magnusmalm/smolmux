#include "test_main.h"
#include "expect.h"

#include <time.h>
#include <unistd.h>

static double now_mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void test_immediate_match(void)
{
    sm_expect_engine_t eng;
    sm_expect_init(&eng);

    sm_expect_add(&eng, "req1", "hello", 5.0, "client-1");
    sm_expect_feed(&eng, (const uint8_t *)"hello world", 11);

    sm_expect_result_t results[4];
    size_t count = sm_expect_collect(&eng, now_mono(), results, 4);
    ASSERT_INT_EQ((int)count, 1);
    ASSERT_INT_EQ(results[0].matched, 1);
    ASSERT_STR_EQ(results[0].id, "req1");
    free(results[0].data);

    sm_expect_destroy(&eng);
}

static void test_accumulated_match(void)
{
    sm_expect_engine_t eng;
    sm_expect_init(&eng);

    sm_expect_add(&eng, "req2", "complete", 5.0, "client-1");

    /* Data arrives in fragments */
    sm_expect_feed(&eng, (const uint8_t *)"com", 3);
    sm_expect_result_t results[4];
    size_t count = sm_expect_collect(&eng, now_mono(), results, 4);
    ASSERT_INT_EQ((int)count, 0);  /* not matched yet */

    sm_expect_feed(&eng, (const uint8_t *)"plete!", 6);
    count = sm_expect_collect(&eng, now_mono(), results, 4);
    ASSERT_INT_EQ((int)count, 1);
    ASSERT_INT_EQ(results[0].matched, 1);
    ASSERT_INT_EQ((int)results[0].data_len, 9);
    free(results[0].data);

    sm_expect_destroy(&eng);
}

static void test_timeout(void)
{
    sm_expect_engine_t eng;
    sm_expect_init(&eng);

    /* Use a very short timeout */
    sm_expect_add(&eng, "req3", "never_match", 0.01, "client-1");
    sm_expect_feed(&eng, (const uint8_t *)"other data", 10);

    /* Wait for timeout */
    usleep(20000);  /* 20ms */

    sm_expect_result_t results[4];
    size_t count = sm_expect_collect(&eng, now_mono(), results, 4);
    ASSERT_INT_EQ((int)count, 1);
    ASSERT_INT_EQ(results[0].matched, 0);
    free(results[0].data);

    sm_expect_destroy(&eng);
}

static void test_multiple_concurrent(void)
{
    sm_expect_engine_t eng;
    sm_expect_init(&eng);

    sm_expect_add(&eng, "req-a", "alpha", 5.0, "client-1");
    sm_expect_add(&eng, "req-b", "beta", 5.0, "client-2");

    sm_expect_feed(&eng, (const uint8_t *)"alpha and beta here", 19);

    sm_expect_result_t results[4];
    size_t count = sm_expect_collect(&eng, now_mono(), results, 4);
    ASSERT_INT_EQ((int)count, 2);

    /* Both should match */
    int a_matched = 0, b_matched = 0;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(results[i].id, "req-a") == 0) a_matched = results[i].matched;
        if (strcmp(results[i].id, "req-b") == 0) b_matched = results[i].matched;
        free(results[i].data);
    }
    ASSERT_INT_EQ(a_matched, 1);
    ASSERT_INT_EQ(b_matched, 1);

    sm_expect_destroy(&eng);
}

static void test_cancel_client(void)
{
    sm_expect_engine_t eng;
    sm_expect_init(&eng);

    sm_expect_add(&eng, "req-x", "pattern", 5.0, "client-1");
    sm_expect_add(&eng, "req-y", "pattern", 5.0, "client-2");

    sm_expect_cancel_client(&eng, "client-1");

    /* Only client-2's expect should remain */
    ASSERT_INT_EQ((int)eng.count, 1);
    ASSERT_STR_EQ(eng.requests[0].client_id, "client-2");

    sm_expect_destroy(&eng);
}

static void test_regex_special_chars(void)
{
    sm_expect_engine_t eng;
    sm_expect_init(&eng);

    /* Match a shell prompt like "$ " at end of line */
    sm_expect_add(&eng, "req-p", "\\$ $", 5.0, "client-1");
    sm_expect_feed(&eng, (const uint8_t *)"user@host:~$ ", 13);

    sm_expect_result_t results[4];
    size_t count = sm_expect_collect(&eng, now_mono(), results, 4);
    ASSERT_INT_EQ((int)count, 1);
    ASSERT_INT_EQ(results[0].matched, 1);
    free(results[0].data);

    sm_expect_destroy(&eng);
}

static void test_invalid_pattern(void)
{
    sm_expect_engine_t eng;
    sm_expect_init(&eng);

    int rc = sm_expect_add(&eng, "bad", "[invalid", 5.0, "client-1");
    ASSERT(rc != 0, "invalid regex rejected");
    ASSERT_INT_EQ((int)eng.count, 0);

    sm_expect_destroy(&eng);
}

/* The scan-offset optimization (H4 fix) only rescans a small lookback
 * window before new data. These tests pin the behavior it must preserve:
 * matches in, and spanning into, late chunks of a large buffer. */

static void test_match_in_large_buffer(void)
{
    sm_expect_engine_t eng;
    sm_expect_init(&eng);

    sm_expect_add(&eng, "big", "NEEDLE", 5.0, "client-1");

    /* Push the buffer well past the lookback window with noise */
    uint8_t noise[512];
    memset(noise, 'x', sizeof(noise));
    for (int i = 0; i < 16; i++)
        sm_expect_feed(&eng, noise, sizeof(noise));

    /* Pattern arriving in a late chunk must still match */
    sm_expect_feed(&eng, (const uint8_t *)"...NEEDLE...", 12);

    sm_expect_result_t results[4];
    size_t count = sm_expect_collect(&eng, now_mono(), results, 4);
    ASSERT_INT_EQ((int)count, 1);
    ASSERT_INT_EQ(results[0].matched, 1);
    free(results[0].data);

    sm_expect_destroy(&eng);
}

static void test_match_spanning_chunks_in_large_buffer(void)
{
    sm_expect_engine_t eng;
    sm_expect_init(&eng);

    sm_expect_add(&eng, "span", "MAGIC_TOKEN", 5.0, "client-1");

    uint8_t noise[512];
    memset(noise, 'x', sizeof(noise));
    for (int i = 0; i < 16; i++)
        sm_expect_feed(&eng, noise, sizeof(noise));

    /* Pattern split across two feeds: the second scan must look back far
     * enough to see the prefix from the previous chunk */
    sm_expect_feed(&eng, (const uint8_t *)"MAGIC_", 6);
    sm_expect_feed(&eng, (const uint8_t *)"TOKEN", 5);

    sm_expect_result_t results[4];
    size_t count = sm_expect_collect(&eng, now_mono(), results, 4);
    ASSERT_INT_EQ((int)count, 1);
    ASSERT_INT_EQ(results[0].matched, 1);
    free(results[0].data);

    sm_expect_destroy(&eng);
}

int main(void)
{
    printf("test_expect\n");

    RUN_TEST(test_immediate_match);
    RUN_TEST(test_accumulated_match);
    RUN_TEST(test_timeout);
    RUN_TEST(test_multiple_concurrent);
    RUN_TEST(test_cancel_client);
    RUN_TEST(test_regex_special_chars);
    RUN_TEST(test_invalid_pattern);
    RUN_TEST(test_match_in_large_buffer);
    RUN_TEST(test_match_spanning_chunks_in_large_buffer);

    TEST_REPORT();
}
