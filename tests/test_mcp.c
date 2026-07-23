#include "test_main.h"
#include "broker.h"
#include "links/uart.h"
#include "protocol.h"
#include "sinks/mcp.h"
#include "util/json_helpers.h"
#include "cJSON.h"

#include <pthread.h>
#include <unistd.h>
#include <pty.h>
#include <string.h>
#include <fcntl.h>

#define TEST_SOCK "/tmp/smolmux-test-mcp.sock"
#define STARTUP_DELAY 150000  /* 150ms */

/* --- Helpers --- */

static void *broker_thread(void *arg)
{
    sm_broker_t *b = arg;
    sm_broker_run(b);
    return NULL;
}

/* Read a line from a pipe fd (with timeout via polling) */
static int read_line(int fd, char *buf, size_t buf_size)
{
    size_t total = 0;
    for (int attempts = 0; attempts < 100; attempts++) {
        ssize_t n = read(fd, buf + total, buf_size - total - 1);
        if (n > 0) {
            total += (size_t)n;
            buf[total] = '\0';
            if (memchr(buf, '\n', total))
                return (int)total;
        }
        usleep(10000);
    }
    buf[total] = '\0';
    return (int)total;
}

/* Send a JSON-RPC request via pipe */
static void send_jsonrpc(int fd, cJSON *msg)
{
    char *str = cJSON_PrintUnformatted(msg);
    if (!str) return;
    size_t len = strlen(str);
    write(fd, str, len);
    write(fd, "\n", 1);
    free(str);
    cJSON_Delete(msg);
}

/* Build a JSON-RPC request */
static cJSON *jsonrpc_request(int id, const char *method, cJSON *params)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(msg, "id", id);
    cJSON_AddStringToObject(msg, "method", method);
    if (params)
        cJSON_AddItemToObject(msg, "params", params);
    else
        cJSON_AddItemToObject(msg, "params", cJSON_CreateObject());
    return msg;
}

typedef struct test_ctx {
    int master;
    int slave;
    sm_broker_t broker;
    sm_link_t *link;
    pthread_t tid;
    int mcp_stdin_read;   /* pipe: broker reads from this */
    int mcp_stdin_write;  /* test writes to this */
    int mcp_stdout_read;  /* test reads from this */
    int mcp_stdout_write; /* broker writes to this */
    int saved_stdin;
    int saved_stdout;
} test_ctx_t;

static void setup(test_ctx_t *ctx)
{
    openpty(&ctx->master, &ctx->slave, NULL, NULL, NULL);
    char *slave_name = ttyname(ctx->slave);

    ctx->link = sm_uart_new(slave_name, 115200, 0);
    sm_broker_init(&ctx->broker, ctx->link, TEST_SOCK);
    snprintf(ctx->broker.port, sizeof(ctx->broker.port), "%s", slave_name);
    ctx->broker.baudrate = 115200;
    sm_profile_init_default(&ctx->broker.profile);

    /* Create pipes for stdin/stdout redirection */
    int stdin_pipe[2], stdout_pipe[2];
    pipe(stdin_pipe);
    pipe(stdout_pipe);

    ctx->mcp_stdin_read = stdin_pipe[0];
    ctx->mcp_stdin_write = stdin_pipe[1];
    ctx->mcp_stdout_read = stdout_pipe[0];
    ctx->mcp_stdout_write = stdout_pipe[1];

    /* Set read end non-blocking for test reads */
    int flags = fcntl(ctx->mcp_stdout_read, F_GETFL, 0);
    fcntl(ctx->mcp_stdout_read, F_SETFL, flags | O_NONBLOCK);

    /* Redirect stdin/stdout for the MCP sink */
    ctx->saved_stdin = dup(STDIN_FILENO);
    ctx->saved_stdout = dup(STDOUT_FILENO);
    dup2(ctx->mcp_stdin_read, STDIN_FILENO);
    dup2(ctx->mcp_stdout_write, STDOUT_FILENO);

    /* Create and register MCP sink */
    sm_sink_t *mcp = sm_mcp_sink_new(&ctx->broker);
    sm_broker_add_sink(&ctx->broker, mcp);

    pthread_create(&ctx->tid, NULL, broker_thread, &ctx->broker);
    usleep(STARTUP_DELAY);
}

