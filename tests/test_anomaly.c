#include "test_main.h"
#include "anomaly.h"

static void test_detect_kernel_panic(void)
{
    sm_anomaly_detector_t det;
    sm_anomaly_init(&det);
    sm_anomaly_add_builtins(&det);

    size_t n = sm_anomaly_feed(&det, (const uint8_t *)"Kernel panic - not syncing: VFS",
                               31, 1000.0);
    ASSERT(n >= 1, "detected kernel panic");

    size_t count;
    const sm_anomaly_incident_t *incs = sm_anomaly_get_incidents(&det, &count);
    ASSERT(count >= 1, "incident recorded");
    ASSERT_STR_EQ(incs[0].pattern_name, "kernel_panic");
    ASSERT_STR_EQ(incs[0].severity, "critical");

    sm_anomaly_destroy(&det);
}

static void test_detect_segfault(void)
{
    sm_anomaly_detector_t det;
    sm_anomaly_init(&det);
    sm_anomaly_add_builtins(&det);

    size_t n = sm_anomaly_feed(&det, (const uint8_t *)"Segmentation fault (core dumped)",
                               31, 1000.0);
    ASSERT(n >= 1, "detected segfault");

    sm_anomaly_destroy(&det);
}

static void test_detect_hard_fault(void)
{
    sm_anomaly_detector_t det;
    sm_anomaly_init(&det);
    sm_anomaly_add_builtins(&det);

    size_t n = sm_anomaly_feed(&det, (const uint8_t *)"HardFault exception occurred",
                               28, 1000.0);
    ASSERT(n >= 1, "detected hard fault");

    sm_anomaly_destroy(&det);
}

static void test_no_false_positives(void)
{
    sm_anomaly_detector_t det;
    sm_anomaly_init(&det);
    sm_anomaly_add_builtins(&det);

    size_t n = sm_anomaly_feed(&det, (const uint8_t *)"normal boot output\neverything ok\n",
                               32, 1000.0);
    ASSERT_INT_EQ((int)n, 0);

    sm_anomaly_destroy(&det);
}

static void test_cooldown(void)
{
    sm_anomaly_detector_t det;
    sm_anomaly_init(&det);
    sm_anomaly_add_builtins(&det);

    /* First detection */
    size_t n1 = sm_anomaly_feed(&det, (const uint8_t *)"Kernel panic!", 13, 1000.0);
    ASSERT(n1 >= 1, "first detection");

    /* Reset window so we can trigger again */
    det.window_len = 0;

    /* Same pattern within cooldown */
    size_t n2 = sm_anomaly_feed(&det, (const uint8_t *)"Kernel panic again!", 19, 1001.0);
    ASSERT_INT_EQ((int)n2, 0);  /* blocked by cooldown */

    sm_anomaly_destroy(&det);
}

static void test_custom_pattern(void)
{
    sm_anomaly_detector_t det;
    sm_anomaly_init(&det);

    int rc = sm_anomaly_add_pattern(&det, "my_error", "CUSTOM_ERROR_[0-9]+", "warning");
    ASSERT_INT_EQ(rc, 0);

    size_t n = sm_anomaly_feed(&det, (const uint8_t *)"got CUSTOM_ERROR_42 here", 24, 1000.0);
    ASSERT(n >= 1, "custom pattern detected");

    size_t count;
    const sm_anomaly_incident_t *incs = sm_anomaly_get_incidents(&det, &count);
    ASSERT_STR_EQ(incs[0].pattern_name, "my_error");

    sm_anomaly_destroy(&det);
}

static void test_literal_prefilter_wdt(void)
{
    sm_anomaly_detector_t det;
    sm_anomaly_init(&det);
    sm_anomaly_add_builtins(&det);

    size_t n = sm_anomaly_feed(&det, (const uint8_t *)"wdt timeout on cpu0",
                               20, 1000.0);
    ASSERT(n >= 1, "wdt literal prefilter allows watchdog regex");

    sm_anomaly_destroy(&det);
}

static void test_pre_context(void)
{
    sm_anomaly_detector_t det;
    sm_anomaly_init(&det);
    sm_anomaly_add_builtins(&det);

    /* Feed some pre-context data first */
    sm_anomaly_feed(&det, (const uint8_t *)"boot: loading kernel\n", 21, 999.0);
    /* Reset window but keep context */
    det.window_len = 0;

    /* Now trigger anomaly */
    sm_anomaly_feed(&det, (const uint8_t *)"Kernel panic!", 13, 1000.0);

    size_t count;
    const sm_anomaly_incident_t *incs = sm_anomaly_get_incidents(&det, &count);
    ASSERT(count >= 1, "incident recorded");
    ASSERT(strlen(incs[count - 1].pre_context) > 0, "pre-context captured");

    sm_anomaly_destroy(&det);
}

/* Regression (found by live SAM C21 dogfooding): on a chatty long-running
 * stream, match_text used to be copied from window[0] — the oldest bytes — so
 * every incident reported stale boot noise instead of the line that matched.
 * match_text must now begin at the matched line. */
static void test_match_text_is_the_matched_line(void)
{
    sm_anomaly_detector_t det;
    sm_anomaly_init(&det);
    sm_anomaly_add_builtins(&det);

    /* Fill the window with noise so the match sits far from window[0]. */
    for (int i = 0; i < 50; i++)
        sm_anomaly_feed(&det, (const uint8_t *)"hello world noise line\n", 23,
                        1000.0 + i);

    size_t n = sm_anomaly_feed(&det,
        (const uint8_t *)"Kernel panic - not syncing: dogfood\n", 36, 1100.0);
    ASSERT(n >= 1, "panic detected after chatty stream");

    size_t count;
    const sm_anomaly_incident_t *incs = sm_anomaly_get_incidents(&det, &count);
    ASSERT(count >= 1, "incident recorded");
    ASSERT(strncmp(incs[count - 1].match_text, "Kernel panic", 12) == 0,
           "match_text begins with the matched signature, not the window head");
    ASSERT(strstr(incs[count - 1].match_text, "hello world noise") == NULL,
           "match_text does not lead with unrelated chatter");

    sm_anomaly_destroy(&det);
}

int main(void)
{
    printf("test_anomaly\n");

    RUN_TEST(test_detect_kernel_panic);
    RUN_TEST(test_detect_segfault);
    RUN_TEST(test_detect_hard_fault);
    RUN_TEST(test_no_false_positives);
    RUN_TEST(test_cooldown);
    RUN_TEST(test_custom_pattern);
    RUN_TEST(test_literal_prefilter_wdt);
    RUN_TEST(test_pre_context);
    RUN_TEST(test_match_text_is_the_matched_line);

    TEST_REPORT();
}
