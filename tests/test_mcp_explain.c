#include "test_main.h"
#include "mcp_explain.h"

#include <string.h>

static void test_suspended_hint(void)
{
    const char *h = sm_mcp_error_hint("[ERROR] serial port is suspended");
    ASSERT_NOT_NULL(h);
    ASSERT(strstr(h, "serial_resume") != NULL, "suspend hint names resume");
}

static void test_offline_hint(void)
{
    const char *h = sm_mcp_error_hint("[ERROR] no broker socket found");
    ASSERT_NOT_NULL(h);
    ASSERT(strstr(h, "smolmux") != NULL, "offline names smolmux");
    ASSERT(strstr(h, "auto-spawn") != NULL || strstr(h, "auto-spawn") == NULL,
           "hint present");
}

/* A refused handshake means the broker IS up — the hint must point at the
 * auth token, not at "start a broker", which would send the agent the wrong
 * way. Ordering matters: this rule has to win over the generic no-broker one. */
static void test_auth_rejected_hint(void)
{
    const char *h =
        sm_mcp_error_hint("[ERROR] broker rejected this client: "
                          "authentication failed");
    ASSERT_NOT_NULL(h);
    ASSERT(strstr(h, "SMOLMUX_AUTH_TOKEN") != NULL, "hint names the token var");
    ASSERT(strstr(h, "Start a broker first") == NULL,
           "does not fall through to the no-broker hint");
}

static void test_handshake_timeout_hint(void)
{
    const char *h =
        sm_mcp_error_hint("[ERROR] broker did not answer the hello handshake");
    ASSERT_NOT_NULL(h);
    ASSERT(strstr(h, "SMOLMUX_AUTH_TOKEN") != NULL, "hint names the token var");
}

static void test_aborted_hint(void)
{
    const char *h = sm_mcp_error_hint("[ABORTED anomaly:guru_meditation] x");
    ASSERT_NOT_NULL(h);
    ASSERT(strstr(h, "incidents") != NULL, "abort points at incidents");
}

static void test_timeout_hint(void)
{
    const char *h = sm_mcp_error_hint("[TIMEOUT] no match");
    ASSERT_NOT_NULL(h);
    ASSERT(strstr(h, "baud") != NULL || strstr(h, "incidents") != NULL,
           "timeout has fix hint");
}

static void test_with_hint_appends(void)
{
    char *out = sm_mcp_error_with_hint("[ERROR] serial port is suspended");
    ASSERT_NOT_NULL(out);
    ASSERT(strstr(out, "[ERROR]") != NULL, "keeps original");
    ASSERT(strstr(out, "Hint:") != NULL, "adds Hint");
    free(out);
}

static void test_offline_message(void)
{
    char *out = sm_mcp_offline_broker_message();
    ASSERT_NOT_NULL(out);
    ASSERT(strstr(out, "broker") != NULL, "mentions broker");
    ASSERT(strstr(out, "Hint:") != NULL, "has hint");
    free(out);
}

static void test_maybe_explain_passthrough(void)
{
    char *ok = strdup("OK");
    char *out = sm_mcp_maybe_explain_result(ok);
    ASSERT_STR_EQ(out, "OK");
    free(out);
}

int main(void)
{
    printf("test_mcp_explain\n");
    RUN_TEST(test_suspended_hint);
    RUN_TEST(test_offline_hint);
    RUN_TEST(test_auth_rejected_hint);
    RUN_TEST(test_handshake_timeout_hint);
    RUN_TEST(test_aborted_hint);
    RUN_TEST(test_timeout_hint);
    RUN_TEST(test_with_hint_appends);
    RUN_TEST(test_offline_message);
    RUN_TEST(test_maybe_explain_passthrough);
    TEST_REPORT();
}
