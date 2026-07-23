#include "test_main.h"
#include "regex_engine.h"

static void test_basic_match(void)
{
    sm_regex_t *re = sm_regex_compile("hello", NULL, 0);
    ASSERT_NOT_NULL(re);
    ASSERT_INT_EQ(sm_regex_exec(re, "hello world", 11, NULL), 0);
    sm_regex_free(re);
}

static void test_no_match(void)
{
    sm_regex_t *re = sm_regex_compile("hello", NULL, 0);
    ASSERT_NOT_NULL(re);
    ASSERT(sm_regex_exec(re, "goodbye", 7, NULL) != 0, "no match");
    sm_regex_free(re);
}

static void test_anchored_prompt(void)
{
    sm_regex_t *re = sm_regex_compile("\\$ *$", NULL, 0);
    ASSERT_NOT_NULL(re);
    ASSERT_INT_EQ(sm_regex_exec(re, "user@host:~$ ", 13, NULL), 0);
    ASSERT(sm_regex_exec(re, "no prompt here", 14, NULL) != 0, "no match");
    sm_regex_free(re);
}

static void test_invalid_pattern(void)
{
    char errbuf[128] = {0};
    sm_regex_t *re = sm_regex_compile("[invalid", errbuf, sizeof(errbuf));
    ASSERT_NULL(re);
    ASSERT(strlen(errbuf) > 0, "error message set");
}

static void test_backend_name(void)
{
    const char *name = sm_regex_backend();
    ASSERT_NOT_NULL(name);
    /* Should be either "posix" or "pcre2" */
    ASSERT(strcmp(name, "posix") == 0 || strcmp(name, "pcre2") == 0,
           "backend is posix or pcre2");
}

static void test_regex_alternation(void)
{
    sm_regex_t *re = sm_regex_compile("alpha|beta", NULL, 0);
    ASSERT_NOT_NULL(re);
    ASSERT_INT_EQ(sm_regex_exec(re, "has alpha here", 14, NULL), 0);
    ASSERT_INT_EQ(sm_regex_exec(re, "has beta here", 13, NULL), 0);
    ASSERT(sm_regex_exec(re, "has gamma here", 14, NULL) != 0, "no match");
    sm_regex_free(re);
}

static void test_free_null(void)
{
    sm_regex_free(NULL);  /* should not crash */
    ASSERT(1, "free NULL ok");
}

int main(void)
{
    printf("test_regex_engine\n");

    RUN_TEST(test_basic_match);
    RUN_TEST(test_no_match);
    RUN_TEST(test_anchored_prompt);
    RUN_TEST(test_invalid_pattern);
    RUN_TEST(test_backend_name);
    RUN_TEST(test_regex_alternation);
    RUN_TEST(test_free_null);

    TEST_REPORT();
}
