#include "test_main.h"
#include "util/str.h"

static void test_init_and_destroy(void)
{
    sm_strbuf_t sb;
    sm_strbuf_init(&sb);
    ASSERT(sb.data == NULL, "init data is NULL");
    ASSERT(sb.len == 0, "init len is 0");
    sm_strbuf_destroy(&sb);
}

static void test_append(void)
{
    sm_strbuf_t sb;
    sm_strbuf_init(&sb);
    sm_strbuf_append(&sb, "hello", 5);
    ASSERT_INT_EQ((int)sb.len, 5);
    ASSERT_STR_EQ(sb.data, "hello");

    sm_strbuf_append_str(&sb, " world");
    ASSERT_INT_EQ((int)sb.len, 11);
    ASSERT_STR_EQ(sb.data, "hello world");

    sm_strbuf_destroy(&sb);
}

static void test_printf(void)
{
    sm_strbuf_t sb;
    sm_strbuf_init(&sb);
    sm_strbuf_printf(&sb, "num=%d str=%s", 42, "test");
    ASSERT_STR_EQ(sb.data, "num=42 str=test");
    sm_strbuf_destroy(&sb);
}

static void test_steal(void)
{
    sm_strbuf_t sb;
    sm_strbuf_init(&sb);
    sm_strbuf_append_str(&sb, "stolen");
    char *s = sm_strbuf_steal(&sb);
    ASSERT_STR_EQ(s, "stolen");
    ASSERT_NULL(sb.data);
    ASSERT_INT_EQ((int)sb.len, 0);
    free(s);
}

static void test_empty_append(void)
{
    sm_strbuf_t sb;
    sm_strbuf_init(&sb);
    sm_strbuf_append(&sb, "", 0);
    ASSERT_INT_EQ((int)sb.len, 0);
    sm_strbuf_destroy(&sb);
}

int main(void)
{
    printf("test_str\n");

    RUN_TEST(test_init_and_destroy);
    RUN_TEST(test_append);
    RUN_TEST(test_printf);
    RUN_TEST(test_steal);
    RUN_TEST(test_empty_append);

    TEST_REPORT();
}
