#include "test_main.h"
#include "io_log.h"

#include <unistd.h>

static void test_create_and_write(void)
{
    const char *tmp = getenv("TMPDIR");
    char path[512];
    snprintf(path, sizeof(path), "%s/smolmux-test-io.jsonl",
             tmp && tmp[0] ? tmp : "/tmp");
    unlink(path);

    sm_io_log_t *log = sm_io_log_open(path);
    ASSERT_NOT_NULL(log);

    sm_io_log_output(log, (const uint8_t *)"hello\n", 6, 1234567890.123);
    sm_io_log_input(log, (const uint8_t *)"cmd\n", 4, "test-client", 1234567891.0);
    sm_io_log_incident(log, "inc-001", "kernel_panic", "critical", 1234567892.0);

    sm_io_log_close(log);

    /* Verify file content */
    FILE *fp = fopen(path, "r");
    ASSERT_NOT_NULL(fp);
    if (fp) {
        char line[1024];
        /* Line 1: output */
        ASSERT_NOT_NULL(fgets(line, sizeof(line), fp));
        ASSERT(strstr(line, "\"type\":\"output\"") != NULL, "output line");
        ASSERT(strstr(line, "\"timestamp\":") != NULL, "has timestamp");
        ASSERT(strstr(line, "\"data\":") != NULL, "has data");

        /* Line 2: input */
        ASSERT_NOT_NULL(fgets(line, sizeof(line), fp));
        ASSERT(strstr(line, "\"type\":\"input\"") != NULL, "input line");
        ASSERT(strstr(line, "\"sender\":\"test-client\"") != NULL, "has sender");

        /* Line 3: incident */
        ASSERT_NOT_NULL(fgets(line, sizeof(line), fp));
        ASSERT(strstr(line, "\"type\":\"incident\"") != NULL, "incident line");
        ASSERT(strstr(line, "\"pattern_name\":\"kernel_panic\"") != NULL, "has pattern");

        fclose(fp);
    }
    unlink(path);
}

static void test_null_log(void)
{
    /* Should not crash with NULL log */
    sm_io_log_output(NULL, (const uint8_t *)"test", 4, 0.0);
    sm_io_log_input(NULL, (const uint8_t *)"test", 4, "x", 0.0);
    sm_io_log_incident(NULL, "id", "pat", "sev", 0.0);
    sm_io_log_close(NULL);
    ASSERT(1, "null operations don't crash");
}

int main(void)
{
    printf("test_io_log\n");

    RUN_TEST(test_create_and_write);
    RUN_TEST(test_null_log);

    TEST_REPORT();
}
