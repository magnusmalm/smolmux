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
#include "sm_features.h"
#include "mcp_broker_conn.h"
#if SM_ENABLE_SINK_TCP
#include "sinks/tcp.h"
#endif

#include <pthread.h>
#include <unistd.h>
#include <pty.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>

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

/* tcp_spec NULL connects over the Unix socket; otherwise "host:port".
 * client_token NULL leaves SMOLMUX_AUTH_TOKEN unset in the child. */
static void spawn_mcp_full(fixture_t *fx, const char *tcp_spec,
                           const char *client_token)
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

        /* Never inherit the runner's token — sm_msg_hello() would pick it up
         * and quietly authenticate a case meant to be rejected. */
        unsetenv("SMOLMUX_AUTH_TOKEN");
        if (client_token)
            setenv("SMOLMUX_AUTH_TOKEN", client_token, 1);

        if (tcp_spec)
            execl(g_mcp_bin, g_mcp_bin, "--tcp", tcp_spec, "-n", "mcp-e2e", NULL);
        else
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

/* broker_token arms the broker's auth gate before its thread starts (so the
 * test never races the reader). tcp_port > 0 adds a TCP sink and points the
 * MCP server at it — the token is only enforced for network-origin clients,
 * so an auth test cannot run over the Unix socket. */
static void setup_full(fixture_t *fx, const char *broker_token, int tcp_port,
                       const char *client_token)
{
    memset(fx, 0, sizeof(*fx));
    openpty(&fx->master, &fx->slave, NULL, NULL, NULL);
    char *slave_name = ttyname(fx->slave);

    fx->link = sm_uart_new(slave_name, 115200, 0);
    sm_broker_init(&fx->broker, fx->link, TEST_SOCK);
    snprintf(fx->broker.port, sizeof(fx->broker.port), "%s", slave_name);
    fx->broker.baudrate = 115200;
    if (broker_token)
        snprintf(fx->broker.auth_token, sizeof(fx->broker.auth_token), "%s",
                 broker_token);

    char tcp_spec[64];
    tcp_spec[0] = '\0';
    if (tcp_port > 0) {
#if SM_ENABLE_SINK_TCP
        sm_broker_add_sink(&fx->broker, sm_tcp_sink_new(tcp_port, NULL));
        snprintf(tcp_spec, sizeof(tcp_spec), "127.0.0.1:%d", tcp_port);
#else
        /* TCP auth tests are compiled out without the sink; callers must not
         * request a port when SM_ENABLE_SINK_TCP is off. */
        (void)tcp_port;
#endif
    }

    pthread_create(&fx->tid, NULL, broker_thread, &fx->broker);
    usleep(STARTUP_DELAY);

    spawn_mcp_full(fx, tcp_spec[0] ? tcp_spec : NULL, client_token);
    usleep(STARTUP_DELAY);  /* mcp connects + hellos before first request */
}

