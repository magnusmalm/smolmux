#include "test_main.h"
#include "util/mi_parse.h"
#include "util/json_helpers.h"

#include <string.h>

/* Convenience: parse a NUL-terminated line. */
static int parse(const char *line, sm_mi_record_t *out)
{
    return sm_mi_parse_line(line, strlen(line), out);
}

static void test_prompt(void)
{
    sm_mi_record_t r;
    ASSERT_INT_EQ(parse("(gdb)", &r), 0);
    ASSERT_INT_EQ(r.type, SM_MI_PROMPT);
    ASSERT_INT_EQ((int)r.token, -1);
    sm_mi_record_free(&r);

    /* trailing CRLF tolerated */
    ASSERT_INT_EQ(parse("(gdb)\r\n", &r), 0);
    ASSERT_INT_EQ(r.type, SM_MI_PROMPT);
    sm_mi_record_free(&r);
}

static void test_simple_result(void)
{
    sm_mi_record_t r;
    ASSERT_INT_EQ(parse("^done", &r), 0);
    ASSERT_INT_EQ(r.type, SM_MI_RESULT);
    ASSERT_STR_EQ(r.class_, "done");
    ASSERT_INT_EQ((int)r.token, -1);
    ASSERT_NOT_NULL(r.results);           /* empty object, not NULL */
    sm_mi_record_free(&r);
}

static void test_tokened_result(void)
{
    sm_mi_record_t r;
    ASSERT_INT_EQ(parse("1234^running", &r), 0);
    ASSERT_INT_EQ(r.type, SM_MI_RESULT);
    ASSERT_STR_EQ(r.class_, "running");
    ASSERT_INT_EQ((int)r.token, 1234);
    sm_mi_record_free(&r);

    /* token "0" is a real token, distinct from "no token" (-1) */
    ASSERT_INT_EQ(parse("0^done", &r), 0);
    ASSERT_INT_EQ((int)r.token, 0);
    sm_mi_record_free(&r);
}

static void test_breakpoint_tuple(void)
{
    sm_mi_record_t r;
    const char *line =
        "12^done,bkpt={number=\"2\",type=\"breakpoint\",disp=\"keep\","
        "enabled=\"y\",addr=\"0x08000abc\",func=\"main\",file=\"main.c\","
        "line=\"42\",times=\"0\"}";
    ASSERT_INT_EQ(parse(line, &r), 0);
    ASSERT_INT_EQ((int)r.token, 12);
    ASSERT_STR_EQ(r.class_, "done");

    cJSON *bkpt = cJSON_GetObjectItem(r.results, "bkpt");
    ASSERT_NOT_NULL(bkpt);
    ASSERT_STR_EQ(sm_json_get_string(bkpt, "number"), "2");
    ASSERT_STR_EQ(sm_json_get_string(bkpt, "func"), "main");
    ASSERT_STR_EQ(sm_json_get_string(bkpt, "addr"), "0x08000abc");
    ASSERT_STR_EQ(sm_json_get_string(bkpt, "file"), "main.c");
    sm_mi_record_free(&r);
}

static void test_stack_list_of_tuples(void)
{
    sm_mi_record_t r;
    const char *line =
        "^done,stack=[frame={level=\"0\",addr=\"0x1000\",func=\"foo\","
        "file=\"a.c\",line=\"10\"},frame={level=\"1\",addr=\"0x1020\","
        "func=\"main\",file=\"a.c\",line=\"20\"}]";
    ASSERT_INT_EQ(parse(line, &r), 0);

    cJSON *stack = cJSON_GetObjectItem(r.results, "stack");
    ASSERT_NOT_NULL(stack);
    ASSERT(cJSON_IsArray(stack), "stack is array");
    ASSERT_INT_EQ(cJSON_GetArraySize(stack), 2);

    /* Each element is a one-key object {"frame": {...}} */
    cJSON *e0 = cJSON_GetArrayItem(stack, 0);
    cJSON *f0 = cJSON_GetObjectItem(e0, "frame");
    ASSERT_NOT_NULL(f0);
    ASSERT_STR_EQ(sm_json_get_string(f0, "func"), "foo");
    ASSERT_STR_EQ(sm_json_get_string(f0, "level"), "0");

    cJSON *e1 = cJSON_GetArrayItem(stack, 1);
    cJSON *f1 = cJSON_GetObjectItem(e1, "frame");
    ASSERT_STR_EQ(sm_json_get_string(f1, "func"), "main");
    sm_mi_record_free(&r);
}

