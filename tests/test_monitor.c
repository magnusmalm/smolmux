/*
 * test_monitor — integration tests for monitor protocol interactions
 *
 * Uses PTY + broker-in-thread pattern (same as test_broker.c).
 * Tests protocol-level behavior, not terminal raw mode.
 */

#include "test_main.h"
#include "broker.h"
#include "links/uart.h"
#include "protocol.h"
#include "util/base64.h"
#include "util/json_helpers.h"
#include "monitor_esc.h"
#include "monitor_crlf.h"
#include "sm_features.h"
#if SM_ENABLE_SINK_TCP
#include "sinks/tcp.h"
#endif

#include <signal.h>
#include <sys/wait.h>

#include <pthread.h>
#include <unistd.h>
#include <pty.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <errno.h>

#define TEST_SOCK "/tmp/smolmux-test-mon.sock"
#define STARTUP_DELAY 150000  /* 150ms */

/* --- Helpers (same pattern as test_broker.c) --- */

static void *broker_thread(void *arg)
{
    sm_broker_t *b = arg;
    sm_broker_run(b);
    return NULL;
}

static int connect_unix(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    return fd;
}

static void send_json(int fd, cJSON *msg)
{
    size_t len;
    char *line = sm_msg_encode(msg, &len);
    write(fd, line, len);
    free(line);
    cJSON_Delete(msg);
}

static sm_msg_t recv_json(int fd)
{
    char buf[8192];
    size_t total = 0;

    for (int attempts = 0; attempts < 50; attempts++) {
        ssize_t n = read(fd, buf + total, sizeof(buf) - total - 1);
        if (n > 0) {
            total += (size_t)n;
            buf[total] = '\0';
            if (memchr(buf, '\n', total))
                break;
        }
        usleep(10000);
    }

    if (total == 0) {
        sm_msg_t empty = {SM_MSG_UNKNOWN, NULL};
        return empty;
    }
    return sm_msg_decode(buf, total);
}

typedef struct test_ctx {
    int master;
    int slave;
    sm_broker_t broker;
    sm_link_t *link;
    pthread_t tid;
} test_ctx_t;

static void setup(test_ctx_t *ctx)
{
    openpty(&ctx->master, &ctx->slave, NULL, NULL, NULL);
    char *slave_name = ttyname(ctx->slave);

    ctx->link = sm_uart_new(slave_name, 115200, 0);
    sm_broker_init(&ctx->broker, ctx->link, TEST_SOCK);
    snprintf(ctx->broker.port, sizeof(ctx->broker.port), "%s", slave_name);
    ctx->broker.baudrate = 115200;

    pthread_create(&ctx->tid, NULL, broker_thread, &ctx->broker);
    usleep(STARTUP_DELAY);
}

static void teardown(test_ctx_t *ctx)
{
    sm_broker_stop(&ctx->broker);
    pthread_join(ctx->tid, NULL);
    sm_broker_destroy(&ctx->broker);
    close(ctx->master);
    close(ctx->slave);
}

/* --- Tests --- */

static void test_connect_observe(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    /* Connect as observer */
    int fd = connect_unix(TEST_SOCK);
    ASSERT(fd >= 0, "connected to broker");

    send_json(fd, sm_msg_hello("monitor", "observer"));
    sm_msg_t welcome = recv_json(fd);
    ASSERT_NOT_NULL(welcome.root);
    ASSERT_INT_EQ(welcome.type, SM_MSG_WELCOME);
    ASSERT_STR_EQ(sm_json_get_string(welcome.root, "your_role"), "observer");
    sm_msg_free(&welcome);

    /* Write data from PTY master — should arrive as output */
    write(ctx.master, "hello from device\n", 18);
    usleep(100000);

    sm_msg_t out = recv_json(fd);
    ASSERT_NOT_NULL(out.root);
    ASSERT_INT_EQ(out.type, SM_MSG_OUTPUT);

    const char *b64 = sm_json_get_string(out.root, "data");
    ASSERT_NOT_NULL(b64);
    size_t dec_len;
    uint8_t *data = sm_base64_decode(b64, strlen(b64), &dec_len);
    ASSERT(dec_len > 0, "decoded data non-empty");
    ASSERT(memcmp(data, "hello from device\n", 18) == 0, "data matches");
    free(data);
    sm_msg_free(&out);

    close(fd);
    teardown(&ctx);
}

static void test_send_keystroke(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    int fd = connect_unix(TEST_SOCK);
    send_json(fd, sm_msg_hello("monitor", "controller"));
    sm_msg_t welcome = recv_json(fd);
    sm_msg_free(&welcome);

    /* Send a keystroke via sm_msg_send */
    uint8_t key = 'A';
    send_json(fd, sm_msg_send("k1", &key, 1));
    usleep(50000);

    /* Verify it reached the PTY master */
    char buf[256];
    ssize_t n = read(ctx.master, buf, sizeof(buf));
    ASSERT(n > 0, "keystroke reached device");
    ASSERT(buf[0] == 'A', "keystroke matches");

    close(fd);
    teardown(&ctx);
}

