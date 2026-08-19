/* End-to-end smoke test for the standalone smolmux-mcp binary: a real broker
 * (PTY-backed UART link) runs in-process, the actual smolmux-mcp executable
 * is spawned with pipes on stdin/stdout, and MCP JSON-RPC is driven over
 * those pipes — initialize, tools/list, and tool calls that round-trip
 * through the broker to the fake device and back. The binary path comes in
 * as argv[1] (CMake passes $<TARGET_FILE:smolmux-mcp>). */
#include "test_main.h"
#include "broker.h"
#include "links/uart.h"
#include "protocol.h"
#include "constants.h"
#include "util/json_helpers.h"

#include <pthread.h>
#include <unistd.h>
#include <pty.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

#define TEST_SOCK "/tmp/smolmux-test-mcpc.sock"
#define STARTUP_DELAY 150000  /* 150ms */

static const char *g_mcp_bin;

/* --- Broker fixture (pattern from test_broker.c) --- */

static void *broker_thread(void *arg)
{
    sm_broker_t *b = arg;
    sm_broker_run(b);
    return NULL;
}

typedef struct fixture {
    int master;
    int slave;
    sm_broker_t broker;
    sm_link_t *link;
    pthread_t tid;

    pid_t mcp_pid;
    int mcp_in;    /* write end -> mcp stdin */
    int mcp_out;   /* read end <- mcp stdout */
    char buf[65536];
    size_t len;
} fixture_t;

static void spawn_mcp(fixture_t *fx)
{
    int to_child[2], from_child[2];
    pipe(to_child);
    pipe(from_child);

    fx->mcp_pid = fork();
    if (fx->mcp_pid == 0) {
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);

        /* Force the default device profile: no env override, and a HOME
         * with no ~/.config/smolmux/profiles to discover. */
        const char *tmp = getenv("TMPDIR");
        setenv("HOME", tmp && tmp[0] ? tmp : "/tmp", 1);
        unsetenv(SM_PROFILE_ENV);
        unsetenv(SM_SOCKET_ENV);

        execl(g_mcp_bin, g_mcp_bin, "-s", TEST_SOCK, "-n", "mcp-e2e", NULL);
        _exit(127);
    }

    close(to_child[0]);
    close(from_child[1]);
    fx->mcp_in = to_child[1];
    fx->mcp_out = from_child[0];

    int flags = fcntl(fx->mcp_out, F_GETFL, 0);
    fcntl(fx->mcp_out, F_SETFL, flags | O_NONBLOCK);
}

static void setup(fixture_t *fx)
{
    memset(fx, 0, sizeof(*fx));
    openpty(&fx->master, &fx->slave, NULL, NULL, NULL);
    char *slave_name = ttyname(fx->slave);

    fx->link = sm_uart_new(slave_name, 115200, 0);
    sm_broker_init(&fx->broker, fx->link, TEST_SOCK);
    snprintf(fx->broker.port, sizeof(fx->broker.port), "%s", slave_name);
    fx->broker.baudrate = 115200;

    pthread_create(&fx->tid, NULL, broker_thread, &fx->broker);
    usleep(STARTUP_DELAY);

    spawn_mcp(fx);
    usleep(STARTUP_DELAY);  /* mcp connects + hellos before first request */
}

static void teardown(fixture_t *fx)
{
    close(fx->mcp_in);   /* stdin EOF -> mcp exits */
    int status = -1;
    for (int i = 0; i < 200; i++) {
        if (waitpid(fx->mcp_pid, &status, WNOHANG) == fx->mcp_pid) break;
        usleep(10000);
    }
    if (status == -1) {
        kill(fx->mcp_pid, SIGKILL);
        waitpid(fx->mcp_pid, &status, 0);
    }
    ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "smolmux-mcp exited cleanly on stdin EOF");
    close(fx->mcp_out);

    sm_broker_stop(&fx->broker);
    pthread_join(fx->tid, NULL);
    sm_broker_destroy(&fx->broker);
    close(fx->master);
    close(fx->slave);
}

/* --- JSON-RPC over the pipe --- */

static void rpc_send(fixture_t *fx, const char *json_line)
{
    write(fx->mcp_in, json_line, strlen(json_line));
    write(fx->mcp_in, "\n", 1);
}

/* Next complete line from mcp stdout (waits up to ~attempts*10ms). */
static char *next_line(fixture_t *fx, int attempts)
{
    static char line[32768];
    for (int i = 0; i < attempts; i++) {
        char *nl = memchr(fx->buf, '\n', fx->len);
        if (nl) {
            size_t full = (size_t)(nl - fx->buf);
            size_t n = full >= sizeof(line) ? sizeof(line) - 1 : full;
            memcpy(line, fx->buf, n);
            line[n] = '\0';
            memmove(fx->buf, nl + 1, fx->len - full - 1);
            fx->len -= full + 1;
            return line;
        }
        ssize_t r = read(fx->mcp_out, fx->buf + fx->len,
                         sizeof(fx->buf) - fx->len);
        if (r > 0)
            fx->len += (size_t)r;
        else
            usleep(10000);
    }
    return NULL;
}

/* Send a request and wait up to ~attempts*10ms for the response with
 * matching integer id. Caller must cJSON_Delete the result. */
static cJSON *rpc_call(fixture_t *fx, int id, const char *json_line,
                       int attempts)
{
    rpc_send(fx, json_line);
    for (int i = 0; i < 50; i++) {
        char *l = next_line(fx, attempts);
        if (!l) return NULL;
        cJSON *resp = cJSON_Parse(l);
        if (!resp) continue;
        cJSON *rid = cJSON_GetObjectItem(resp, "id");
        if (cJSON_IsNumber(rid) && (int)rid->valuedouble == id)
            return resp;
        cJSON_Delete(resp);
    }
    return NULL;
}