static void test_register_values_plain_list(void)
{
    sm_mi_record_t r;
    const char *line =
        "^done,register-values=[{number=\"0\",value=\"0x0\"},"
        "{number=\"13\",value=\"0x20001000\"}]";
    ASSERT_INT_EQ(parse(line, &r), 0);
    cJSON *rv = cJSON_GetObjectItem(r.results, "register-values");
    ASSERT_NOT_NULL(rv);
    ASSERT_INT_EQ(cJSON_GetArraySize(rv), 2);
    cJSON *v1 = cJSON_GetArrayItem(rv, 1);
    ASSERT_STR_EQ(sm_json_get_string(v1, "number"), "13");
    ASSERT_STR_EQ(sm_json_get_string(v1, "value"), "0x20001000");
    sm_mi_record_free(&r);
}

static void test_empty_list(void)
{
    sm_mi_record_t r;
    ASSERT_INT_EQ(parse("^done,threads=[]", &r), 0);
    cJSON *th = cJSON_GetObjectItem(r.results, "threads");
    ASSERT_NOT_NULL(th);
    ASSERT(cJSON_IsArray(th), "empty threads is array");
    ASSERT_INT_EQ(cJSON_GetArraySize(th), 0);
    sm_mi_record_free(&r);
}

static void test_error_record(void)
{
    sm_mi_record_t r;
    ASSERT_INT_EQ(parse("^error,msg=\"No symbol \\\"foo\\\" in current context.\"", &r), 0);
    ASSERT_STR_EQ(r.class_, "error");
    /* The msg c-string has escaped quotes that must be unescaped. */
    ASSERT_STR_EQ(sm_json_get_string(r.results, "msg"),
                  "No symbol \"foo\" in current context.");
    sm_mi_record_free(&r);
}

static void test_exec_async_stopped(void)
{
    sm_mi_record_t r;
    const char *line =
        "*stopped,reason=\"breakpoint-hit\",disp=\"keep\",bkptno=\"1\","
        "frame={addr=\"0x08000abc\",func=\"main\",args=[],file=\"main.c\","
        "line=\"42\"},thread-id=\"1\",stopped-threads=\"all\"";
    ASSERT_INT_EQ(parse(line, &r), 0);
    ASSERT_INT_EQ(r.type, SM_MI_EXEC_ASYNC);
    ASSERT_STR_EQ(r.class_, "stopped");
    ASSERT_STR_EQ(sm_json_get_string(r.results, "reason"), "breakpoint-hit");
    cJSON *frame = cJSON_GetObjectItem(r.results, "frame");
    ASSERT_STR_EQ(sm_json_get_string(frame, "func"), "main");
    /* nested empty list args=[] */
    cJSON *args = cJSON_GetObjectItem(frame, "args");
    ASSERT(cJSON_IsArray(args) && cJSON_GetArraySize(args) == 0, "empty args list");
    sm_mi_record_free(&r);
}

static void test_exec_async_running(void)
{
    sm_mi_record_t r;
    ASSERT_INT_EQ(parse("*running,thread-id=\"all\"", &r), 0);
    ASSERT_INT_EQ(r.type, SM_MI_EXEC_ASYNC);
    ASSERT_STR_EQ(r.class_, "running");
    sm_mi_record_free(&r);
}