/* CTL-1: escape 'c' is not a second hello. Owning-phase helper. */
static void test_escape_c_is_not_hello(void)
{
    ASSERT_INT_EQ(sm_mon_esc_kind('c'), SM_MON_ESC_RESTART_C);
    ASSERT(sm_mon_esc_kind('c') != SM_MON_ESC_FORWARD, "c is not forwarded");
    ASSERT_INT_EQ(sm_mon_esc_kind('q'), SM_MON_ESC_QUIT);
    ASSERT_INT_EQ(sm_mon_esc_kind('t'), SM_MON_ESC_TAKEOVER);
    ASSERT_INT_EQ(sm_mon_esc_wire(SM_MON_ESC_RESTART_C), SM_MON_WIRE_NONE);
    ASSERT_INT_EQ(sm_mon_esc_wire(sm_mon_esc_kind('c')), SM_MON_WIRE_NONE);
    ASSERT_INT_EQ(sm_mon_esc_wire(SM_MON_ESC_STATUS), SM_MON_WIRE_STATUS);
    ASSERT_INT_EQ(sm_mon_esc_wire(SM_MON_ESC_TAKEOVER), SM_MON_WIRE_TAKEOVER);
}

static size_t map_str(const char *in, char *out, int *prev_cr)
{
    size_t n = sm_mon_map_lf((const uint8_t *)in, strlen(in),
                             (uint8_t *)out, prev_cr);
    out[n] = '\0';
    return n;
}

/* Bare LF from the device must return the carriage on a raw-mode tty. */
static void test_map_lf(void)
{
    char out[64];
    int cr = 0;

    map_str("a\nb\n", out, &cr);
    ASSERT_STR_EQ(out, "a\r\nb\r\n");

    cr = 0;
    map_str("a\r\nb\r\n", out, &cr);
    ASSERT_STR_EQ(out, "a\r\nb\r\n");

    /* CRLF split across two output chunks is not doubled. */
    cr = 0;
    map_str("line\r", out, &cr);
    ASSERT_STR_EQ(out, "line\r");
    map_str("\nnext", out, &cr);
    ASSERT_STR_EQ(out, "\nnext");

    /* LF then LF is two line breaks; lone CR (progress bars) is kept. */
    cr = 0;
    map_str("\n\n", out, &cr);
    ASSERT_STR_EQ(out, "\r\n\r\n");
    map_str("50%\r60%", out, &cr);
    ASSERT_STR_EQ(out, "50%\r60%");

    /* Non-text bytes pass through; only 0x0a gains a CR. */
    const uint8_t bin[] = { 0x00, 0xff, 0x0a, 0x1b, 0x0d, 0x0a };
    const uint8_t want[] = { 0x00, 0xff, 0x0d, 0x0a, 0x1b, 0x0d, 0x0a };
    uint8_t got[12];
    cr = 0;
    size_t n = sm_mon_map_lf(bin, sizeof(bin), got, &cr);
    ASSERT_INT_EQ((int)n, (int)sizeof(want));
    ASSERT(memcmp(got, want, sizeof(want)) == 0, "binary bytes unchanged");
}

static void test_role_upgrade_rejected(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    /* Connect as observer first */
    int fd = connect_unix(TEST_SOCK);
    send_json(fd, sm_msg_hello("monitor", "observer"));
    sm_msg_t welcome = recv_json(fd);
    ASSERT_STR_EQ(sm_json_get_string(welcome.root, "your_role"), "observer");
    sm_msg_free(&welcome);

    /* Try to re-send hello as controller — should be rejected */
    send_json(fd, sm_msg_hello("monitor", "controller"));
    sm_msg_t err = recv_json(fd);
    ASSERT_INT_EQ(err.type, SM_MSG_ERROR);
    ASSERT_STR_EQ(sm_json_get_string(err.root, "message"), "hello already received");
    sm_msg_free(&err);

    /* Verify still observer — send should fail */
    send_json(fd, sm_msg_send("s1", (const uint8_t *)"x", 1));
    sm_msg_t err2 = recv_json(fd);
    ASSERT_INT_EQ(err2.type, SM_MSG_ERROR);
    sm_msg_free(&err2);

    close(fd);
    teardown(&ctx);
}

static void test_status_request(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    int fd = connect_unix(TEST_SOCK);
    send_json(fd, sm_msg_hello("monitor", "observer"));
    sm_msg_t welcome = recv_json(fd);
    sm_msg_free(&welcome);

    /* Request status */
    send_json(fd, sm_msg_status("mon-st"));
    sm_msg_t resp = recv_json(fd);
    ASSERT_NOT_NULL(resp.root);
    ASSERT_INT_EQ(resp.type, SM_MSG_STATUS_RESPONSE);
    ASSERT_INT_EQ(sm_json_get_bool(resp.root, "connected", 0), 1);
    ASSERT_INT_EQ(sm_json_get_int(resp.root, "baud", 0), 115200);
    sm_msg_free(&resp);

    close(fd);
    teardown(&ctx);
}