static void teardown(test_ctx_t *ctx)
{
    /* Close write end of stdin pipe to signal EOF */
    close(ctx->mcp_stdin_write);
    usleep(50000);

    sm_broker_stop(&ctx->broker);
    pthread_join(ctx->tid, NULL);
    sm_broker_destroy(&ctx->broker);

    /* Restore stdin/stdout */
    dup2(ctx->saved_stdin, STDIN_FILENO);
    dup2(ctx->saved_stdout, STDOUT_FILENO);
    close(ctx->saved_stdin);
    close(ctx->saved_stdout);

    close(ctx->mcp_stdin_read);
    close(ctx->mcp_stdout_read);
    close(ctx->mcp_stdout_write);
    close(ctx->master);
    close(ctx->slave);
}

/* --- Tests --- */

static void test_initialize(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    /* Send initialize request */
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "protocolVersion", "2024-11-05");
    cJSON *caps = cJSON_CreateObject();
    cJSON_AddItemToObject(params, "capabilities", caps);
    cJSON *client_info = cJSON_CreateObject();
    cJSON_AddStringToObject(client_info, "name", "test");
    cJSON_AddStringToObject(client_info, "version", "1.0");
    cJSON_AddItemToObject(params, "clientInfo", client_info);

    send_jsonrpc(ctx.mcp_stdin_write, jsonrpc_request(1, "initialize", params));
    usleep(100000);

    /* Read response */
    char buf[4096];
    int n = read_line(ctx.mcp_stdout_read, buf, sizeof(buf));
    ASSERT(n > 0, "got initialize response");

    cJSON *resp = cJSON_Parse(buf);
    ASSERT_NOT_NULL(resp);

    cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
    ASSERT_NOT_NULL(result);

    const char *version = sm_json_get_string(result, "protocolVersion");
    ASSERT_NOT_NULL(version);
    ASSERT_STR_EQ(version, "2024-11-05");

    cJSON *info = cJSON_GetObjectItemCaseSensitive(result, "serverInfo");
    ASSERT_NOT_NULL(info);
    ASSERT_STR_EQ(sm_json_get_string(info, "name"), "smolmux");

    cJSON_Delete(resp);
    teardown(&ctx);
}

static void test_tools_list(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    /* Initialize first */
    cJSON *init_params = cJSON_CreateObject();
    cJSON_AddStringToObject(init_params, "protocolVersion", "2024-11-05");
    send_jsonrpc(ctx.mcp_stdin_write, jsonrpc_request(1, "initialize", init_params));
    usleep(100000);

    /* Drain initialize response */
    char buf[16384];
    read_line(ctx.mcp_stdout_read, buf, sizeof(buf));

    /* Send tools/list */
    send_jsonrpc(ctx.mcp_stdin_write, jsonrpc_request(2, "tools/list", NULL));
    usleep(100000);

    int n = read_line(ctx.mcp_stdout_read, buf, sizeof(buf));
    ASSERT(n > 0, "got tools/list response");

    cJSON *resp = cJSON_Parse(buf);
    ASSERT_NOT_NULL(resp);

    cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
    ASSERT_NOT_NULL(result);

    cJSON *tools = cJSON_GetObjectItemCaseSensitive(result, "tools");
    ASSERT(cJSON_IsArray(tools), "tools is array");
    ASSERT(cJSON_GetArraySize(tools) == 16, "16 tools registered");

    cJSON_Delete(resp);
    teardown(&ctx);
}

static void test_serial_read(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    /* Initialize */
    cJSON *init_params = cJSON_CreateObject();
    cJSON_AddStringToObject(init_params, "protocolVersion", "2024-11-05");
    send_jsonrpc(ctx.mcp_stdin_write, jsonrpc_request(1, "initialize", init_params));
    usleep(100000);
    char buf[8192];
    read_line(ctx.mcp_stdout_read, buf, sizeof(buf));

    /* Write some data from device */
    write(ctx.master, "device output\n", 14);
    usleep(100000);

    /* Call serial_read */
    cJSON *call_params = cJSON_CreateObject();
    cJSON_AddStringToObject(call_params, "name", "serial_read");
    cJSON_AddItemToObject(call_params, "arguments", cJSON_CreateObject());
    send_jsonrpc(ctx.mcp_stdin_write, jsonrpc_request(3, "tools/call", call_params));
    usleep(100000);

    int n = read_line(ctx.mcp_stdout_read, buf, sizeof(buf));
    ASSERT(n > 0, "got serial_read response");

    cJSON *resp = cJSON_Parse(buf);
    ASSERT_NOT_NULL(resp);

    cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
    ASSERT_NOT_NULL(result);
    cJSON *content = cJSON_GetObjectItemCaseSensitive(result, "content");
    ASSERT(cJSON_IsArray(content), "content is array");
    cJSON *first = cJSON_GetArrayItem(content, 0);
    ASSERT_NOT_NULL(first);
    const char *text = sm_json_get_string(first, "text");
    ASSERT_NOT_NULL(text);
    ASSERT(strstr(text, "device output") != NULL, "output contains device data");

    cJSON_Delete(resp);
    teardown(&ctx);
}

