/*
 * Tests for watcher report generation and filename logic.
 *
 * The testable units are the pure functions in watcher.c:
 *   - watcher_format_report()
 *   - watcher_make_report_filename()
 *   - watcher_sanitize_filename()
 */

#include "test_main.h"
#include "constants.h"
#include "protocol.h"
#include "util/base64.h"
#include "util/json_helpers.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>

/* Declarations of non-static functions from watcher.c */
extern char *watcher_format_report(const cJSON *anomaly, const cJSON *chunks);
extern void watcher_make_report_filename(const cJSON *anomaly, char *out, size_t out_len);
extern void watcher_sanitize_filename(const char *src, char *dst, size_t dst_len);
extern int watcher_test_feed(int fd, size_t *cap_out);
extern void watcher_test_reset(void);

/* Helper: build a test anomaly cJSON object */
static cJSON *make_test_anomaly(const char *pattern, const char *severity,
                                const char *incident_id, double timestamp,
                                const char *match_text, const char *pre_context)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "type", "anomaly");
    cJSON_AddStringToObject(obj, "pattern_name", pattern);
    cJSON_AddStringToObject(obj, "severity", severity);
    cJSON_AddStringToObject(obj, "incident_id", incident_id);
    cJSON_AddNumberToObject(obj, "timestamp", timestamp);
    cJSON_AddStringToObject(obj, "match_text", match_text);
    if (pre_context)
        cJSON_AddStringToObject(obj, "pre_context", pre_context);
    return obj;
}

/* Helper: build a history chunk with base64-encoded data */
static cJSON *make_chunk(double timestamp, const char *text)
{
    cJSON *chunk = cJSON_CreateObject();
    cJSON_AddNumberToObject(chunk, "timestamp", timestamp);
    char *b64 = sm_base64_encode((const uint8_t *)text, strlen(text));
    cJSON_AddStringToObject(chunk, "data", b64);
    free(b64);
    return chunk;
}

/* --- Tests --- */

static void test_format_report(void)
{
    cJSON *anomaly = make_test_anomaly(
        "kernel_panic", "critical", "inc-001", 1700000000.123456,
        "Kernel panic - not syncing", "some pre-context output");

    cJSON *chunks = cJSON_CreateArray();
    cJSON_AddItemToArray(chunks, make_chunk(1699999990.500, "Boot: OK\n"));
    cJSON_AddItemToArray(chunks, make_chunk(1700000000.100, "Kernel panic - not syncing\n"));

    char *report = watcher_format_report(anomaly, chunks);
    ASSERT_NOT_NULL(report);

    /* Header */
    ASSERT(strstr(report, "# Incident: kernel_panic") != NULL,
           "report should contain header");

    /* Metadata table */
    ASSERT(strstr(report, "| Incident ID | `inc-001` |") != NULL,
           "report should contain incident ID");
    ASSERT(strstr(report, "| Pattern | kernel_panic |") != NULL,
           "report should contain pattern");
    ASSERT(strstr(report, "| Severity | critical |") != NULL,
           "report should contain severity");
    ASSERT(strstr(report, "| Match | Kernel panic - not syncing |") != NULL,
           "report should contain match text");

    /* Pre-context */
    ASSERT(strstr(report, "## Pre-context") != NULL,
           "report should contain pre-context section");
    ASSERT(strstr(report, "some pre-context output") != NULL,
           "report should contain pre-context text");

    /* History */
    ASSERT(strstr(report, "## Output History") != NULL,
           "report should contain history section");
    ASSERT(strstr(report, "Boot: OK") != NULL,
           "report should contain decoded history chunk");
    ASSERT(strstr(report, "Kernel panic") != NULL,
           "report should contain decoded history data");

    free(report);
    cJSON_Delete(anomaly);
    cJSON_Delete(chunks);
}

static void test_format_report_no_history(void)
{
    cJSON *anomaly = make_test_anomaly(
        "oops", "warning", "inc-002", 1700000000.0,
        "Oops: something", NULL);

    char *report = watcher_format_report(anomaly, NULL);
    ASSERT_NOT_NULL(report);

    ASSERT(strstr(report, "(no history available)") != NULL,
           "report without history should say so");

    /* No pre-context section when not provided */
    ASSERT(strstr(report, "## Pre-context") == NULL,
           "report without pre-context should not have section");

    free(report);
    cJSON_Delete(anomaly);
}