#if SM_ENABLE_SINK_TCP
/* smolmux-monitor built next to this test binary. */
static const char *find_monitor(char *buf, size_t len)
{
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return NULL;
    exe[n] = '\0';
    char *slash = strrchr(exe, '/');
    if (!slash) return NULL;
    *slash = '\0';
    if ((size_t)snprintf(buf, len, "%s/smolmux-monitor", exe) >= len)
        return NULL;
    return access(buf, X_OK) == 0 ? buf : NULL;
}

/* Run the real monitor against 127.0.0.1:port with token (NULL = none).
 * Collects stderr until it exits or ~3 s pass; kills it if still running.
 * Returns the exit status (-1 if it had to be killed). */
static int run_monitor(const char *bin, int port, const char *token,
                       char *err, size_t err_len)
{
    int ep[2];
    if (pipe(ep) != 0) return -2;
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(ep[1], STDERR_FILENO);
        close(ep[0]); close(ep[1]);
        /* No inherited token and no real token files. */
        unsetenv("SMOLMUX_AUTH_TOKEN");
        const char *tmp = getenv("TMPDIR");
        setenv("XDG_RUNTIME_DIR", tmp && tmp[0] ? tmp : "/tmp", 1);
        if (token) setenv("SMOLMUX_AUTH_TOKEN", token, 1);
        char spec[32];
        snprintf(spec, sizeof(spec), "127.0.0.1:%d", port);
        execl(bin, bin, "--tcp", spec, "-n", "mon-e2e", NULL);
        _exit(127);
    }
    close(ep[1]);
    fcntl(ep[0], F_SETFL, fcntl(ep[0], F_GETFL, 0) | O_NONBLOCK);
    size_t off = 0;
    int status = 0, exited = 0;
    for (int i = 0; i < 300 && !exited; i++) {
        ssize_t r = read(ep[0], err + off, err_len - off - 1);
        if (r > 0) off += (size_t)r;
        if (waitpid(pid, &status, WNOHANG) == pid) exited = 1;
        else usleep(10000);
    }
    ssize_t r;
    while ((r = read(ep[0], err + off, err_len - off - 1)) > 0)
        off += (size_t)r;
    err[off] = '\0';
    close(ep[0]);
    if (!exited) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* todo #18: "Connected to ..." only after the broker's welcome. A refused
 * hello must print the broker's reason, no banner, and exit non-zero. */
static void test_monitor_connected_only_after_welcome(void)
{
    char bin[4200];
    if (!find_monitor(bin, sizeof(bin))) {
        ASSERT(0, "smolmux-monitor next to test binary");
        return;
    }
    test_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    openpty(&ctx.master, &ctx.slave, NULL, NULL, NULL);
    char *slave_name = ttyname(ctx.slave);
    ctx.link = sm_uart_new(slave_name, 115200, 0);
    sm_broker_init(&ctx.broker, ctx.link, TEST_SOCK);
    snprintf(ctx.broker.port, sizeof(ctx.broker.port), "%s", slave_name);
    ctx.broker.baudrate = 115200;
    snprintf(ctx.broker.auth_token, sizeof(ctx.broker.auth_token), "sekrit");
    const int port = 15571;
    sm_broker_add_sink(&ctx.broker, sm_tcp_sink_new(port, NULL));
    pthread_create(&ctx.tid, NULL, broker_thread, &ctx.broker);
    usleep(STARTUP_DELAY);

    char err[8192];
    int rc = run_monitor(bin, port, NULL, err, sizeof(err));
    ASSERT(strstr(err, "authentication failed") != NULL,
           "refusal shows the broker's reason");
    ASSERT(strstr(err, "Connected to") == NULL, "no banner when refused");
    ASSERT(rc > 0, "refused monitor exits non-zero");

    rc = run_monitor(bin, port, "sekrit", err, sizeof(err));
    ASSERT(strstr(err, "Connected to tcp://127.0.0.1") != NULL,
           "banner after welcome with the right token");
    ASSERT(strstr(err, "authentication failed") == NULL, "accepted");

    teardown(&ctx);
}
#endif

int main(void)
{
    printf("test_monitor\n");

    RUN_TEST(test_connect_observe);
    RUN_TEST(test_send_keystroke);
    RUN_TEST(test_escape_c_is_not_hello);
    RUN_TEST(test_map_lf);
    RUN_TEST(test_role_upgrade_rejected);
    RUN_TEST(test_status_request);
#if SM_ENABLE_SINK_TCP
    RUN_TEST(test_monitor_connected_only_after_welcome);
#endif

    TEST_REPORT();
}