static void test_serial_port_status(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    /* Initialize */
    cJSON *init_params = cJSON_CreateObject();
    cJSON_AddStringToObject(init_params, "protocolVersion", "2024-11-05");
    send_jsonrpc(ctx.mcp_stdin_write, jsonrpc_request(1, "initialize", init_params));
    usleep(100000);
    char buf[8192];
    read_line(ctx.mcp_stdout_read, buf, sizeof(buf));

    /* Call serial_port_status */
    cJSON *call_params = cJSON_CreateObject();
    cJSON_AddStringToObject(call_params, "name", "serial_port_status");
    cJSON_AddItemToObject(call_params, "arguments", cJSON_CreateObject());
    send_jsonrpc(ctx.mcp_stdin_write, jsonrpc_request(2, "tools/call", call_params));
    usleep(100000);

    int n = read_line(ctx.mcp_stdout_read, buf, sizeof(buf));
    ASSERT(n > 0, "got status response");

    cJSON *resp = cJSON_Parse(buf);
    ASSERT_NOT_NULL(resp);

    cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
    ASSERT_NOT_NULL(result);
    cJSON *content = cJSON_GetObjectItemCaseSensitive(result, "content");
    cJSON *first = cJSON_GetArrayItem(content, 0);
    const char *text = sm_json_get_string(first, "text");
    ASSERT_NOT_NULL(text);
    ASSERT(strstr(text, "Baud: 115200") != NULL, "status contains baud");

    cJSON_Delete(resp);
    teardown(&ctx);
}

static void test_serial_boot_status(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    /* Declare a boot pipeline before any device data flows. */
    sm_boot_add_stage(&ctx.broker.boot, "uboot", "U-Boot 20");
    sm_boot_add_stage(&ctx.broker.boot, "login", "login:");

    cJSON *init_params = cJSON_CreateObject();
    cJSON_AddStringToObject(init_params, "protocolVersion", "2024-11-05");
    send_jsonrpc(ctx.mcp_stdin_write, jsonrpc_request(1, "initialize", init_params));
    usleep(100000);
    char buf[8192];
    read_line(ctx.mcp_stdout_read, buf, sizeof(buf));

    /* Device reaches the U-Boot stage. */
    write(ctx.master, "U-Boot 2024.01\r\n", 16);
    usleep(100000);

    cJSON *call_params = cJSON_CreateObject();
    cJSON_AddStringToObject(call_params, "name", "serial_boot_status");
    cJSON_AddItemToObject(call_params, "arguments", cJSON_CreateObject());
    send_jsonrpc(ctx.mcp_stdin_write, jsonrpc_request(2, "tools/call", call_params));
    usleep(100000);

    int n = read_line(ctx.mcp_stdout_read, buf, sizeof(buf));
    ASSERT(n > 0, "got boot_status response");

    cJSON *resp = cJSON_Parse(buf);
    ASSERT_NOT_NULL(resp);
    cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
    ASSERT_NOT_NULL(result);
    cJSON *content = cJSON_GetObjectItemCaseSensitive(result, "content");
    cJSON *first = cJSON_GetArrayItem(content, 0);
    const char *text = sm_json_get_string(first, "text");
    ASSERT_NOT_NULL(text);
    ASSERT(strstr(text, "furthest: uboot") != NULL, "reports furthest stage");
    ASSERT(strstr(text, "in progress") != NULL, "reports in-progress state");
    ASSERT(strstr(text, "[x] uboot") != NULL, "uboot checked");
    ASSERT(strstr(text, "[ ] login") != NULL, "login unchecked");

    cJSON_Delete(resp);
    teardown(&ctx);
}

