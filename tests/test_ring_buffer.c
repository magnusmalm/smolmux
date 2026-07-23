#include "test_main.h"
#include "ring_buffer.h"

static void test_append_and_get_all(void)
{
    sm_ring_buffer_t rb;
    sm_rb_init(&rb, 1024);

    sm_rb_append(&rb, (const uint8_t *)"hello", 5, 1.0);
    sm_rb_append(&rb, (const uint8_t *)"world", 5, 2.0);

    sm_rb_chunk_t *chunks;
    size_t count = sm_rb_get_all(&rb, &chunks);
    ASSERT_INT_EQ((int)count, 2);
    ASSERT_INT_EQ((int)chunks[0].len, 5);
    ASSERT(memcmp(chunks[0].data, "hello", 5) == 0, "first chunk");
    ASSERT(memcmp(chunks[1].data, "world", 5) == 0, "second chunk");
    free(chunks);

    /* Same timestamp coalesces */
    sm_rb_append(&rb, (const uint8_t *)"!!!", 3, 2.0);
    count = sm_rb_get_all(&rb, &chunks);
    ASSERT_INT_EQ((int)count, 2);
    ASSERT_INT_EQ((int)chunks[1].len, 8);
    ASSERT(memcmp(chunks[1].data, "world!!!", 8) == 0, "coalesced same-ts append");
    free(chunks);

    sm_rb_destroy(&rb);
}

static void test_eviction(void)
{
    sm_ring_buffer_t rb;
    sm_rb_init(&rb, 20);  /* small limit */

    /* Add 30 bytes total → should evict oldest */
    sm_rb_append(&rb, (const uint8_t *)"aaaaaaaaaa", 10, 1.0);
    sm_rb_append(&rb, (const uint8_t *)"bbbbbbbbbb", 10, 2.0);
    sm_rb_append(&rb, (const uint8_t *)"cccccccccc", 10, 3.0);

    ASSERT(rb.total_bytes <= 20, "total_bytes within limit");

    sm_rb_chunk_t *chunks;
    size_t count = sm_rb_get_all(&rb, &chunks);
    ASSERT(count <= 2, "evicted oldest chunks");
    ASSERT(memcmp(chunks[count - 1].data, "cccccccccc", 10) == 0,
           "latest chunk preserved");
    free(chunks);

    sm_rb_destroy(&rb);
}

static void test_get_since(void)
{
    sm_ring_buffer_t rb;
    sm_rb_init(&rb, 1024);

    sm_rb_append(&rb, (const uint8_t *)"a", 1, 1.0);
    sm_rb_append(&rb, (const uint8_t *)"b", 1, 2.0);
    sm_rb_append(&rb, (const uint8_t *)"c", 1, 3.0);

    sm_rb_chunk_t *chunks;
    size_t count = sm_rb_get_since(&rb, 2.0, &chunks);
    ASSERT_INT_EQ((int)count, 2);
    ASSERT(memcmp(chunks[0].data, "b", 1) == 0, "since includes boundary");
    ASSERT(memcmp(chunks[1].data, "c", 1) == 0, "since includes later");
    free(chunks);

    /* since_ts beyond all data */
    count = sm_rb_get_since(&rb, 10.0, &chunks);
    ASSERT_INT_EQ((int)count, 0);

    sm_rb_destroy(&rb);
}

static void test_get_last_n_bytes(void)
{
    sm_ring_buffer_t rb;
    sm_rb_init(&rb, 1024);

    sm_rb_append(&rb, (const uint8_t *)"aaa", 3, 1.0);
    sm_rb_append(&rb, (const uint8_t *)"bb", 2, 2.0);
    sm_rb_append(&rb, (const uint8_t *)"c", 1, 3.0);

    sm_rb_chunk_t *chunks;
    size_t count = sm_rb_get_last_n_bytes(&rb, 3, &chunks);
    ASSERT(count >= 2, "at least 2 chunks");
    free(chunks);

    /* Request 0 bytes */
    count = sm_rb_get_last_n_bytes(&rb, 0, &chunks);
    ASSERT_INT_EQ((int)count, 0);

    sm_rb_destroy(&rb);
}

static void test_empty_buffer(void)
{
    sm_ring_buffer_t rb;
    sm_rb_init(&rb, 1024);

    sm_rb_chunk_t *chunks;
    size_t count = sm_rb_get_all(&rb, &chunks);
    ASSERT_INT_EQ((int)count, 0);

    count = sm_rb_get_since(&rb, 0.0, &chunks);
    ASSERT_INT_EQ((int)count, 0);

    sm_rb_destroy(&rb);
}

int main(void)
{
    printf("test_ring_buffer\n");

    RUN_TEST(test_append_and_get_all);
    RUN_TEST(test_eviction);
    RUN_TEST(test_get_since);
    RUN_TEST(test_get_last_n_bytes);
    RUN_TEST(test_empty_buffer);

    TEST_REPORT();
}
