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

/* Wave 3: monotonic seq cursor + dropped on eviction. */
static void test_since_seq_two_page(void)
{
    sm_ring_buffer_t rb;
    sm_rb_init(&rb, 1024);

    sm_rb_append(&rb, (const uint8_t *)"AAAA", 4, 1.0);
    sm_rb_append(&rb, (const uint8_t *)"BBBB", 4, 2.0);
    sm_rb_append(&rb, (const uint8_t *)"CCCC", 4, 3.0);
    ASSERT_INT_EQ((int)sm_rb_total_seq(&rb), 12);

    sm_rb_chunk_t *chunks = NULL;
    size_t skip = 0;
    uint64_t cursor = 0, dropped = 0;
    int has_more = 0;
    size_t n = sm_rb_get_since_seq(&rb, 0, 6, &chunks, &skip, &cursor,
                                   &dropped, &has_more);
    ASSERT(n >= 1, "page1 has chunks");
    ASSERT_INT_EQ((int)dropped, 0);
    ASSERT_INT_EQ((int)skip, 0);
    ASSERT(has_more == 1, "more after 6-byte page");
    ASSERT_INT_EQ((int)cursor, 6);
    free(chunks);

    n = sm_rb_get_since_seq(&rb, cursor, 0, &chunks, &skip, &cursor,
                            &dropped, &has_more);
    ASSERT(n >= 1, "page2 has chunks");
    ASSERT_INT_EQ((int)dropped, 0);
    ASSERT(has_more == 0, "no more after rest");
    ASSERT_INT_EQ((int)cursor, 12);

    /* Reconstruct from both pages' usable bytes */
    free(chunks);
    sm_rb_destroy(&rb);
}

static void test_since_seq_dropped(void)
{
    sm_ring_buffer_t rb;
    sm_rb_init(&rb, 10);

    sm_rb_append(&rb, (const uint8_t *)"0123456789", 10, 1.0);
    sm_rb_append(&rb, (const uint8_t *)"ABCDEF", 6, 2.0);
    /* Eviction should drop some early bytes */
    ASSERT(sm_rb_first_seq(&rb) > 0, "first_seq advanced after eviction");

    sm_rb_chunk_t *chunks = NULL;
    size_t skip = 0;
    uint64_t cursor = 0, dropped = 0;
    int has_more = 0;
    size_t n = sm_rb_get_since_seq(&rb, 0, 0, &chunks, &skip, &cursor,
                                   &dropped, &has_more);
    ASSERT(dropped > 0, "stale since_seq reports dropped");
    ASSERT(n >= 1 || cursor == sm_rb_total_seq(&rb), "query completes");
    free(chunks);
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
    RUN_TEST(test_since_seq_two_page);
    RUN_TEST(test_since_seq_dropped);

    TEST_REPORT();
}
