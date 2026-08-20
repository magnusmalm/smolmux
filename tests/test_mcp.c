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
#include <stdlib.h>

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

    /* Wave 1 P0a: instructions on initialize */
    const char *ins = sm_json_get_string(result, "instructions");
    ASSERT_NOT_NULL(ins);
    ASSERT(strlen(ins) >= 200, "instructions length >= 200");
    ASSERT(strstr(ins, "serial_suspend") != NULL, "instructions: suspend");
    ASSERT(strstr(ins, "serial_get_incidents") != NULL,
           "instructions: incidents");

    cJSON *caps_out = cJSON_GetObjectItemCaseSensitive(result, "capabilities");
    ASSERT_NOT_NULL(caps_out);
    ASSERT(cJSON_GetObjectItemCaseSensitive(caps_out, "prompts") != NULL,
           "capabilities.prompts advertised");

    cJSON_Delete(resp);
    teardown(&ctx);
}

static void test_prompts_list_and_get(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    cJSON *init_params = cJSON_CreateObject();
    cJSON_AddStringToObject(init_params, "protocolVersion", "2024-11-05");
    send_jsonrpc(ctx.mcp_stdin_write, jsonrpc_request(1, "initialize",
                                                       init_params));
    usleep(100000);
    char buf[16384];
    read_line(ctx.mcp_stdout_read, buf, sizeof(buf));

    /* prompts/list — exact three names */
    send_jsonrpc(ctx.mcp_stdin_write, jsonrpc_request(2, "prompts/list", NULL));
    usleep(100000);
    int n = read_line(ctx.mcp_stdout_read, buf, sizeof(buf));
    ASSERT(n > 0, "got prompts/list response");
    cJSON *resp = cJSON_Parse(buf);
    ASSERT_NOT_NULL(resp);
    cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
    cJSON *prompts = cJSON_GetObjectItemCaseSensitive(result, "prompts");
    ASSERT(cJSON_IsArray(prompts), "prompts array");
    ASSERT_INT_EQ(cJSON_GetArraySize(prompts), 3);
    int saw_b = 0, saw_d = 0, saw_f = 0;
    cJSON *p;
    cJSON_ArrayForEach(p, prompts) {
        const char *name = sm_json_get_string(p, "name");
        if (name && strcmp(name, "bringup") == 0) saw_b = 1;
        else if (name && strcmp(name, "debug_serial") == 0) saw_d = 1;
        else if (name && strcmp(name, "flash_safe") == 0) saw_f = 1;
    }
    ASSERT(saw_b && saw_d && saw_f, "exact prompt set on sink");
    cJSON_Delete(resp);

    /* prompts/get flash_safe */
    cJSON *get_params = cJSON_CreateObject();
    cJSON_AddStringToObject(get_params, "name", "flash_safe");
    send_jsonrpc(ctx.mcp_stdin_write,
                 jsonrpc_request(3, "prompts/get", get_params));
    usleep(100000);
    n = read_line(ctx.mcp_stdout_read, buf, sizeof(buf));
    ASSERT(n > 0, "got prompts/get response");
    resp = cJSON_Parse(buf);
    ASSERT_NOT_NULL(resp);
    result = cJSON_GetObjectItemCaseSensitive(resp, "result");
    cJSON *msgs = cJSON_GetObjectItemCaseSensitive(result, "messages");
    ASSERT(cJSON_IsArray(msgs) && cJSON_GetArraySize(msgs) >= 1, "messages");
    cJSON *msg0 = cJSON_GetArrayItem(msgs, 0);
    cJSON *content = cJSON_GetObjectItemCaseSensitive(msg0, "content");
    const char *text = sm_json_get_string(content, "text");
    ASSERT(text && strstr(text, "serial_suspend") != NULL,
           "flash_safe cites serial_suspend");
    ASSERT(text && strstr(text, "serial_boot_status") != NULL,
           "flash_safe cites serial_boot_status");
    cJSON_Delete(resp);

    /* unknown prompt -> error */
    cJSON *bad = cJSON_CreateObject();
    cJSON_AddStringToObject(bad, "name", "not_a_prompt");
    send_jsonrpc(ctx.mcp_stdin_write, jsonrpc_request(4, "prompts/get", bad));
    usleep(100000);
    n = read_line(ctx.mcp_stdout_read, buf, sizeof(buf));
    ASSERT(n > 0, "got error response");
    resp = cJSON_Parse(buf);
    ASSERT_NOT_NULL(resp);
    ASSERT(cJSON_GetObjectItemCaseSensitive(resp, "error") != NULL,
           "unknown prompt is error");
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
    ASSERT(cJSON_GetArraySize(tools) == 17, "17 tools registered");

    /* Wave 4: annotations on at least one RO and one destructive tool */
    int saw_ro = 0, saw_destr = 0;
    cJSON *t;
    cJSON_ArrayForEach(t, tools) {
        cJSON *ann = cJSON_GetObjectItemCaseSensitive(t, "annotations");
        if (!cJSON_IsObject(ann)) continue;
        cJSON *ro = cJSON_GetObjectItemCaseSensitive(ann, "readOnlyHint");
        cJSON *de = cJSON_GetObjectItemCaseSensitive(ann, "destructiveHint");
        if (cJSON_IsTrue(ro)) saw_ro = 1;
        if (cJSON_IsTrue(de)) saw_destr = 1;
    }
    ASSERT(saw_ro, "readOnlyHint on some tool");
    ASSERT(saw_destr, "destructiveHint on some tool");

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

/* SM-15 was closed on the wire path only; the in-process MCP sink used to
 * forward any pin key to link->set_param (allow_shell, target, …). */
static void test_serial_pin_control_rejects_non_pin_keys(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    static const char *const bad[] = {
        "allow_shell", "target", "baud", "parity", "flow_control",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        cJSON *args = cJSON_CreateObject();
        cJSON_AddStringToObject(args, "pin", bad[i]);
        cJSON_AddStringToObject(args, "action", "1");
        cJSON *params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "name", "serial_pin_control");
        cJSON_AddItemToObject(params, "arguments", args);
        send_jsonrpc(ctx.mcp_stdin_write,
                      jsonrpc_request((int)(100 + i), "tools/call", params));
        usleep(100000);

        char buf[4096];
        int n = read_line(ctx.mcp_stdout_read, buf, sizeof(buf));
        ASSERT(n > 0, "got pin_control response");
        cJSON *resp = cJSON_Parse(buf);
        ASSERT_NOT_NULL(resp);
        cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
        ASSERT_NOT_NULL(result);
        cJSON *content = cJSON_GetObjectItemCaseSensitive(result, "content");
        cJSON *first = cJSON_GetArrayItem(content, 0);
        const char *text = sm_json_get_string(first, "text");
        ASSERT_NOT_NULL(text);
        ASSERT(strstr(text, "unknown pin") != NULL,
               "rejects non-line-control pin key");
        cJSON_Delete(resp);
    }

    teardown(&ctx);
}