static void setup(fixture_t *fx)
{
    setup_full(fx, NULL, 0, NULL);
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

/* sm_broker_conn_wait() must drain a final message that arrives together with
 * the peer's hangup. Writing a line to a socketpair and closing the write end
 * makes poll report POLLIN|POLLHUP in the same revents — exactly the shape a
 * broker produces when it answers "authentication failed" and immediately
 * drops the client. Checking hangup first discarded that reply unread, which
 * is why the reason never reached the agent.
 *
 * Deterministic on purpose: the e2e test below races the socket close and so
 * does not reliably exercise this ordering. */
static void test_conn_wait_drains_data_before_hangup(void)
{
    int sv[2];
    ASSERT_INT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    const char *line =
        "{\"type\":\"error\",\"message\":\"authentication failed\"}\n";
    ASSERT(write(sv[1], line, strlen(line)) > 0, "queued the final message");
    close(sv[1]);   /* data and EOF now pending together */

    sm_broker_conn_t c;
    ASSERT_INT_EQ(sm_broker_conn_init(&c, 4096), 0);
    c.fd = sv[0];

    cJSON *msg = sm_broker_conn_wait(&c, NULL, 1000);
    ASSERT_NOT_NULL(msg);
    if (msg) {
        ASSERT_STR_EQ(sm_json_get_string(msg, "message"),
                      "authentication failed");
        cJSON_Delete(msg);
    }

    sm_broker_conn_destroy(&c);
}

static char g_event_type[32];
static void event_cb(void *user, const char *type, cJSON *root)
{
    (void)user;
    (void)root;
    snprintf(g_event_type, sizeof(g_event_type), "%s", type ? type : "");
}

static void test_conn_event_cb_link_down(void)
{
    int sv[2];
    ASSERT_INT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    const char *line = "{\"type\":\"link_down\",\"reason\":\"device disconnected\"}\n";
    ASSERT(write(sv[1], line, strlen(line)) > 0, "queued link_down");
    close(sv[1]);

    sm_broker_conn_t c;
    ASSERT_INT_EQ(sm_broker_conn_init(&c, 4096), 0);
    c.fd = sv[0];
    g_event_type[0] = '\0';
    sm_broker_conn_set_event_cb(&c, event_cb, NULL);
    (void)sm_broker_conn_read(&c, "no-match");
    ASSERT_STR_EQ(g_event_type, "link_down");
    sm_broker_conn_destroy(&c);
}

#if SM_ENABLE_SINK_TCP
/* A rejected handshake must reach the agent as a JSON-RPC result naming the
 * cause, not as silence.
 *
 * try_connect_broker() used to mark itself online whenever the welcome wait
 * came back empty, on top of a socket the broker had already dropped. Three
 * things hid the reason: the broker's error reply was discarded because
 * sm_broker_conn_wait() honoured POLLHUP before draining POLLIN; the startup
 * wait only matched SM_MSG_WELCOME, so an error was "unmatched" even when
 * read; and the connect reported success regardless. The agent got no
 * response at all and the operator got no hint that a token was wrong.
 *
 * Runs over TCP on purpose: the broker only enforces the token for
 * network-origin clients, so this cannot be exercised on the Unix socket.
 * Gated on SM_ENABLE_SINK_TCP — without the sink the fixture cannot link. */
static void test_mcp_auth_rejection_reaches_agent(void)
{
    fixture_t fx;
    setup_full(&fx, "sekrit", 15557, NULL);   /* broker wants a token; client has none */

    cJSON *resp = rpc_call(&fx, 1,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":"
        "{\"name\":\"serial_port_status\",\"arguments\":{}}}",
        400);
    ASSERT_NOT_NULL(resp);   /* silence is the bug */
    if (resp) {
        const char *text = tool_text(resp);
        ASSERT_NOT_NULL(text);
        if (text) {
            ASSERT(strstr(text, "authentication failed") != NULL,
                   "names the broker's actual reason");
            ASSERT(strstr(text, "SMOLMUX_AUTH_TOKEN") != NULL,
                   "hint names the token variable");
            /* Must not be mistaken for an unreachable broker: that hint would
             * send the agent off to start a second one. */
            ASSERT(strstr(text, "Start a broker first") == NULL,
                   "not the generic no-broker guidance");
        }
        cJSON_Delete(resp);
    }

    /* Soft-fail means soft-fail: the server is still alive and answering
     * after the rejection, not wedged or exited. */
    cJSON *resp2 = rpc_call(&fx, 2,
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":"
        "{\"name\":\"serial_list_ports\",\"arguments\":{}}}",
        400);
    ASSERT_NOT_NULL(resp2);
    if (resp2) cJSON_Delete(resp2);

    teardown(&fx);
}
#endif /* SM_ENABLE_SINK_TCP */

static void test_mcp_e2e_smoke(void)
{
    /* Child MCP inherits this; mutate tools are hidden by default. */
    setenv("SMOLMUX_MCP_MUTATE", "1", 1);
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
        /* Wave 1 P0a */
        const char *ins = sm_json_get_string(result, "instructions");
        ASSERT(ins && strlen(ins) >= 200, "instructions present");
        ASSERT(ins && strstr(ins, "serial_suspend") != NULL,
               "instructions: serial_suspend");
        ASSERT(ins && strstr(ins, "serial_get_incidents") != NULL,
               "instructions: serial_get_incidents");
        cJSON *caps = cJSON_GetObjectItem(result, "capabilities");
        ASSERT(caps && cJSON_GetObjectItem(caps, "prompts") != NULL,
               "capabilities.prompts");
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

    /* Wave 1 P0b: prompts/list exact set */
    resp = rpc_call(&fx, 9,
        "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"prompts/list\"}", 500);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        cJSON *prompts = cJSON_GetObjectItem(
            cJSON_GetObjectItem(resp, "result"), "prompts");
        ASSERT(cJSON_IsArray(prompts) && cJSON_GetArraySize(prompts) == 3,
               "exactly 3 prompts");
        int saw_b = 0, saw_d = 0, saw_f = 0;
        cJSON *p;
        cJSON_ArrayForEach(p, prompts) {
            const char *n = sm_json_get_string(p, "name");
            if (n && strcmp(n, "bringup") == 0) saw_b = 1;
            else if (n && strcmp(n, "debug_serial") == 0) saw_d = 1;
            else if (n && strcmp(n, "flash_safe") == 0) saw_f = 1;
        }
        ASSERT(saw_b && saw_d && saw_f, "bringup/debug_serial/flash_safe");
        cJSON_Delete(resp);
    }

    /* prompts/get debug_serial cites incidents tool */
    resp = rpc_call(&fx, 10,
        "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"prompts/get\","
        "\"params\":{\"name\":\"debug_serial\"}}", 500);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        cJSON *result = cJSON_GetObjectItem(resp, "result");
        cJSON *msgs = cJSON_GetObjectItem(result, "messages");
        cJSON *msg0 = cJSON_GetArrayItem(msgs, 0);
        cJSON *content = cJSON_GetObjectItem(msg0, "content");
        const char *text = sm_json_get_string(content, "text");
        ASSERT(text && strstr(text, "serial_get_incidents") != NULL,
               "debug_serial cites serial_get_incidents");
        cJSON_Delete(resp);
    }

    /* unknown prompt */
    resp = rpc_call(&fx, 11,
        "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"prompts/get\","
        "\"params\":{\"name\":\"nope\"}}", 500);
    ASSERT_NOT_NULL(resp);
    if (resp) {
        ASSERT(cJSON_GetObjectItem(resp, "error") != NULL,
               "unknown prompt errors");
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
    RUN_TEST(test_conn_wait_drains_data_before_hangup);
    RUN_TEST(test_conn_event_cb_link_down);
#if SM_ENABLE_SINK_TCP
    RUN_TEST(test_mcp_auth_rejection_reaches_agent);
#endif

    TEST_REPORT();
}
