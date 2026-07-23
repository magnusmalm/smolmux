#include "test_main.h"
#include "text_log.h"

#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Fixed reference instant: 2009-02-13 23:31:30 UTC. All expectations are
 * derived through localtime_r() with the same seconds value the code under
 * test receives, so the tests are timezone-independent. */
#define TS_BASE 1234567890.0

static char g_dir[512];

static void set_dir(const char *name)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof(g_dir), "%s/smolmux-test-textlog-%s",
             tmp && tmp[0] ? tmp : "/tmp", name);
}

static void expected_path(char *out, size_t out_len, const char *file_port, double ts)
{
    time_t sec = (time_t)ts;
    struct tm tm;
    localtime_r(&sec, &tm);
    snprintf(out, out_len, "%s/%s-%04d%02d%02d.log", g_dir, file_port,
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

static void expected_stamp(char *out, size_t out_len, double ts)
{
    time_t sec = (time_t)ts;
    struct tm tm;
    localtime_r(&sec, &tm);
    snprintf(out, out_len, "[%02d:%02d:%02d] ", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* Read one line (without trailing \n) into buf; returns buf or NULL. */
static char *read_line(FILE *fp, char *buf, size_t len)
{
    if (!fgets(buf, (int)len, fp)) return NULL;
    buf[strcspn(buf, "\n")] = '\0';
    return buf;
}

static void test_filename_and_line_format(void)
{
    set_dir("basic");
    char path[600];
    expected_path(path, sizeof(path), "ttyUSB0", TS_BASE);
    unlink(path);

    sm_text_log_t *log = sm_text_log_open(g_dir, "/dev/ttyUSB0");
    ASSERT_NOT_NULL(log);
    if (!log) return;

    /* CRLF line + an empty line + a partial line flushed by close */
    sm_text_log_write(log, (const uint8_t *)"hello\r\n\r\npartial", 16, TS_BASE);
    sm_text_log_close(log);

    /* /dev/ prefix stripped, date from the write timestamp's local day */
    FILE *fp = fopen(path, "r");
    ASSERT_NOT_NULL(fp);
    if (!fp) return;

    char stamp[32], line[256];
    expected_stamp(stamp, sizeof(stamp), TS_BASE);

    char want[300];
    snprintf(want, sizeof(want), "%shello", stamp);
    ASSERT_STR_EQ(read_line(fp, line, sizeof(line)), want);

    /* Empty line: timestamp only (with its trailing space), \r dropped */
    ASSERT_STR_EQ(read_line(fp, line, sizeof(line)), stamp);

    /* Partial line flushed on close, no timestamp prefix */
    ASSERT_STR_EQ(read_line(fp, line, sizeof(line)), "partial");

    ASSERT_NULL(read_line(fp, line, sizeof(line)));
    fclose(fp);
    unlink(path);
}

static void test_slash_replacement_in_port_name(void)
{
    set_dir("slash");
    char path[600];
    expected_path(path, sizeof(path), "serial-by-id-usb-FTDI", TS_BASE);
    unlink(path);

    sm_text_log_t *log = sm_text_log_open(g_dir, "/dev/serial/by-id/usb-FTDI");
    ASSERT_NOT_NULL(log);
    if (!log) return;

    sm_text_log_write(log, (const uint8_t *)"x\n", 2, TS_BASE);
    sm_text_log_close(log);

    struct stat st;
    ASSERT_INT_EQ(stat(path, &st), 0);
    unlink(path);
}

static void test_day_rotation(void)
{
    set_dir("rotate");
    double day1 = TS_BASE;
    double day2 = TS_BASE + 2 * 86400;  /* two days later: new local day even
                                           across a DST-shifted 25h day */
    char path1[600], path2[600];
    expected_path(path1, sizeof(path1), "ttyACM7", day1);
    expected_path(path2, sizeof(path2), "ttyACM7", day2);
    unlink(path1);
    unlink(path2);

    sm_text_log_t *log = sm_text_log_open(g_dir, "/dev/ttyACM7");
    ASSERT_NOT_NULL(log);
    if (!log) return;

    sm_text_log_write(log, (const uint8_t *)"before\n", 7, day1);
    sm_text_log_write(log, (const uint8_t *)"after\n", 6, day2);
    sm_text_log_close(log);

    char stamp[32], line[256], want[300];
    FILE *fp = fopen(path1, "r");
    ASSERT_NOT_NULL(fp);
    if (fp) {
        expected_stamp(stamp, sizeof(stamp), day1);
        snprintf(want, sizeof(want), "%sbefore", stamp);
        ASSERT_STR_EQ(read_line(fp, line, sizeof(line)), want);
        ASSERT_NULL(read_line(fp, line, sizeof(line)));
        fclose(fp);
    }

    fp = fopen(path2, "r");
    ASSERT_NOT_NULL(fp);
    if (fp) {
        expected_stamp(stamp, sizeof(stamp), day2);
        snprintf(want, sizeof(want), "%safter", stamp);
        ASSERT_STR_EQ(read_line(fp, line, sizeof(line)), want);
        ASSERT_NULL(read_line(fp, line, sizeof(line)));
        fclose(fp);
    }
    unlink(path1);
    unlink(path2);
}

static void test_long_line_truncated(void)
{
    set_dir("longline");
    char path[600];
    expected_path(path, sizeof(path), "ttyUSB1", TS_BASE);
    unlink(path);

    sm_text_log_t *log = sm_text_log_open(g_dir, "ttyUSB1");
    ASSERT_NOT_NULL(log);
    if (!log) return;

    /* line_buf is 4096 → at most 4095 payload chars survive */
    size_t n = 5000;
    uint8_t *big = malloc(n + 1);
    ASSERT_NOT_NULL(big);
    if (big) {
        memset(big, 'A', n);
        big[n] = '\n';
        sm_text_log_write(log, big, n + 1, TS_BASE);
        free(big);
    }
    sm_text_log_close(log);

    FILE *fp = fopen(path, "r");
    ASSERT_NOT_NULL(fp);
    if (fp) {
        char *line = malloc(8192);
        ASSERT_NOT_NULL(line);
        if (line && fgets(line, 8192, fp)) {
            size_t stamp_len = strlen("[HH:MM:SS] ");
            ASSERT_INT_EQ((int)strcspn(line, "\n"), (int)(stamp_len + 4095));
        } else {
            ASSERT(0, "log line missing");
        }
        free(line);
        fclose(fp);
    }
    unlink(path);
}

static void test_null_safety(void)
{
    sm_text_log_write(NULL, (const uint8_t *)"x\n", 2, TS_BASE);
    sm_text_log_close(NULL);
    ASSERT(1, "null operations don't crash");
}

/* Regression: short sessions used to leave a 0-byte .log until process exit
 * because fflush waited for SM_TEXT_LOG_FLUSH_LINES (32). Dogfood sessions
 * often have fewer UART lines than that while JSONL already has content. */
static void test_flush_visible_without_close(void)
{
    set_dir("flushlive");
    char path[600];
    expected_path(path, sizeof(path), "ttyUSB9", TS_BASE);
    unlink(path);

    sm_text_log_t *log = sm_text_log_open(g_dir, "/dev/ttyUSB9");
    ASSERT_NOT_NULL(log);
    if (!log) return;

    sm_text_log_write(log, (const uint8_t *)"live-line\n", 10, TS_BASE);
    /* Do not close — file must already be readable on disk. */

    FILE *fp = fopen(path, "r");
    ASSERT_NOT_NULL(fp);
    if (fp) {
        char stamp[32], line[256], want[300];
        expected_stamp(stamp, sizeof(stamp), TS_BASE);
        snprintf(want, sizeof(want), "%slive-line", stamp);
        ASSERT_STR_EQ(read_line(fp, line, sizeof(line)), want);
        fclose(fp);
    }

    sm_text_log_close(log);
    unlink(path);
}

int main(void)
{
    printf("test_text_log\n");

    RUN_TEST(test_filename_and_line_format);
    RUN_TEST(test_slash_replacement_in_port_name);
    RUN_TEST(test_day_rotation);
    RUN_TEST(test_long_line_truncated);
    RUN_TEST(test_null_safety);
    RUN_TEST(test_flush_visible_without_close);

    TEST_REPORT();
}