/* REL-MCP-4K: oversized command is fail-closed; PTY must not see a clip. */
static void test_serial_send_command_too_long(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    cJSON *init_params = cJSON_CreateObject();
    cJSON_AddStringToObject(init_params, "protocolVersion", "2024-11-05");
    send_jsonrpc(ctx.mcp_stdin_write, jsonrpc_request(1, "initialize",
                                                      init_params));
    usleep(100000);
    char buf[8192];
    read_line(ctx.mcp_stdout_read, buf, sizeof(buf));

    char *long_cmd = malloc(5001);
    ASSERT_NOT_NULL(long_cmd);
    memset(long_cmd, 'A', 5000);
    long_cmd[5000] = '\0';

    cJSON *call_params = cJSON_CreateObject();
    cJSON_AddStringToObject(call_params, "name", "serial_send_command");
    cJSON *tool_args = cJSON_CreateObject();
    cJSON_AddStringToObject(tool_args, "command", long_cmd);
    cJSON_AddStringToObject(tool_args, "expect_pattern", "x");
    cJSON_AddNumberToObject(tool_args, "timeout_ms", 200);
    cJSON_AddItemToObject(call_params, "arguments", tool_args);
    send_jsonrpc(ctx.mcp_stdin_write, jsonrpc_request(4, "tools/call",
                                                      call_params));
    usleep(100000);

    int flags = fcntl(ctx.master, F_GETFL, 0);
    fcntl(ctx.master, F_SETFL, flags | O_NONBLOCK);
    char dev[256];
    ssize_t dn = read(ctx.master, dev, sizeof(dev) - 1);
    if (dn < 0)
        dn = 0;
    dev[dn] = '\0';

    int n = read_line(ctx.mcp_stdout_read, buf, sizeof(buf));
    ASSERT(n > 0, "got too-long response");
    ASSERT(strstr(buf, "too long") != NULL, "fail-closed on 4k overflow");
    ASSERT(strstr(dev, "AAAA") == NULL, "partial command not sent");

    free(long_cmd);
    teardown(&ctx);
}