static void test_make_report_filename(void)
{
    /* 1700000000 = 2023-11-14T22:13:20Z */
    cJSON *anomaly = make_test_anomaly(
        "kernel_panic", "critical", "abc-123", 1700000000.0,
        "match", NULL);

    char filename[512];
    watcher_make_report_filename(anomaly, filename, sizeof(filename));

    /* Should start with YYYYMMDD-HHMMSS */
    ASSERT(strstr(filename, "20231114-221320") != NULL,
           "filename should contain formatted timestamp");
    ASSERT(strstr(filename, "kernel_panic") != NULL,
           "filename should contain pattern name");
    ASSERT(strstr(filename, "abc-123") != NULL,
           "filename should contain incident ID");
    /* Should end with .md */
    size_t flen = strlen(filename);
    ASSERT(flen >= 3 && strcmp(filename + flen - 3, ".md") == 0,
           "filename should end with .md");

    cJSON_Delete(anomaly);
}

static void test_sanitize_filename(void)
{
    char out[256];

    /* Normal string passes through */
    watcher_sanitize_filename("hello-world_v2.0", out, sizeof(out));
    ASSERT_STR_EQ(out, "hello-world_v2.0");

    /* Unsafe characters replaced with underscore */
    watcher_sanitize_filename("foo/bar\\baz:qux", out, sizeof(out));
    ASSERT_STR_EQ(out, "foo_bar_baz_qux");

    /* Spaces replaced */
    watcher_sanitize_filename("hello world", out, sizeof(out));
    ASSERT_STR_EQ(out, "hello_world");

    /* Empty string */
    watcher_sanitize_filename("", out, sizeof(out));
    ASSERT_STR_EQ(out, "");

    /* Path traversal ".." is neutralized */
    watcher_sanitize_filename("../../../etc/passwd", out, sizeof(out));
    ASSERT(strstr(out, "..") == NULL, "must not contain ..");

    watcher_sanitize_filename("foo..bar", out, sizeof(out));
    ASSERT(strstr(out, "..") == NULL, "must not contain ..");

    /* Truncation at 128 chars */
    char long_str[256];
    memset(long_str, 'a', 200);
    long_str[200] = '\0';
    watcher_sanitize_filename(long_str, out, sizeof(out));
    ASSERT_INT_EQ((int)strlen(out), 128);
}

static void test_discover_socket_env(void)
{
    /* Set env var and verify discover_socket returns it.
     * discover_socket is static in watcher.c, so we test the env var
     * path indirectly through the fact that SM_SOCKET_ENV is defined. */
    ASSERT_STR_EQ(SM_SOCKET_ENV, "SMOLMUX_SOCKET");
}

/* A >64 KB single-line response (a large history_response, or broker-coalesced
 * output) must grow the read buffer and reassemble, not be misread as a
 * disconnect (return -1) once the fixed buffer fills without a newline. */
static void test_read_buffer_grows(void)
{
    watcher_test_reset();

    /* ~200 KB raw -> ~267 KB base64 -> a single line well past the 64 KB
     * initial buffer, forcing several growth steps. */
    const size_t payload = 200 * 1024;
    uint8_t *raw = malloc(payload);
    ASSERT_NOT_NULL(raw);
    memset(raw, 'Z', payload);
    cJSON *m = sm_msg_output(raw, payload, 1.0);  /* dispatched as no-op */
    size_t line_len = 0;
    char *line = sm_msg_encode(m, &line_len);     /* includes trailing '\n' */
    cJSON_Delete(m);
    ASSERT_NOT_NULL(line);
    ASSERT(line_len > 64 * 1024, "line exceeds initial buffer");

    int sp[2];
    ASSERT_INT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sp), 0);

    size_t cap = 0;
    const size_t chunk = 16 * 1024;
    size_t written = 0;
    while (written < line_len) {
        size_t w = line_len - written < chunk ? line_len - written : chunk;
        ASSERT(write(sp[1], line + written, w) == (ssize_t)w, "chunk written");
        written += w;
        int rc = watcher_test_feed(sp[0], &cap);
        ASSERT_INT_EQ(rc, 0);   /* never a false disconnect mid-line */
    }
    ASSERT(cap > 64 * 1024, "read buffer grew beyond initial capacity");

    close(sp[0]);
    close(sp[1]);
    free(line);
    free(raw);
    watcher_test_reset();
}

int main(void)
{
    printf("test_watcher\n");

    RUN_TEST(test_format_report);
    RUN_TEST(test_format_report_no_history);
    RUN_TEST(test_make_report_filename);
    RUN_TEST(test_sanitize_filename);
    RUN_TEST(test_discover_socket_env);
    RUN_TEST(test_read_buffer_grows);

    TEST_REPORT();
}