/* result.content[0].text of a tool-call response (NULL if malformed). */
static const char *tool_text(cJSON *resp)
{
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    cJSON *content = cJSON_GetObjectItem(result, "content");
    cJSON *item = cJSON_GetArrayItem(content, 0);
    return item ? sm_json_get_string(item, "text") : NULL;
}

/* --- Test --- */

static void test_mcp_e2e_smoke(void)
{
    fixture_t fx;
    setup(&fx);

    /* Declare a boot pipeline for the serial_boot_status check below. */
    sm_boot_add_stage(&fx.broker.boot, "uboot", "U-Boot 20");
    sm_boot_add_stage(&fx.broker.boot, "login", "login:");

    /* initialize — tight 2s budget: it must be answered promptly after the
     * hello/welcome handshake (a welcome-wait stall here is a bug; the
     * original wait_for_response(NULL) never matched the welcome and every
     * startup ate a 5s timeout). */
    cJSON *resp = rpc_call(&fx, 1,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}",
        200);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        cJSON *result = cJSON_GetObjectItem(resp, "result");
        ASSERT_STR_EQ(sm_json_get_string(result, "protocolVersion"),
                      SM_MCP_PROTOCOL_VERSION);
        cJSON *info = cJSON_GetObjectItem(result, "serverInfo");
        ASSERT_STR_EQ(sm_json_get_string(info, "name"), SM_NAME "-mcp");
        cJSON_Delete(resp);
    }
    rpc_send(&fx, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");

    /* tools/list */
    resp = rpc_call(&fx, 2,
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}", 500);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        cJSON *tools = cJSON_GetObjectItem(
            cJSON_GetObjectItem(resp, "result"), "tools");
        ASSERT(cJSON_IsArray(tools) && cJSON_GetArraySize(tools) > 0,
               "tools list non-empty");
        int found = 0;
        cJSON *t;
        cJSON_ArrayForEach(t, tools) {
            const char *n = sm_json_get_string(t, "name");
            if (n && strcmp(n, "serial_send_command") == 0) found = 1;
        }
        ASSERT(found, "serial_send_command advertised");
        cJSON_Delete(resp);
    }

    /* serial_write: MCP -> broker -> device */
    resp = rpc_call(&fx, 3,
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":"
        "{\"name\":\"serial_write\",\"arguments\":{\"data\":\"hello-mcp\\n\"}}}",
        500);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        const char *text = tool_text(resp);
        ASSERT_STR_EQ(text, "OK");
        cJSON_Delete(resp);
    }
    char devbuf[256] = {0};
    ssize_t n = 0;
    for (int i = 0; i < 100 && n <= 0; i++) {
        n = read(fx.master, devbuf, sizeof(devbuf) - 1);
        if (n <= 0) usleep(10000);
    }
    ASSERT(n > 0, "device received data");
    ASSERT(strstr(devbuf, "hello-mcp") != NULL, "device got serial_write payload");

    /* serial_read: device -> broker -> MCP output buffer */
    write(fx.master, "device-says-hi\n", 15);
    usleep(200000);  /* let output propagate broker -> mcp buffer */
    resp = rpc_call(&fx, 4,
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":"
        "{\"name\":\"serial_read\",\"arguments\":{}}}", 500);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        const char *text = tool_text(resp);
        ASSERT(text && strstr(text, "device-says-hi") != NULL,
               "serial_read returned device output");
        cJSON_Delete(resp);
    }

    /* serial_port_status: broker status round trip */
    resp = rpc_call(&fx, 5,
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":"
        "{\"name\":\"serial_port_status\",\"arguments\":{}}}", 500);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        const char *text = tool_text(resp);
        ASSERT(text && strstr(text, "Connected: true") != NULL,
               "port status reports connected");
        cJSON_Delete(resp);
    }

    /* serial_boot_status: device reaches U-Boot, tool reports the furthest stage */
    write(fx.master, "U-Boot 2024.01\r\n", 16);
    usleep(150000);
    resp = rpc_call(&fx, 7,
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":"
        "{\"name\":\"serial_boot_status\",\"arguments\":{}}}", 500);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        const char *text = tool_text(resp);
        ASSERT(text && strstr(text, "furthest: uboot") != NULL,
               "boot status reports furthest stage");
        cJSON_Delete(resp);
    }

    /* serial_add_autoresponder: registers a rule that reaches the broker */
    resp = rpc_call(&fx, 8,
        "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\",\"params\":"
        "{\"name\":\"serial_add_autoresponder\",\"arguments\":"
        "{\"name\":\"yn\",\"pattern\":\"\\\\[y/N\\\\]\",\"send\":\"y\\\\n\"}}}", 500);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        const char *text = tool_text(resp);
        ASSERT(text && strstr(text, "added") != NULL, "autoresponder tool confirms add");
        cJSON_Delete(resp);
    }

    /* unknown method -> JSON-RPC error, not silence */
    resp = rpc_call(&fx, 6,
        "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"nope/nothing\"}", 500);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        cJSON *err = cJSON_GetObjectItem(resp, "error");
        ASSERT(err != NULL, "unknown method yields error response");
        ASSERT_INT_EQ(sm_json_get_int(err, "code", 0), -32601);
        cJSON_Delete(resp);
    }

    teardown(&fx);
}

int main(int argc, char *argv[])
{
    printf("test_mcp_client\n");
    signal(SIGPIPE, SIG_IGN);
    unlink(TEST_SOCK);

    if (argc < 2) {
        fprintf(stderr, "usage: %s <path-to-smolmux-mcp>\n", argv[0]);
        return 1;
    }
    g_mcp_bin = argv[1];

    RUN_TEST(test_mcp_e2e_smoke);

    TEST_REPORT();
}