static void test_notify_async(void)
{
    sm_mi_record_t r;
    ASSERT_INT_EQ(parse("=thread-group-added,id=\"i1\"", &r), 0);
    ASSERT_INT_EQ(r.type, SM_MI_NOTIFY_ASYNC);
    ASSERT_STR_EQ(r.class_, "thread-group-added");
    ASSERT_STR_EQ(sm_json_get_string(r.results, "id"), "i1");
    sm_mi_record_free(&r);
}

static void test_console_stream(void)
{
    sm_mi_record_t r;
    ASSERT_INT_EQ(parse("~\"Reading symbols from a.out...\\n\"", &r), 0);
    ASSERT_INT_EQ(r.type, SM_MI_CONSOLE);
    ASSERT_STR_EQ(r.stream_data, "Reading symbols from a.out...\n");
    ASSERT_NULL(r.results);
    sm_mi_record_free(&r);
}

static void test_log_and_target_stream(void)
{
    sm_mi_record_t r;
    ASSERT_INT_EQ(parse("&\"warning: bad\\n\"", &r), 0);
    ASSERT_INT_EQ(r.type, SM_MI_LOG);
    ASSERT_STR_EQ(r.stream_data, "warning: bad\n");
    sm_mi_record_free(&r);

    ASSERT_INT_EQ(parse("@\"target says hi\\n\"", &r), 0);
    ASSERT_INT_EQ(r.type, SM_MI_TARGET);
    ASSERT_STR_EQ(r.stream_data, "target says hi\n");
    sm_mi_record_free(&r);
}

static void test_non_mi_returns_error(void)
{
    sm_mi_record_t r;
    /* Plain program output on the console (not an MI record). */
    ASSERT_INT_EQ(parse("hello world from firmware", &r), -1);
    /* Digits then a stream char are not a valid record. */
    ASSERT_INT_EQ(parse("5~\"x\"", &r), -1);
    /* Bare digits, no type char. */
    ASSERT_INT_EQ(parse("42", &r), -1);
    /* Empty line. */
    ASSERT_INT_EQ(parse("", &r), -1);
}

static void test_memory_bytes(void)
{
    sm_mi_record_t r;
    const char *line =
        "^done,memory=[{begin=\"0xe000ed28\",offset=\"0x00000000\","
        "end=\"0xe000ed2c\",contents=\"00000082\"}]";
    ASSERT_INT_EQ(parse(line, &r), 0);
    cJSON *mem = cJSON_GetObjectItem(r.results, "memory");
    ASSERT_INT_EQ(cJSON_GetArraySize(mem), 1);
    cJSON *region = cJSON_GetArrayItem(mem, 0);
    ASSERT_STR_EQ(sm_json_get_string(region, "begin"), "0xe000ed28");
    ASSERT_STR_EQ(sm_json_get_string(region, "contents"), "00000082");
    sm_mi_record_free(&r);
}

static void test_string_with_commas_and_braces(void)
{
    sm_mi_record_t r;
    /* A quoted value containing the very delimiters the parser scans for must
     * be consumed whole, not split. */
    ASSERT_INT_EQ(parse("^done,value=\"{a, b, c}\"", &r), 0);
    ASSERT_STR_EQ(sm_json_get_string(r.results, "value"), "{a, b, c}");
    sm_mi_record_free(&r);
}

int main(void)
{
    printf("test_mi_parse\n");

    RUN_TEST(test_prompt);
    RUN_TEST(test_simple_result);
    RUN_TEST(test_tokened_result);
    RUN_TEST(test_breakpoint_tuple);
    RUN_TEST(test_stack_list_of_tuples);
    RUN_TEST(test_register_values_plain_list);
    RUN_TEST(test_empty_list);
    RUN_TEST(test_error_record);
    RUN_TEST(test_exec_async_stopped);
    RUN_TEST(test_exec_async_running);
    RUN_TEST(test_notify_async);
    RUN_TEST(test_console_stream);
    RUN_TEST(test_log_and_target_stream);
    RUN_TEST(test_non_mi_returns_error);
    RUN_TEST(test_memory_bytes);
    RUN_TEST(test_string_with_commas_and_braces);

    TEST_REPORT();
}
