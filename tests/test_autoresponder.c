#include "test_main.h"
#include "autoresponder.h"

static size_t feed(sm_autoresponder_t *ar, const char *s, double ts,
                   sm_ar_fired_t *out, size_t max)
{
    return sm_autoresponder_feed(ar, (const uint8_t *)s, strlen(s), ts, out, max);
}

static void test_basic_match_and_response(void)
{
    sm_autoresponder_t ar;
    sm_autoresponder_init(&ar);
    ASSERT_INT_EQ(sm_autoresponder_add(&ar, "menu", "Press any key",
                                       (const uint8_t *)" ", 1, 0, 1000), 0);

    sm_ar_fired_t out[4];
    ASSERT_INT_EQ((int)feed(&ar, "booting...\nPress any key to stop\n", 1.0, out, 4), 1);
    ASSERT_STR_EQ(out[0].name, "menu");
    ASSERT_INT_EQ((int)out[0].response_len, 1);
    ASSERT(out[0].response[0] == ' ', "response is a space");
    ASSERT(strstr(out[0].matched_text, "Press any key") != NULL, "matched line captured");

    sm_autoresponder_destroy(&ar);
}

static void test_cooldown_prevents_refire(void)
{
    sm_autoresponder_t ar;
    sm_autoresponder_init(&ar);
    sm_autoresponder_add(&ar, "yn", "\\[y/N\\]", (const uint8_t *)"y\n", 2, 0, 1000);

    sm_ar_fired_t out[4];
    ASSERT_INT_EQ((int)feed(&ar, "Continue? [y/N] ", 1.0, out, 4), 1);
    /* Same prompt again within cooldown -> no re-fire. */
    ASSERT_INT_EQ((int)feed(&ar, "Continue? [y/N] ", 1.5, out, 4), 0);
    /* Past cooldown -> fires again. */
    ASSERT_INT_EQ((int)feed(&ar, "Continue? [y/N] ", 3.0, out, 4), 1);

    sm_autoresponder_destroy(&ar);
}

static void test_once_disables_after_fire(void)
{
    sm_autoresponder_t ar;
    sm_autoresponder_init(&ar);
    sm_autoresponder_add(&ar, "login", "login:", (const uint8_t *)"root\n", 5,
                         /*once*/1, 1);

    sm_ar_fired_t out[4];
    ASSERT_INT_EQ((int)feed(&ar, "buildroot login:", 1.0, out, 4), 1);
    /* once=1 -> disabled, never fires again even past cooldown. */
    ASSERT_INT_EQ((int)feed(&ar, "buildroot login:", 5.0, out, 4), 0);

    sm_autoresponder_destroy(&ar);
}

static void test_multiple_rules(void)
{
    sm_autoresponder_t ar;
    sm_autoresponder_init(&ar);
    sm_autoresponder_add(&ar, "a", "AAA", (const uint8_t *)"1", 1, 0, 1000);
    sm_autoresponder_add(&ar, "b", "BBB", (const uint8_t *)"2", 1, 0, 1000);

    sm_ar_fired_t out[4];
    /* Both patterns present in one chunk -> both fire. */
    ASSERT_INT_EQ((int)feed(&ar, "xxAAAyyBBBzz", 1.0, out, 4), 2);

    sm_autoresponder_destroy(&ar);
}

static void test_add_replaces_by_name(void)
{
    sm_autoresponder_t ar;
    sm_autoresponder_init(&ar);
    sm_autoresponder_add(&ar, "r", "FOO", (const uint8_t *)"1", 1, 0, 1000);
    /* Re-add same name with a different pattern: replaces, not duplicates. */
    sm_autoresponder_add(&ar, "r", "BAR", (const uint8_t *)"2", 1, 0, 1000);
    ASSERT_INT_EQ((int)ar.rule_count, 1);

    sm_ar_fired_t out[4];
    ASSERT_INT_EQ((int)feed(&ar, "FOO", 1.0, out, 4), 0);   /* old pattern gone */
    ASSERT_INT_EQ((int)feed(&ar, "BAR", 2.0, out, 4), 1);   /* new pattern active */

    sm_autoresponder_destroy(&ar);
}

static void test_remove(void)
{
    sm_autoresponder_t ar;
    sm_autoresponder_init(&ar);
    sm_autoresponder_add(&ar, "x", "ZZZ", (const uint8_t *)"1", 1, 0, 1000);
    ASSERT_INT_EQ(sm_autoresponder_remove(&ar, "x"), 1);
    ASSERT_INT_EQ(sm_autoresponder_remove(&ar, "x"), 0);   /* already gone */
    ASSERT_INT_EQ((int)ar.rule_count, 0);

    sm_ar_fired_t out[4];
    ASSERT_INT_EQ((int)feed(&ar, "ZZZ", 1.0, out, 4), 0);

    sm_autoresponder_destroy(&ar);
}

static void test_cross_chunk_match(void)
{
    sm_autoresponder_t ar;
    sm_autoresponder_init(&ar);
    sm_autoresponder_add(&ar, "p", "PROMPT>", (const uint8_t *)"go\n", 3, 0, 1000);

    sm_ar_fired_t out[4];
    ASSERT_INT_EQ((int)feed(&ar, "PROM", 1.0, out, 4), 0);
    ASSERT_INT_EQ((int)feed(&ar, "PT> ", 2.0, out, 4), 1);   /* spans the boundary */

    sm_autoresponder_destroy(&ar);
}

static void test_bad_regex_rejected(void)
{
    sm_autoresponder_t ar;
    sm_autoresponder_init(&ar);
    ASSERT_INT_EQ(sm_autoresponder_add(&ar, "bad", "[unclosed",
                                       (const uint8_t *)"x", 1, 0, 1000), -1);
    ASSERT_INT_EQ((int)ar.rule_count, 0);
    sm_autoresponder_destroy(&ar);
}

int main(void)
{
    printf("test_autoresponder\n");

    RUN_TEST(test_basic_match_and_response);
    RUN_TEST(test_cooldown_prevents_refire);
    RUN_TEST(test_once_disables_after_fire);
    RUN_TEST(test_multiple_rules);
    RUN_TEST(test_add_replaces_by_name);
    RUN_TEST(test_remove);
    RUN_TEST(test_cross_chunk_match);
    RUN_TEST(test_bad_regex_rejected);

    TEST_REPORT();
}