/* REL-CR: eol=cr writes CR, not LF. */
static void test_serial_send_command_eol_cr(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    cJSON *init_params = cJSON_CreateObject();
    cJSON_AddStringToObject(init_params, "protocolVersion", "2024-11-05");
    send_jsonrpc(ctx.mcp_stdin_write, jsonrpc_request(1, "initialize",
                                                      init_params));
    usleep(100000);
    char buf[8192];
    read_line(ctx.mcp_stdout_read, buf, sizeof(buf));

    cJSON *call_params = cJSON_CreateObject();
    cJSON_AddStringToObject(call_params, "name", "serial_send_command");
    cJSON *tool_args = cJSON_CreateObject();
    cJSON_AddStringToObject(tool_args, "command", "ver");
    cJSON_AddStringToObject(tool_args, "eol", "cr");
    cJSON_AddStringToObject(tool_args, "expect_pattern", "never-match-xx");
    cJSON_AddNumberToObject(tool_args, "timeout_ms", 200);
    cJSON_AddItemToObject(call_params, "arguments", tool_args);
    send_jsonrpc(ctx.mcp_stdin_write, jsonrpc_request(5, "tools/call",
                                                      call_params));
    usleep(80000);

    char dev[64];
    memset(dev, 0, sizeof(dev));
    ssize_t dn = read(ctx.master, dev, sizeof(dev) - 1);
    if (dn < 0)
        dn = 0;
    int saw_cr = 0, saw_lf = 0;
    for (ssize_t i = 0; i < dn; i++) {
        if (dev[i] == '\r')
            saw_cr = 1;
        if (dev[i] == '\n')
            saw_lf = 1;
    }
    ASSERT(saw_cr, "eol=cr writes CR to the device");
    ASSERT(!saw_lf, "eol=cr must not write LF");

    teardown(&ctx);
}

int main(void)
{
    setenv("SMOLMUX_MCP_MUTATE", "1", 1);
    printf("test_mcp\n");

    RUN_TEST(test_initialize);
    RUN_TEST(test_prompts_list_and_get);
    RUN_TEST(test_tools_list);
    RUN_TEST(test_serial_read);
    RUN_TEST(test_serial_port_status);
    RUN_TEST(test_serial_boot_status);
    RUN_TEST(test_serial_add_autoresponder);
    RUN_TEST(test_serial_send_command);
    RUN_TEST(test_serial_send_command_too_long);
    RUN_TEST(test_serial_send_command_eol_cr);
    RUN_TEST(test_method_not_found);
    RUN_TEST(test_serial_pin_control_rejects_non_pin_keys);

    TEST_REPORT();
}