static void test_serial_add_autoresponder(void)
{
    test_ctx_t ctx;
    setup(&ctx);
    fcntl(ctx.master, F_SETFL, O_NONBLOCK);

    cJSON *init_params = cJSON_CreateObject();
    cJSON_AddStringToObject(init_params, "protocolVersion", "2024-11-05");
    send_jsonrpc(ctx.mcp_stdin_write, jsonrpc_request(1, "initialize", init_params));
    usleep(100000);
    char buf[8192];
    read_line(ctx.mcp_stdout_read, buf, sizeof(buf));

    /* Add a rule via the tool: on "[y/N]", send "y\n". */
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "name", "confirm");
    cJSON_AddStringToObject(args, "pattern", "\\[y/N\\]");
    cJSON_AddStringToObject(args, "send", "y\\n");
    cJSON *call_params = cJSON_CreateObject();
    cJSON_AddStringToObject(call_params, "name", "serial_add_autoresponder");
    cJSON_AddItemToObject(call_params, "arguments", args);
    send_jsonrpc(ctx.mcp_stdin_write, jsonrpc_request(2, "tools/call", call_params));
    usleep(100000);

    int n = read_line(ctx.mcp_stdout_read, buf, sizeof(buf));
    ASSERT(n > 0, "got add response");
    cJSON *resp = cJSON_Parse(buf);
    ASSERT_NOT_NULL(resp);
    cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
    cJSON *content = result ? cJSON_GetObjectItemCaseSensitive(result, "content") : NULL;
    cJSON *first = content ? cJSON_GetArrayItem(content, 0) : NULL;
    const char *text = first ? sm_json_get_string(first, "text") : NULL;
    ASSERT(text && strstr(text, "confirm") != NULL, "tool confirms the rule");
    if (resp) cJSON_Delete(resp);

    /* Device prints the prompt; the broker must auto-send "y\n". */
    write(ctx.master, "Proceed? [y/N] ", 15);
    usleep(100000);
    char dev[64];
    ssize_t rn = read(ctx.master, dev, sizeof(dev));
    int saw_y = 0;
    for (ssize_t i = 0; i < rn; i++)
        if (dev[i] == 'y') saw_y = 1;
    ASSERT(saw_y, "auto-response written to device");

    teardown(&ctx);
}

static void test_serial_send_command(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    /* Initialize */
    cJSON *init_params = cJSON_CreateObject();
    cJSON_AddStringToObject(init_params, "protocolVersion", "2024-11-05");
    send_jsonrpc(ctx.mcp_stdin_write, jsonrpc_request(1, "initialize", init_params));
    usleep(100000);
    char buf[8192];
    read_line(ctx.mcp_stdout_read, buf, sizeof(buf));

    /* Call serial_send_command with explicit pattern */
    cJSON *call_params = cJSON_CreateObject();
    cJSON_AddStringToObject(call_params, "name", "serial_send_command");
    cJSON *tool_args = cJSON_CreateObject();
    cJSON_AddStringToObject(tool_args, "command", "test_cmd");
    cJSON_AddStringToObject(tool_args, "expect_pattern", "done");
    cJSON_AddNumberToObject(tool_args, "timeout_ms", 2000);
    cJSON_AddItemToObject(call_params, "arguments", tool_args);
    send_jsonrpc(ctx.mcp_stdin_write, jsonrpc_request(3, "tools/call", call_params));

    /* Read and echo command from device side */
    usleep(50000);
    char dev_buf[256];
    read(ctx.master, dev_buf, sizeof(dev_buf));

    /* Device sends matching response */
    write(ctx.master, "output done\n", 12);
    usleep(200000);

    /* Read response */
    int n = read_line(ctx.mcp_stdout_read, buf, sizeof(buf));
    ASSERT(n > 0, "got send_command response");

    cJSON *resp = cJSON_Parse(buf);
    ASSERT_NOT_NULL(resp);

    cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
    ASSERT_NOT_NULL(result);
    cJSON *content = cJSON_GetObjectItemCaseSensitive(result, "content");
    cJSON *first = cJSON_GetArrayItem(content, 0);
    const char *text = sm_json_get_string(first, "text");
    ASSERT_NOT_NULL(text);
    ASSERT(strstr(text, "done") != NULL, "response contains matched output");

    cJSON_Delete(resp);
    teardown(&ctx);
}

static void test_method_not_found(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    send_jsonrpc(ctx.mcp_stdin_write,
                  jsonrpc_request(1, "nonexistent/method", NULL));
    usleep(100000);

    char buf[4096];
    int n = read_line(ctx.mcp_stdout_read, buf, sizeof(buf));
    ASSERT(n > 0, "got error response");

    cJSON *resp = cJSON_Parse(buf);
    ASSERT_NOT_NULL(resp);

    cJSON *err = cJSON_GetObjectItemCaseSensitive(resp, "error");
    ASSERT_NOT_NULL(err);
    ASSERT_INT_EQ(sm_json_get_int(err, "code", 0), -32601);

    cJSON_Delete(resp);
    teardown(&ctx);
}

int main(void)
{
    printf("test_mcp\n");

    RUN_TEST(test_initialize);
    RUN_TEST(test_tools_list);
    RUN_TEST(test_serial_read);
    RUN_TEST(test_serial_port_status);
    RUN_TEST(test_serial_boot_status);
    RUN_TEST(test_serial_add_autoresponder);
    RUN_TEST(test_serial_send_command);
    RUN_TEST(test_method_not_found);

    TEST_REPORT();
}
