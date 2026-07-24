#include "test_main.h"
#include "broker.h"
#include "client.h"
#include "links/uart.h"
#include "protocol.h"
#include "broker_info.h"
#include "util/base64.h"
#include "util/json_helpers.h"
#include "util/shared_line.h"
#include "util/sock_util.h"

#include <pthread.h>
#include <unistd.h>
#include <pty.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

#define TEST_SOCK "/tmp/smolmux-test.sock"
#define STARTUP_DELAY 150000  /* 150ms */

/* --- Helpers --- */

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

    /* Set non-blocking for test reads */
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

    /* Read until we get a newline */
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

static int status_client_int(cJSON *root, const char *name, const char *key)
{
    cJSON *clients = cJSON_GetObjectItem(root, "clients");
    if (!clients || !cJSON_IsArray(clients))
        return -1;

    cJSON *ci;
    cJSON_ArrayForEach(ci, clients) {
        const char *n = sm_json_get_string(ci, "name");
        if (n && strcmp(n, name) == 0)
            return sm_json_get_int(ci, key, -1);
    }
    return -1;
}

static void teardown(test_ctx_t *ctx)
{
    sm_broker_stop(&ctx->broker);
    pthread_join(ctx->tid, NULL);
    sm_broker_destroy(&ctx->broker);
    close(ctx->master);
    close(ctx->slave);
}

/* --- sm_broker_stop_by_socket: real child process, SIGTERM path --- */

static sm_broker_t g_child_broker;

static void child_sigterm(int sig)
{
    (void)sig;
    sm_broker_stop(&g_child_broker);
}

/* Fork a child running a real broker on a fresh PTY, mimicking main.c's
 * SIGTERM handler -> sm_broker_stop -> clean teardown (socket unlinked).
 * Parent keeps the PTY master open for the test's duration. */
static pid_t fork_broker_process(const char *sock, int *master_out,
                                 char *slave_name_out, size_t slave_name_len)
{
    int master = -1, slave = -1;
    if (openpty(&master, &slave, NULL, NULL, NULL) != 0)
        return -1;
    if (slave_name_out)
        snprintf(slave_name_out, slave_name_len, "%s", ttyname(slave));

    pid_t pid = fork();
    if (pid == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = child_sigterm;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGTERM, &sa, NULL);

        char *slave_name = ttyname(slave);
        sm_link_t *link = sm_uart_new(slave_name, 115200, 0);
        sm_broker_init(&g_child_broker, link, sock);
        snprintf(g_child_broker.port, sizeof(g_child_broker.port), "%s",
                 slave_name);
        g_child_broker.baudrate = 115200;
        broker_thread(&g_child_broker);
        sm_broker_destroy(&g_child_broker);
        _exit(0);
    }
    close(slave);
    *master_out = master;
    return pid;
}

static void stop_test_sock_path(char *out, size_t len, const char *tag)
{
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0])
        tmp = "/tmp";
    snprintf(out, len, "%s/test-broker-%s-%d.sock", tmp, tag, (int)getpid());
}

static void test_stop_by_socket(void)
{
    char sock[SM_SOCK_PATH_MAX];
    stop_test_sock_path(sock, sizeof(sock), "stop");
    unlink(sock);

    int master = -1;
    pid_t pid = fork_broker_process(sock, &master, NULL, 0);
    ASSERT(pid > 0, "forked broker child");

    int fd = -1;
    for (int i = 0; i < 150; i++) {
        fd = connect_unix(sock);
        if (fd >= 0)
            break;
        usleep(20 * 1000);
    }
    ASSERT(fd >= 0, "child broker reachable");
    if (fd >= 0)
        close(fd);

    int bpid = -1;
    char err[256] = "";
    ASSERT_INT_EQ(sm_broker_stop_by_socket(sock, 5000, &bpid, err,
                                           sizeof(err)), 0);
    ASSERT_INT_EQ(bpid, (int)pid);

    struct stat st;
    ASSERT(stat(sock, &st) != 0 && errno == ENOENT,
           "socket unlinked after stop");

    int status = 0;
    ASSERT(waitpid(pid, &status, 0) == pid, "child reaped");
    ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0,
           "child exited cleanly");
    close(master);

    /* Second stop must fail loudly, not hang or invent a pid. */
    err[0] = '\0';
    ASSERT_INT_EQ(sm_broker_stop_by_socket(sock, 1000, NULL, err,
                                           sizeof(err)), -1);
    ASSERT(strstr(err, "no broker socket") != NULL,
           "second stop reports missing broker");
}

/* sm_find_broker_for_endpoint: direct path, by-id-style symlink, negative.
 * Discovery only globs smolmux-*.sock under XDG_RUNTIME_DIR and /tmp, so the
 * test broker lives in a private XDG dir with a glob-matching name. */
static void test_find_broker_for_endpoint(void)
{
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !tmpdir[0])
        tmpdir = "/tmp";
    /* 80 keeps xdg + "/smolmux-find-test.sock" inside SM_SOCK_PATH_MAX. */
    char xdg[80];
    snprintf(xdg, sizeof(xdg), "%s/smolmux-find-xdg-%d", tmpdir, (int)getpid());
    ASSERT(mkdir(xdg, 0700) == 0 || errno == EEXIST, "mkdir private XDG");
    char saved_xdg[256] = {0};
    const char *prev_xdg = getenv("XDG_RUNTIME_DIR");
    if (prev_xdg)
        snprintf(saved_xdg, sizeof(saved_xdg), "%s", prev_xdg);
    setenv("XDG_RUNTIME_DIR", xdg, 1);

    char sock[SM_SOCK_PATH_MAX];
    snprintf(sock, sizeof(sock), "%s/smolmux-find-test.sock", xdg);
    unlink(sock);

    int master = -1;
    char slave_name[128] = "";
    pid_t pid = fork_broker_process(sock, &master, slave_name,
                                    sizeof(slave_name));
    ASSERT(pid > 0, "forked broker child");
    ASSERT(slave_name[0], "captured pty slave name");

    int fd = -1;
    for (int i = 0; i < 150; i++) {
        fd = connect_unix(sock);
        if (fd >= 0)
            break;
        usleep(20 * 1000);
    }
    ASSERT(fd >= 0, "child broker reachable");
    if (fd >= 0)
        close(fd);

    sm_broker_info_t info;
    memset(&info, 0, sizeof(info));
    ASSERT_INT_EQ(sm_find_broker_for_endpoint(slave_name, &info, 800), 0);
    ASSERT_STR_EQ(info.socket, sock);
    ASSERT_INT_EQ(info.pid, (int)pid);

    /* A by-id-style symlink to the same device must match via realpath. */
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0])
        tmp = "/tmp";
    char link_path[256];
    snprintf(link_path, sizeof(link_path), "%s/test-byid-link-%d", tmp,
             (int)getpid());
    unlink(link_path);
    ASSERT_INT_EQ(symlink(slave_name, link_path), 0);
    memset(&info, 0, sizeof(info));
    ASSERT_INT_EQ(sm_find_broker_for_endpoint(link_path, &info, 800), 0);
    ASSERT_STR_EQ(info.socket, sock);
    unlink(link_path);

    /* A port nobody holds finds nothing. */
    ASSERT_INT_EQ(sm_find_broker_for_endpoint("/dev/null", NULL, 800), -1);

    char err[256] = "";
    ASSERT_INT_EQ(sm_broker_stop_by_socket(sock, 5000, NULL, err,
                                           sizeof(err)), 0);
    int status = 0;
    ASSERT(waitpid(pid, &status, 0) == pid, "child reaped");
    close(master);

    if (prev_xdg)
        setenv("XDG_RUNTIME_DIR", saved_xdg, 1);
    else
        unsetenv("XDG_RUNTIME_DIR");
    rmdir(xdg);
}

static void test_stop_by_socket_stale(void)
{
    char sock[SM_SOCK_PATH_MAX];
    stop_test_sock_path(sock, sizeof(sock), "stale");
    unlink(sock);

    /* Bind then close without unlinking: the file remains, nobody listens. */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT(fd >= 0, "socket created");
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock);
    ASSERT_INT_EQ(bind(fd, (struct sockaddr *)&addr, sizeof(addr)), 0);
    close(fd);

    char err[256] = "";
    ASSERT_INT_EQ(sm_broker_stop_by_socket(sock, 1000, NULL, err,
                                           sizeof(err)), -1);
    ASSERT(strstr(err, "not responding") != NULL, "stale socket reported");
    unlink(sock);
}

/* --- Tests --- */

static void test_start_stop(void)
{
    test_ctx_t ctx;
    setup(&ctx);
    ASSERT(!ctx.broker.stopped, "broker running");
    teardown(&ctx);
    ASSERT(1, "broker started and stopped cleanly");
}

/* Long by-id auto-derived socket: broker must bind/listen and accept clients.
 * Uses a private XDG_RUNTIME_DIR so we do not touch the real runtime dir. */
static void test_broker_listens_on_derived_long_byid_socket(void)
{
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0])
        tmp = "/tmp";
    char xdg[256];
    snprintf(xdg, sizeof(xdg), "%s/smolmux-xdg-%d", tmp, (int)getpid());
    ASSERT(mkdir(xdg, 0700) == 0 || errno == EEXIST, "mkdir private XDG");

    char saved_xdg[256] = {0};
    const char *prev = getenv("XDG_RUNTIME_DIR");
    if (prev)
        snprintf(saved_xdg, sizeof(saved_xdg), "%s", prev);
    setenv("XDG_RUNTIME_DIR", xdg, 1);

    const char *byid =
        "/dev/serial/by-id/"
        "usb-Prolific_Technology_Inc._USB-Serial_Controller_TESTSERIAL00-if00-port0";
    char sock[SM_SOCK_PATH_MAX];
    ASSERT_INT_EQ(sm_derive_socket_path(sock, sizeof(sock), byid), 0);
    ASSERT(strlen(sock) <= SM_SOCK_FINAL_MAX, "derived path within final max");
    /* Old naive form would be too long for temp bind under this XDG prefix. */
    char naive[512];
    snprintf(naive, sizeof(naive), "%s/smolmux-%s.sock", xdg,
             "usb-Prolific_Technology_Inc._USB-Serial_Controller_"
             "TESTSERIAL00-if00-port0");
    char naive_tmp[576];
    int nt = snprintf(naive_tmp, sizeof(naive_tmp), "%s.%d.tmp", naive, 1234567890);
    ASSERT(nt >= SM_SOCK_PATH_MAX, "naive long path would overflow sun_path");

    int master = -1, slave = -1;
    ASSERT_INT_EQ(openpty(&master, &slave, NULL, NULL, NULL), 0);
    char *slave_name = ttyname(slave);
    ASSERT_NOT_NULL(slave_name);

    sm_link_t *link = sm_uart_new(slave_name, 115200, 0);
    ASSERT_NOT_NULL(link);

    sm_broker_t broker;
    sm_broker_init(&broker, link, sock);
    snprintf(broker.port, sizeof(broker.port), "%s", slave_name);
    broker.baudrate = 115200;

    pthread_t tid;
    ASSERT_INT_EQ(pthread_create(&tid, NULL, broker_thread, &broker), 0);
    usleep(STARTUP_DELAY);

    ASSERT(!broker.stopped, "broker running on derived long socket");
    int fd = connect_unix(sock);
    ASSERT(fd >= 0, "client connected to derived long by-id socket");

    send_json(fd, sm_msg_hello("byid-test", "observer"));
    sm_msg_t resp = recv_json(fd);
    ASSERT_NOT_NULL(resp.root);
    ASSERT_INT_EQ(resp.type, SM_MSG_WELCOME);
    sm_msg_free(&resp);
    close(fd);

    sm_broker_stop(&broker);
    pthread_join(tid, NULL);
    sm_broker_destroy(&broker);
    close(master);
    close(slave);
    unlink(sock);

    if (saved_xdg[0])
        setenv("XDG_RUNTIME_DIR", saved_xdg, 1);
    else
        unsetenv("XDG_RUNTIME_DIR");
    rmdir(xdg);
}

static void test_connect_hello(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    int fd = connect_unix(TEST_SOCK);
    ASSERT(fd >= 0, "connected to broker");

    send_json(fd, sm_msg_hello("test", "controller"));
    sm_msg_t resp = recv_json(fd);
    ASSERT_NOT_NULL(resp.root);
    ASSERT_INT_EQ(resp.type, SM_MSG_WELCOME);
    ASSERT_STR_EQ(sm_json_get_string(resp.root, "your_role"), "controller");
    sm_msg_free(&resp);

    close(fd);
    teardown(&ctx);
}

static void test_send_receive(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    int fd = connect_unix(TEST_SOCK);
    send_json(fd, sm_msg_hello("test", "controller"));
    sm_msg_t welcome = recv_json(fd);
    sm_msg_free(&welcome);

    /* Send data to device */
    send_json(fd, sm_msg_send("s1", (const uint8_t *)"hello\n", 6));

    /* Verify it arrived at PTY master */
    usleep(50000);
    char buf[256];
    ssize_t n = read(ctx.master, buf, sizeof(buf));
    ASSERT(n > 0, "data reached device");
    ASSERT(memcmp(buf, "hello\n", 6) == 0, "data matches");

    /* Device sends response */
    write(ctx.master, "response\n", 9);
    usleep(50000);

    /* Client should receive output */
    sm_msg_t out = recv_json(fd);
    ASSERT_NOT_NULL(out.root);
    ASSERT_INT_EQ(out.type, SM_MSG_OUTPUT);
    sm_msg_free(&out);

    close(fd);
    teardown(&ctx);
}

static void test_observer_cannot_send(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    int fd = connect_unix(TEST_SOCK);
    send_json(fd, sm_msg_hello("obs", "observer"));
    sm_msg_t welcome = recv_json(fd);
    sm_msg_free(&welcome);

    /* Try to send — should get error */
    send_json(fd, sm_msg_send("s1", (const uint8_t *)"data", 4));
    sm_msg_t resp = recv_json(fd);
    ASSERT_NOT_NULL(resp.root);
    ASSERT_INT_EQ(resp.type, SM_MSG_ERROR);
    sm_msg_free(&resp);

    close(fd);
    teardown(&ctx);
}

static void test_send_expect(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    int fd = connect_unix(TEST_SOCK);
    send_json(fd, sm_msg_hello("test", "controller"));
    sm_msg_t welcome = recv_json(fd);
    sm_msg_free(&welcome);

    /* Send with expect pattern */
    send_json(fd, sm_msg_send_expect("e1", (const uint8_t *)"cmd\n", 4,
                                      "result", 5000));

    /* Device sends matching response */
    usleep(50000);
    /* Drain the command from master */
    char buf[256];
    read(ctx.master, buf, sizeof(buf));

    write(ctx.master, "result ok\n", 10);
    usleep(100000);

    /* Expect reading output messages until we get expect_result */
    sm_msg_t msg;
    int found_result = 0;
    for (int i = 0; i < 10; i++) {
        msg = recv_json(fd);
        if (!msg.root) break;
        if (msg.type == SM_MSG_EXPECT_RESULT) {
            found_result = 1;
            ASSERT_INT_EQ(sm_json_get_bool(msg.root, "matched", 0), 1);
            sm_msg_free(&msg);
            break;
        }
        sm_msg_free(&msg);
    }
    ASSERT(found_result, "received expect_result");

    close(fd);
    teardown(&ctx);
}

static void test_send_expect_timeout(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    int fd = connect_unix(TEST_SOCK);
    send_json(fd, sm_msg_hello("test", "controller"));
    sm_msg_t welcome = recv_json(fd);
    sm_msg_free(&welcome);

    /* Send with expect — very short timeout, pattern won't match */
    send_json(fd, sm_msg_send_expect("e2", (const uint8_t *)"cmd\n", 4,
                                      "never_match_this_pattern", 100));

    /* Wait for timeout */
    usleep(300000);

    sm_msg_t msg;
    int found_result = 0;
    for (int i = 0; i < 10; i++) {
        msg = recv_json(fd);
        if (!msg.root) break;
        if (msg.type == SM_MSG_EXPECT_RESULT) {
            found_result = 1;
            ASSERT_INT_EQ(sm_json_get_bool(msg.root, "matched", 0), 0);
            sm_msg_free(&msg);
            break;
        }
        sm_msg_free(&msg);
    }
    ASSERT(found_result, "received timeout expect_result");

    close(fd);
    teardown(&ctx);
}

static void test_takeover_release(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    /* Two controllers */
    int fd1 = connect_unix(TEST_SOCK);
    send_json(fd1, sm_msg_hello("ctrl1", "controller"));
    sm_msg_t w1 = recv_json(fd1);
    sm_msg_free(&w1);

    int fd2 = connect_unix(TEST_SOCK);
    send_json(fd2, sm_msg_hello("ctrl2", "controller"));
    sm_msg_t w2 = recv_json(fd2);
    sm_msg_free(&w2);

    /* Client 1 takes over */
    send_json(fd1, sm_msg_takeover("t1"));
    sm_msg_t ack = recv_json(fd1);
    ASSERT_NOT_NULL(ack.root);
    sm_msg_free(&ack);

    /* Client 2 tries to send — should fail */
    send_json(fd2, sm_msg_send("s2", (const uint8_t *)"blocked", 7));
    sm_msg_t err = recv_json(fd2);
    ASSERT_NOT_NULL(err.root);
    ASSERT_INT_EQ(err.type, SM_MSG_ERROR);
    sm_msg_free(&err);

    /* Client 1 releases */
    send_json(fd1, sm_msg_release("r1"));
    sm_msg_t rel = recv_json(fd1);
    sm_msg_free(&rel);

    /* Now client 2 can send */
    send_json(fd2, sm_msg_send("s3", (const uint8_t *)"ok\n", 3));
    usleep(50000);
    char buf[256];
    ssize_t n = read(ctx.master, buf, sizeof(buf));
    ASSERT(n > 0, "data sent after release");

    close(fd1);
    close(fd2);
    teardown(&ctx);
}

static void test_status_query(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    int fd = connect_unix(TEST_SOCK);
    send_json(fd, sm_msg_hello("test", "observer"));
    sm_msg_t welcome = recv_json(fd);
    sm_msg_free(&welcome);

    send_json(fd, sm_msg_status("st1"));
    sm_msg_t resp = recv_json(fd);
    ASSERT_NOT_NULL(resp.root);
    ASSERT_INT_EQ(resp.type, SM_MSG_STATUS_RESPONSE);
    ASSERT_INT_EQ(sm_json_get_bool(resp.root, "connected", 0), 1);
    sm_msg_free(&resp);

    close(fd);
    teardown(&ctx);
}

static void test_history_request(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    /* Generate some device output */
    write(ctx.master, "boot output\n", 12);
    usleep(100000);

    int fd = connect_unix(TEST_SOCK);
    send_json(fd, sm_msg_hello("test", "observer"));
    sm_msg_t welcome = recv_json(fd);
    sm_msg_free(&welcome);

    /* Drain any output message */
    usleep(50000);
    sm_msg_t out = recv_json(fd);
    if (out.root) sm_msg_free(&out);

    send_json(fd, sm_msg_history_request("h1", 0.0, 0));
    sm_msg_t resp = recv_json(fd);
    ASSERT_NOT_NULL(resp.root);
    ASSERT_INT_EQ(resp.type, SM_MSG_HISTORY_RESPONSE);
    cJSON *chunks = cJSON_GetObjectItem(resp.root, "chunks");
    ASSERT(cJSON_IsArray(chunks), "chunks is array");
    ASSERT(cJSON_GetArraySize(chunks) > 0, "has history");
    sm_msg_free(&resp);

    close(fd);
    teardown(&ctx);
}

static void test_suspend_resume(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    int fd = connect_unix(TEST_SOCK);
    send_json(fd, sm_msg_hello("test", "controller"));
    sm_msg_t welcome = recv_json(fd);
    sm_msg_free(&welcome);

    /* Suspend */
    send_json(fd, sm_msg_suspend("su1"));
    usleep(50000);

    /* Read the suspended broadcast */
    sm_msg_t msg = recv_json(fd);
    ASSERT_NOT_NULL(msg.root);
    ASSERT_INT_EQ(msg.type, SM_MSG_SUSPENDED);
    sm_msg_free(&msg);

    /* Sending should fail while suspended */
    send_json(fd, sm_msg_send("s-susp", (const uint8_t *)"data", 4));
    sm_msg_t err = recv_json(fd);
    ASSERT_NOT_NULL(err.root);
    ASSERT_INT_EQ(err.type, SM_MSG_ERROR);
    sm_msg_free(&err);

    /* Resume */
    send_json(fd, sm_msg_resume("re1"));
    usleep(50000);

    msg = recv_json(fd);
    ASSERT_NOT_NULL(msg.root);
    ASSERT_INT_EQ(msg.type, SM_MSG_RESUMED);
    sm_msg_free(&msg);

    close(fd);
    teardown(&ctx);
}

static void test_broadcast_to_observer(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    /* Connect controller and observer */
    int ctrl = connect_unix(TEST_SOCK);
    send_json(ctrl, sm_msg_hello("ctrl", "controller"));
    sm_msg_t w1 = recv_json(ctrl);
    sm_msg_free(&w1);

    int obs = connect_unix(TEST_SOCK);
    send_json(obs, sm_msg_hello("obs", "observer"));
    sm_msg_t w2 = recv_json(obs);
    sm_msg_free(&w2);

    /* Device sends output */
    write(ctx.master, "device output\n", 14);
    usleep(100000);

    /* Observer should receive output */
    sm_msg_t out = recv_json(obs);
    ASSERT_NOT_NULL(out.root);
    ASSERT_INT_EQ(out.type, SM_MSG_OUTPUT);
    sm_msg_free(&out);

    close(ctrl);
    close(obs);
    teardown(&ctx);
}

/* T1: Verify data flow works after suspend/resume (fd may change) */
static void test_suspend_resume_data_flow(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    int fd = connect_unix(TEST_SOCK);
    send_json(fd, sm_msg_hello("test", "controller"));
    sm_msg_t welcome = recv_json(fd);
    sm_msg_free(&welcome);

    /* Verify data flow works before suspend */
    write(ctx.master, "before\n", 7);
    usleep(100000);
    sm_msg_t pre = recv_json(fd);
    ASSERT_NOT_NULL(pre.root);
    ASSERT_INT_EQ(pre.type, SM_MSG_OUTPUT);
    sm_msg_free(&pre);

    /* Suspend */
    send_json(fd, sm_msg_suspend("su1"));
    usleep(50000);
    sm_msg_t smsg = recv_json(fd);
    ASSERT_INT_EQ(smsg.type, SM_MSG_SUSPENDED);
    sm_msg_free(&smsg);

    /* Resume */
    send_json(fd, sm_msg_resume("re1"));
    usleep(50000);
    sm_msg_t rmsg = recv_json(fd);
    ASSERT_INT_EQ(rmsg.type, SM_MSG_RESUMED);
    sm_msg_free(&rmsg);

    /* Verify data flow works AFTER resume — this is the critical check.
     * The link may have a new fd after reopen. */
    write(ctx.master, "after\n", 6);
    usleep(100000);
    sm_msg_t post = recv_json(fd);
    ASSERT_NOT_NULL(post.root);
    ASSERT_INT_EQ(post.type, SM_MSG_OUTPUT);
    sm_msg_free(&post);

    /* Also verify sending to device still works */
    send_json(fd, sm_msg_send("s-post", (const uint8_t *)"ping\n", 5));
    usleep(50000);
    char buf[256];
    ssize_t n = read(ctx.master, buf, sizeof(buf));
    ASSERT(n > 0, "data reached device after resume");

    close(fd);
    teardown(&ctx);
}

/* T2: Client disconnect during active expect — broker must not crash */
static void test_disconnect_during_expect(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    int fd = connect_unix(TEST_SOCK);
    send_json(fd, sm_msg_hello("test", "controller"));
    sm_msg_t welcome = recv_json(fd);
    sm_msg_free(&welcome);

    /* Register an expect with long timeout */
    send_json(fd, sm_msg_send_expect("e-disc", (const uint8_t *)"cmd\n", 4,
                                      "never_match", 5000));
    usleep(50000);

    /* Drain the command from master */
    char buf[256];
    read(ctx.master, buf, sizeof(buf));

    /* Abruptly close the client while expect is still pending */
    close(fd);
    usleep(200000);

    /* Broker should still be alive — verify by connecting a new client */
    int fd2 = connect_unix(TEST_SOCK);
    ASSERT(fd2 >= 0, "broker alive after client disconnect during expect");
    send_json(fd2, sm_msg_hello("test2", "observer"));
    sm_msg_t w2 = recv_json(fd2);
    ASSERT_NOT_NULL(w2.root);
    ASSERT_INT_EQ(w2.type, SM_MSG_WELCOME);
    sm_msg_free(&w2);

    close(fd2);
    teardown(&ctx);
}

/* P4-01: coalesce must not double-free when sm_shared_line_new fails */
static void test_coalesce_shared_line_oom(void)
{
    sm_shared_line_test_reset_hooks();
    ASSERT_INT_EQ(sm_shared_line_test_fail_next, 0);

    int pair[2];
    ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair");

    sm_client_t *c = sm_client_new(pair[0], 1);
    ASSERT_NOT_NULL(c);
    c->hello_received = 1;

    cJSON *out1 = sm_msg_output((const uint8_t *)"aaa", 3, 1.0);
    size_t len1;
    char *line1 = sm_msg_encode(out1, &len1);
    cJSON_Delete(out1);
    ASSERT_NOT_NULL(line1);
    sm_shared_line_t *sl1 = sm_shared_line_new(line1, len1);
    ASSERT_NOT_NULL(sl1);
    ASSERT_INT_EQ(sm_client_send_shared(c, sl1), 0);
    sm_shared_line_release(sl1);

    const char *prev_data = c->write_queue[0].data;
    size_t prev_len = c->write_queue[0].len;
    sm_shared_line_t *prev_shared = c->write_queue[0].shared;
    ASSERT_INT_EQ((int)c->wq_count, 1);

    cJSON *out2 = sm_msg_output((const uint8_t *)"bbb", 3, 2.0);
    size_t len2;
    char *line2 = sm_msg_encode(out2, &len2);
    cJSON_Delete(out2);
    ASSERT_NOT_NULL(line2);
    sm_shared_line_t *sl2 = sm_shared_line_new(line2, len2);
    ASSERT_NOT_NULL(sl2);

    /* Fail the merged line alloc inside try_coalesce_output, not sl2 itself */
    sm_shared_line_test_fail_next = 1;
    ASSERT_INT_EQ(sm_client_send_shared(c, sl2), 0);
    sm_shared_line_release(sl2);

    ASSERT(c->write_queue[0].shared == prev_shared, "prev shared intact after OOM");
    ASSERT(c->write_queue[0].data == prev_data, "prev data intact after OOM");
    ASSERT_INT_EQ((int)c->write_queue[0].len, (int)prev_len);
    ASSERT_INT_EQ((int)c->wq_count, 2);

    sm_client_destroy(c);
    close(pair[1]);
    sm_shared_line_test_reset_hooks();
}

/* P4-01 ISSUE-1: refuse coalesce into partially-flushed head; stream intact */
static void test_coalesce_skips_partially_flushed_head(void)
{
    enum { RAW1 = 8192, RAW2 = 64 };
    uint8_t raw1[RAW1];
    uint8_t raw2[RAW2];
    memset(raw1, 'A', RAW1);
    memset(raw2, 'B', RAW2);

    int pair[2];
    ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair");

    int sndbuf = 2048;
    ASSERT(setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) == 0,
           "SO_SNDBUF");
    int flags = fcntl(pair[0], F_GETFL, 0);
    ASSERT(fcntl(pair[0], F_SETFL, flags | O_NONBLOCK) == 0, "client nonblock");
    flags = fcntl(pair[1], F_GETFL, 0);
    ASSERT(fcntl(pair[1], F_SETFL, flags | O_NONBLOCK) == 0, "peer nonblock");

    sm_client_t *c = sm_client_new(pair[0], 1);
    ASSERT_NOT_NULL(c);

    cJSON *out1 = sm_msg_output(raw1, RAW1, 1.0);
    size_t len1;
    char *line1 = sm_msg_encode(out1, &len1);
    cJSON_Delete(out1);
    ASSERT_NOT_NULL(line1);
    sm_shared_line_t *sl1 = sm_shared_line_new(line1, len1);
    ASSERT_NOT_NULL(sl1);
    ASSERT_INT_EQ(sm_client_send_shared(c, sl1), 0);
    sm_shared_line_release(sl1);

    int rc = sm_client_flush(c);
    ASSERT_INT_EQ(rc, 1);
    ASSERT_INT_EQ((int)c->wq_count, 1);
    size_t head_off = c->write_queue[c->wq_head].offset;
    size_t head_len = c->write_queue[c->wq_head].len;
    ASSERT(head_off > 0 && head_off < head_len, "partial flush offset in range");

    cJSON *out2 = sm_msg_output(raw2, RAW2, 2.0);
    size_t len2;
    char *line2 = sm_msg_encode(out2, &len2);
    cJSON_Delete(out2);
    ASSERT_NOT_NULL(line2);
    sm_shared_line_t *sl2 = sm_shared_line_new(line2, len2);
    ASSERT_NOT_NULL(sl2);
    ASSERT_INT_EQ(sm_client_send_shared(c, sl2), 0);
    sm_shared_line_release(sl2);

    ASSERT_INT_EQ((int)c->write_queue[c->wq_head].offset, (int)head_off);
    ASSERT_INT_EQ((int)c->wq_count, 2);

    char *wire1 = malloc(len1);
    char *wire2 = malloc(len2);
    ASSERT_NOT_NULL(wire1);
    ASSERT_NOT_NULL(wire2);
    memcpy(wire1, line1, len1);
    memcpy(wire2, line2, len2);

    char recv_buf[256 * 1024];
    size_t recv_len = 0;
    for (;;) {
        ssize_t pre = read(pair[1], recv_buf + recv_len, sizeof(recv_buf) - recv_len);
        if (pre <= 0)
            break;
        recv_len += (size_t)pre;
    }
    ASSERT_INT_EQ((int)recv_len, (int)head_off);

    for (int spin = 0; spin < 10000 && c->wq_count > 0; spin++) {
        ssize_t n = read(pair[1], recv_buf + recv_len, sizeof(recv_buf) - recv_len);
        if (n > 0)
            recv_len += (size_t)n;
        rc = sm_client_flush(c);
        ASSERT(rc >= 0, "flush ok while draining");
    }
    ASSERT_INT_EQ(rc, 0);
    flags = fcntl(pair[1], F_GETFL, 0);
    ASSERT(fcntl(pair[1], F_SETFL, flags & ~O_NONBLOCK) == 0, "peer blocking");
    while (recv_len < len1 + len2) {
        ssize_t n = read(pair[1], recv_buf + recv_len, len1 + len2 - recv_len);
        ASSERT(n > 0, "read remainder");
        recv_len += (size_t)n;
    }

    ASSERT_INT_EQ((int)c->wq_count, 0);
    ASSERT_INT_EQ((int)recv_len, (int)(len1 + len2));
    ASSERT(memcmp(recv_buf, wire1, len1) == 0, "wire bytes: first line");
    ASSERT(memcmp(recv_buf + len1, wire2, len2) == 0, "wire bytes: second line");
    free(wire1);
    free(wire2);

    size_t frame_count = 0;
    size_t start = 0;
    sm_msg_t frames[2];
    memset(frames, 0, sizeof(frames));
    for (size_t i = 0; i <= recv_len && frame_count < 2; i++) {
        if (i == recv_len || recv_buf[i] == '\n') {
            size_t flen = i - start;
            if (flen > 0) {
                frames[frame_count] = sm_msg_decode(recv_buf + start, flen);
                if (frames[frame_count].root)
                    frame_count++;
            }
            start = i + 1;
        }
    }
    ASSERT_INT_EQ((int)frame_count, 2);

    const char *b64_0 = sm_json_get_string(frames[0].root, "data");
    const char *b64_1 = sm_json_get_string(frames[1].root, "data");
    ASSERT_NOT_NULL(b64_0);
    ASSERT_NOT_NULL(b64_1);
    size_t dlen0, dlen1;
    uint8_t *dec0 = sm_base64_decode(b64_0, strlen(b64_0), &dlen0);
    uint8_t *dec1 = sm_base64_decode(b64_1, strlen(b64_1), &dlen1);
    ASSERT_NOT_NULL(dec0);
    ASSERT_NOT_NULL(dec1);
    ASSERT_INT_EQ(frames[0].type, SM_MSG_OUTPUT);
    ASSERT_INT_EQ(frames[1].type, SM_MSG_OUTPUT);
    ASSERT_INT_EQ((int)dlen0, RAW1);
    ASSERT_INT_EQ((int)dlen1, RAW2);
    ASSERT(memcmp(dec0, raw1, RAW1) == 0, "frame0 payload matches RAW1");
    ASSERT(memcmp(dec1, raw2, RAW2) == 0, "frame1 payload matches RAW2");
    free(dec0);
    free(dec1);
    sm_msg_free(&frames[0]);
    sm_msg_free(&frames[1]);

    sm_client_destroy(c);
    close(pair[1]);
}

/* Perf/robustness: the coalescer caps the merged head size, so a fully stalled
 * client's queue splits into bounded entries instead of one ever-growing entry
 * (which made the decode+re-encode O(K^2) in backlog depth). Verifies the size
 * bound AND that no output bytes are lost across the split. */
static void test_coalesce_head_size_cap(void)
{
    int pair[2];
    ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair");

    sm_client_t *c = sm_client_new(pair[0], 1);
    ASSERT_NOT_NULL(c);
    c->hello_received = 1;

    /* Never flush -> entries keep offset 0 -> coalescing engages every send. */
    enum { PAYLOAD = 512, LINES = 200 };
    uint8_t raw[PAYLOAD];
    memset(raw, 'Z', PAYLOAD);

    for (int i = 0; i < LINES; i++) {
        cJSON *m = sm_msg_output(raw, PAYLOAD, 1000.0 + i);
        size_t len;
        char *line = sm_msg_encode(m, &len);
        cJSON_Delete(m);
        ASSERT_NOT_NULL(line);
        sm_shared_line_t *sl = sm_shared_line_new(line, len);
        ASSERT_NOT_NULL(sl);
        ASSERT_INT_EQ(sm_client_send_shared(c, sl), 0);
        sm_shared_line_release(sl);
    }

    /* One encoded line, for the per-entry slack bound. */
    size_t one_line_encoded = 0;
    {
        cJSON *m = sm_msg_output(raw, PAYLOAD, 0.0);
        char *line = sm_msg_encode(m, &one_line_encoded);
        cJSON_Delete(m);
        free(line);
    }

    /* Cap engaged: the backlog is now several bounded entries, not one giant
     * merged entry (which pre-cap would have left wq_count == 1). */
    ASSERT(c->wq_count > 1, "cap split the backlog into multiple entries");
    for (size_t i = 0; i < c->wq_count; i++) {
        size_t slot = (c->wq_head + i) % c->wq_cap;
        ASSERT(c->write_queue[slot].len <=
                   SM_CLIENT_COALESCE_MAX_BYTES + one_line_encoded * 2,
               "queued entry bounded by the head cap");
    }

    /* Correctness: decoding every queued output message and summing the
     * payloads must reproduce all LINES*PAYLOAD original bytes, all intact. */
    size_t total_payload = 0;
    int all_z = 1;
    for (size_t i = 0; i < c->wq_count; i++) {
        size_t slot = (c->wq_head + i) % c->wq_cap;
        /* entry data ends with '\n'; sm_msg_decode wants the line without it */
        sm_msg_t msg = sm_msg_decode(c->write_queue[slot].data,
                                     c->write_queue[slot].len - 1);
        ASSERT(msg.root != NULL, "queued entry is a valid message");
        ASSERT_INT_EQ(msg.type, SM_MSG_OUTPUT);
        const char *b64 = sm_json_get_string(msg.root, "data");
        if (b64) {
            size_t dlen;
            uint8_t *dec = sm_base64_decode(b64, strlen(b64), &dlen);
            if (dec) {
                total_payload += dlen;
                for (size_t j = 0; j < dlen; j++)
                    if (dec[j] != 'Z') { all_z = 0; break; }
                free(dec);
            }
        }
        sm_msg_free(&msg);
    }
    ASSERT_INT_EQ((int)total_payload, PAYLOAD * LINES);
    ASSERT(all_z, "all payload bytes preserved across the split");

    sm_client_destroy(c);
    close(pair[1]);
}

/* T3: Write queue overflow — coalescing reduces drops; wq_drops in status */
static void test_write_queue_overflow(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    const int flood = SM_CLIENT_WRITE_QUEUE_SIZE + 50;

    int fd = connect_unix(TEST_SOCK);
    send_json(fd, sm_msg_hello("slowobs", "observer"));
    sm_msg_t welcome = recv_json(fd);
    sm_msg_free(&welcome);

    /* Slow consumer: stop reading; flood small consecutive output lines.
     * Coalescing merges them into one queue slot — far fewer drops than
     * one slot per line (would be flood - SM_CLIENT_WRITE_QUEUE_SIZE). */
    for (int i = 0; i < flood; i++) {
        write(ctx.master, "a\n", 2);
        usleep(1000);
    }
    usleep(200000);

    int stfd = connect_unix(TEST_SOCK);
    ASSERT(stfd >= 0, "broker alive after write queue pressure");
    send_json(stfd, sm_msg_hello("stctrl", "controller"));
    sm_msg_t st_w = recv_json(stfd);
    sm_msg_free(&st_w);

    send_json(stfd, sm_msg_status("wq-st"));
    sm_msg_t status = recv_json(stfd);
    ASSERT_NOT_NULL(status.root);
    ASSERT_INT_EQ(status.type, SM_MSG_STATUS_RESPONSE);

    int drops = status_client_int(status.root, "slowobs", "wq_drops");
    int wq_count = status_client_int(status.root, "slowobs", "wq_count");
    ASSERT(drops >= 0, "slowobs wq_drops exposed in status");
    ASSERT(wq_count >= 0, "slowobs wq_count exposed in status");
    ASSERT_INT_EQ(drops, 0);
    ASSERT(wq_count >= 1 && wq_count <= 2, "coalesced queue depth");
    ASSERT(drops < flood - SM_CLIENT_WRITE_QUEUE_SIZE,
           "coalescing reduced drop rate vs per-message enqueue");
    sm_msg_free(&status);

    close(stfd);
    close(fd);
    teardown(&ctx);
}

/* T6: USB-serial disconnect — close master PTY to cause EIO on slave reads */
static void test_link_disconnect_reconnect(void)
{
    test_ctx_t ctx;
    setup(&ctx);
    ctx.broker.reconnect = 1;

    int fd = connect_unix(TEST_SOCK);
    send_json(fd, sm_msg_hello("test", "observer"));
    sm_msg_t welcome = recv_json(fd);
    sm_msg_free(&welcome);

    /* Close the master side of the PTY. This causes EIO on reads
     * from the slave (which the UART link holds open). This is the
     * closest simulation of a USB-serial unplug with PTYs. */
    close(ctx.master);
    ctx.master = -1;

    /* Wait for broker to detect disconnect via EIO */
    usleep(300000);

    /* Look for link_down message */
    sm_msg_t msg;
    int got_link_down = 0;
    for (int i = 0; i < 20; i++) {
        msg = recv_json(fd);
        if (!msg.root) break;
        if (msg.type == SM_MSG_LINK_DOWN) {
            got_link_down = 1;
            sm_msg_free(&msg);
            break;
        }
        sm_msg_free(&msg);
    }
    ASSERT(got_link_down, "received link_down on device disconnect");

    /* Broker should still be alive */
    ASSERT(!ctx.broker.stopped, "broker still running after disconnect");
    ASSERT(ctx.broker.link_disconnected == 1, "link marked disconnected");

    close(fd);
    teardown(&ctx);
}

static double mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* Must match SM_LINK_DRAIN_MAX_READS in src/broker.c */
#define TEST_LINK_DRAIN_MAX_READS 16

static int g_stub_read_calls;

static ssize_t stub_read_zero_avail(int fd, void *buf, size_t len)
{
    (void)fd;
    (void)buf;
    (void)len;
    g_stub_read_calls++;
    return 0;
}

static int stub_avail_always_positive(int fd, int *avail)
{
    (void)fd;
    *avail = 4096;
    return 0;
}

/* P2-01: one sm_msg_encode per fan-out; shared-line pointer identity across clients */
static void test_broadcast_no_per_client_memcpy(void)
{
    /* Unit leg: direct sm_broker_broadcast_clients */
    sm_broker_test_broadcast_encode_count = 0;

    int pair[6];
    sm_client_t *unit_clients[3];
    for (int i = 0; i < 3; i++) {
        ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, pair + i * 2) == 0, "socketpair");
        unit_clients[i] = sm_client_new(pair[i * 2], (unsigned)i);
        ASSERT_NOT_NULL(unit_clients[i]);
        unit_clients[i]->hello_received = 1;
    }

    cJSON *out_msg = sm_msg_output((const uint8_t *)"unit", 4, 1000.0);
    ASSERT_INT_EQ(sm_broker_broadcast_clients(unit_clients, 3, out_msg, NULL), 3);
    ASSERT_INT_EQ((int)sm_broker_test_broadcast_encode_count, 1);

    sm_shared_line_t *shared0 = unit_clients[0]->write_queue[0].shared;
    ASSERT_NOT_NULL(shared0);
    ASSERT(unit_clients[1]->write_queue[0].shared == shared0,
           "client 1 shares broadcast line");
    ASSERT(unit_clients[2]->write_queue[0].shared == shared0,
           "client 2 shares broadcast line");

    cJSON_Delete(out_msg);
    for (int i = 0; i < 3; i++) {
        sm_client_destroy(unit_clients[i]);
        close(pair[i * 2 + 1]);
    }

    /* Integration leg: PTY device output → process_link_chunk → broadcast */
    test_ctx_t ctx;
    setup(&ctx);

    sm_broker_test_broadcast_encode_count = 0;

    int fds[3];
    for (int i = 0; i < 3; i++) {
        fds[i] = connect_unix(TEST_SOCK);
        char name[16];
        snprintf(name, sizeof(name), "obs%d", i);
        cJSON *hello = cJSON_CreateObject();
        cJSON_AddStringToObject(hello, "type", "hello");
        cJSON_AddStringToObject(hello, "name", name);
        cJSON_AddStringToObject(hello, "role", "observer");
        cJSON_AddNumberToObject(hello, "protocol_version", 1);
        send_json(fds[i], hello);
        sm_msg_t w = recv_json(fds[i]);
        sm_msg_free(&w);
    }

    write(ctx.master, "shared broadcast\n", 17);
    usleep(150000);

    ASSERT_INT_EQ((int)sm_broker_test_broadcast_encode_count, 1);

    for (int i = 0; i < 3; i++) {
        sm_msg_t out = recv_json(fds[i]);
        ASSERT_NOT_NULL(out.root);
        ASSERT_INT_EQ(out.type, SM_MSG_OUTPUT);
        sm_msg_free(&out);
        close(fds[i]);
    }

    teardown(&ctx);
}

static void test_send_payload_limit(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    int fd = connect_unix(TEST_SOCK);
    send_json(fd, sm_msg_hello("test", "controller"));
    sm_msg_t welcome = recv_json(fd);
    sm_msg_free(&welcome);

    size_t big_len = SM_MAX_SEND_B64_LEN + 64;
    char *big = malloc(big_len + 1);
    ASSERT_NOT_NULL(big);
    memset(big, 'A', big_len);
    big[big_len] = '\0';

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "send");
    cJSON_AddStringToObject(msg, "id", "big1");
    cJSON_AddStringToObject(msg, "data", big);
    send_json(fd, msg);
    free(big);

    sm_msg_t resp = recv_json(fd);
    ASSERT_NOT_NULL(resp.root);
    ASSERT_INT_EQ(resp.type, SM_MSG_ERROR);
    sm_msg_free(&resp);

    close(fd);
    teardown(&ctx);
}

static void test_suspend_clears_expects(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    int fd = connect_unix(TEST_SOCK);
    send_json(fd, sm_msg_hello("test", "controller"));
    sm_msg_t welcome = recv_json(fd);
    sm_msg_free(&welcome);

    cJSON *se = cJSON_CreateObject();
    cJSON_AddStringToObject(se, "type", "send_expect");
    cJSON_AddStringToObject(se, "id", "exp1");
    cJSON_AddStringToObject(se, "pattern", "never-match-xyz");
    cJSON_AddNumberToObject(se, "timeout_ms", 30000);
    send_json(fd, se);

    usleep(50000);
    ASSERT(ctx.broker.expect.count > 0, "expect registered");

    send_json(fd, sm_msg_suspend("sus1"));
    sm_msg_t sus = recv_json(fd);
    sm_msg_free(&sus);

    ASSERT_INT_EQ((int)ctx.broker.expect.count, 0);

    close(fd);
    teardown(&ctx);
}

static void test_link_drain_cap_enforced(void)
{
    g_stub_read_calls = 0;
    sm_broker_test_set_drain_hooks(stub_read_zero_avail, stub_avail_always_positive);

    int rc = sm_broker_test_drain_link_fd(0, TEST_LINK_DRAIN_MAX_READS, NULL, NULL);
    ASSERT_INT_EQ(rc, 0);
    ASSERT_INT_EQ(g_stub_read_calls, TEST_LINK_DRAIN_MAX_READS);

    sm_broker_test_reset_drain_hooks();
}

static void test_link_drain_until_eagain(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    enum { BURST = 32 * 1024 };
    uint8_t *burst = malloc(BURST);
    ASSERT_NOT_NULL(burst);
    memset(burst, 'A', BURST);

    double t0 = mono_ms();
    ssize_t wn = write(ctx.master, burst, BURST);
    ASSERT(wn == BURST, "wrote full burst to device");
    free(burst);

    /* Let the broker drain the PTY (read-until-EAGAIN) into history. */
    for (int i = 0; i < 50; i++) {
        if (ctx.broker.history.total_bytes >= BURST)
            break;
        usleep(20000);
    }

    double elapsed = mono_ms() - t0;
    ASSERT_INT_EQ((int)ctx.broker.history.total_bytes, BURST);
    ASSERT(elapsed < 2000.0, "32KB burst drained quickly");

    teardown(&ctx);
}

static void test_broker_probe(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    /* Board labels are read by the broker thread only inside handle_status,
     * which runs when the probe's status request arrives — well after this
     * write and the intervening socket handshake, so no data race in practice. */
    snprintf(ctx.broker.board, sizeof(ctx.broker.board), "testboard");
    snprintf(ctx.broker.role, sizeof(ctx.broker.role), "console");

    /* Probe the live broker (UART link over a PTY). */
    sm_broker_info_t info;
    int rc = sm_broker_probe(TEST_SOCK, &info, 1500);
    ASSERT_INT_EQ(rc, 0);
    ASSERT(info.reachable, "broker reachable");
    ASSERT_STR_EQ(info.link_type, "uart");
    ASSERT(info.endpoint[0] == '/', "endpoint is a device path");
    ASSERT(info.connected, "PTY link connected");
    ASSERT_STR_EQ(info.socket, TEST_SOCK);
    ASSERT_STR_EQ(info.board, "testboard");
    ASSERT_STR_EQ(info.role, "console");
    /* Broker runs in-process here, so SO_PEERCRED resolves to our own pid. */
    ASSERT_INT_EQ(info.pid, (int)getpid());

    teardown(&ctx);

    /* A dead socket path is cleanly reported unreachable. */
    rc = sm_broker_probe("/tmp/smolmux-test-nonexistent.sock", &info, 300);
    ASSERT_INT_EQ(rc, -1);
    ASSERT(!info.reachable, "nonexistent socket unreachable");
}

static void test_broker_discover_and_json(void)
{
    test_ctx_t ctx;
    setup(&ctx);   /* broker on TEST_SOCK — matches the /tmp/smolmux-*.sock glob */
    snprintf(ctx.broker.board, sizeof(ctx.broker.board), "grpboard");
    snprintf(ctx.broker.role, sizeof(ctx.broker.role), "swd");

    sm_broker_info_t infos[32];
    size_t n = sm_broker_discover(infos, 32, 1500);
    ASSERT(n >= 1, "discovery found at least the test broker");

    int idx = -1;
    for (size_t i = 0; i < n && i < 32; i++)
        if (strcmp(infos[i].socket, TEST_SOCK) == 0) idx = (int)i;
    ASSERT(idx >= 0, "test broker present in discovery");

    if (idx >= 0) {
        ASSERT(infos[idx].reachable, "discovered broker reachable");
        ASSERT_STR_EQ(infos[idx].link_type, "uart");

        /* JSON serialization carries the agent-facing fields. */
        cJSON *j = sm_broker_info_to_json(&infos[idx]);
        ASSERT_STR_EQ(sm_json_get_string(j, "socket"), TEST_SOCK);
        ASSERT_STR_EQ(sm_json_get_string(j, "link_type"), "uart");
        ASSERT(sm_json_get_bool(j, "reachable", 0), "json reachable=true");
        ASSERT(sm_json_get_string(j, "endpoint") != NULL, "json has endpoint");
        ASSERT_STR_EQ(sm_json_get_string(j, "board"), "grpboard");
        ASSERT_STR_EQ(sm_json_get_string(j, "role"), "swd");
        cJSON_Delete(j);
    }

    teardown(&ctx);

    /* Unreachable broker serializes to just socket + reachable:false. */
    sm_broker_info_t dead;
    sm_broker_probe("/tmp/smolmux-test-nonexistent.sock", &dead, 200);
    cJSON *dj = sm_broker_info_to_json(&dead);
    ASSERT(!sm_json_get_bool(dj, "reachable", 1), "json reachable=false");
    ASSERT_NULL(cJSON_GetObjectItem(dj, "link_type"));
    cJSON_Delete(dj);
}

static void test_history_request_budget(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    enum { BURST = 256 * 1024 };
    uint8_t *burst = malloc(BURST);
    ASSERT_NOT_NULL(burst);
    memset(burst, 'H', BURST);
    for (size_t off = 0; off < BURST; off += 4096) {
        size_t n = BURST - off > 4096 ? 4096 : BURST - off;
        write(ctx.master, burst + off, n);
        usleep(5000);
    }
    free(burst);
    usleep(300000);

    int fd1 = connect_unix(TEST_SOCK);
    send_json(fd1, sm_msg_hello("hist", "observer"));
    sm_msg_t w1 = recv_json(fd1);
    sm_msg_free(&w1);

    int fd2 = connect_unix(TEST_SOCK);
    send_json(fd2, sm_msg_hello("st", "observer"));
    sm_msg_t w2 = recv_json(fd2);
    sm_msg_free(&w2);

    send_json(fd1, sm_msg_history_request("h-budget", 0.0, 0));

    double t0 = mono_ms();
    send_json(fd2, sm_msg_status("st-budget"));
    sm_msg_t resp = recv_json(fd2);
    double elapsed = mono_ms() - t0;

    ASSERT_NOT_NULL(resp.root);
    ASSERT_INT_EQ(resp.type, SM_MSG_STATUS_RESPONSE);
    ASSERT(elapsed < 400.0, "status answered during incremental history encode");
    sm_msg_free(&resp);

    close(fd1);
    close(fd2);
    teardown(&ctx);
}

static void test_break_nonblocking(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    int fd = connect_unix(TEST_SOCK);
    send_json(fd, sm_msg_hello("test", "controller"));
    sm_msg_t welcome = recv_json(fd);
    sm_msg_free(&welcome);

    /* Schedule a long break, then immediately query status. The response
     * must arrive while the break is still in progress — proving the
     * event loop does not sleep through the break duration. */
    cJSON *pc = cJSON_CreateObject();
    cJSON_AddStringToObject(pc, "type", "pin_control");
    cJSON_AddStringToObject(pc, "id", "brk1");
    cJSON_AddStringToObject(pc, "pin", "break");
    cJSON_AddStringToObject(pc, "action", "pulse");
    cJSON_AddNumberToObject(pc, "duration_ms", 600);
    send_json(fd, pc);

    double t0 = mono_ms();
    send_json(fd, sm_msg_status("st1"));
    sm_msg_t resp = recv_json(fd);
    double elapsed = mono_ms() - t0;

    ASSERT_NOT_NULL(resp.root);
    ASSERT_INT_EQ(resp.type, SM_MSG_STATUS_RESPONSE);
    ASSERT(elapsed < 400.0, "status answered while break in progress");
    sm_msg_free(&resp);

    /* The pin_control ack arrives once the break completes */
    usleep(700000);
    sm_msg_t ack = recv_json(fd);
    ASSERT_NOT_NULL(ack.root);
    ASSERT_STR_EQ(sm_json_get_string(ack.root, "id"), "brk1");
    ASSERT_STR_EQ(sm_json_get_string(ack.root, "status"), "ok");
    sm_msg_free(&ack);

    close(fd);
    teardown(&ctx);
}

/* Read messages until one of type `want` arrives (or timeout). Returns the
 * parsed object (caller frees) or NULL. Accumulates across reads so multiple
 * queued lines aren't dropped. */
static cJSON *recv_type(int fd, const char *want, int max_ms)
{
    char buf[16384];
    size_t total = 0;
    for (int t = 0; t < max_ms / 10; t++) {
        ssize_t n = read(fd, buf + total, sizeof(buf) - total - 1);
        if (n > 0) {
            total += (size_t)n;
            buf[total] = '\0';
            char *start = buf, *nl;
            while ((nl = memchr(start, '\n', (size_t)(buf + total - start)))) {
                *nl = '\0';
                cJSON *j = cJSON_Parse(start);
                if (j) {
                    const char *ty = sm_json_get_string(j, "type");
                    if (ty && strcmp(ty, want) == 0) return j;
                    cJSON_Delete(j);
                }
                start = nl + 1;
            }
            size_t rem = (size_t)(buf + total - start);
            memmove(buf, start, rem);
            total = rem;
        } else {
            usleep(10000);
        }
    }
    return NULL;
}

static int count_char(const char *buf, ssize_t n, char ch)
{
    int c = 0;
    for (ssize_t i = 0; i < n; i++) if (buf[i] == ch) c++;
    return c;
}

/* The autoboot flood streams a key from inside the event loop and stops the
 * instant a broker-side match sees the prompt — the bootdelay=0 break. */
static void test_autoboot_flood_stops_on_match(void)
{
    test_ctx_t ctx;
    setup(&ctx);
    fcntl(ctx.master, F_SETFL, O_NONBLOCK);   /* reads must not block when idle */
    int fd = connect_unix(TEST_SOCK);
    ASSERT(fd >= 0, "connected");
    send_json(fd, sm_msg_hello("t", "controller"));
    sm_msg_t w = recv_json(fd);
    sm_msg_free(&w);

    /* Flood a space every 5ms, up to 3s, stop on the U-Boot prompt "=> ". */
    send_json(fd, sm_msg_interrupt_autoboot("ab1", (const uint8_t *)" ", 1,
                                            5, 3000, "=> "));

    /* Device side: the keys are streaming to us. */
    usleep(60000);
    char dev[512];
    ssize_t dn = read(ctx.master, dev, sizeof(dev));
    ASSERT(dn > 0 && count_char(dev, dn, ' ') > 0,
           "keystrokes stream to the device");

    /* The prompt appears — the broker-side matcher must halt the flood. */
    (void)!write(ctx.master, "\r\nU-Boot 2021.10\r\n=> ", 21);

    cJSON *res = recv_type(fd, "autoboot_result", 3000);
    ASSERT_NOT_NULL(res);
    if (res) {
        ASSERT(sm_json_get_bool(res, "matched", 0), "stopped on prompt match");
        ASSERT_STR_EQ(sm_json_get_string(res, "reason"), "matched");
        ASSERT(sm_json_get_int(res, "sent", 0) > 0, "keys were sent");
        cJSON_Delete(res);
    }

    /* No more keys after the match. */
    (void)read(ctx.master, dev, sizeof(dev));   /* drain in-flight */
    usleep(80000);
    ssize_t after = read(ctx.master, dev, sizeof(dev));
    ASSERT(after <= 0 || count_char(dev, after, ' ') == 0,
           "flooding halts after the match");

    close(fd);
    teardown(&ctx);
}

/* With no matching prompt, the flood stops at the duration cap. */
static void test_autoboot_flood_timeout(void)
{
    test_ctx_t ctx;
    setup(&ctx);
    int fd = connect_unix(TEST_SOCK);
    send_json(fd, sm_msg_hello("t", "controller"));
    sm_msg_t w = recv_json(fd);
    sm_msg_free(&w);

    send_json(fd, sm_msg_interrupt_autoboot("ab2", (const uint8_t *)" ", 1,
                                            5, 300, "NEVERMATCHXYZ"));
    cJSON *res = recv_type(fd, "autoboot_result", 2000);
    ASSERT_NOT_NULL(res);
    if (res) {
        ASSERT(!sm_json_get_bool(res, "matched", 1), "no match within duration");
        ASSERT_STR_EQ(sm_json_get_string(res, "reason"), "timeout");
        ASSERT(sm_json_get_int(res, "sent", 0) > 0, "keys were sent");
        cJSON_Delete(res);
    }

    close(fd);
    teardown(&ctx);
}

/* Boot-stage progress: device output advances the tracker, the broker
 * broadcasts a boot_stage event, and status_response carries the boot object. */
static void test_boot_stage_progress(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    /* Declare a two-stage pipeline on the running broker. No device data has
     * flowed yet, so this races nothing the broker reads. */
    sm_boot_add_stage(&ctx.broker.boot, "uboot", "U-Boot 20");
    sm_boot_add_stage(&ctx.broker.boot, "login", "login:");

    int obs = connect_unix(TEST_SOCK);
    send_json(obs, sm_msg_hello("obs", "observer"));
    sm_msg_t w = recv_json(obs);
    sm_msg_free(&w);

    /* Device reaches the U-Boot stage. */
    (void)!write(ctx.master, "U-Boot 2024.01 (build)\r\n", 24);

    cJSON *bs = recv_type(obs, "boot_stage", 2000);
    ASSERT_NOT_NULL(bs);
    if (bs) {
        ASSERT_STR_EQ(sm_json_get_string(bs, "name"), "uboot");
        ASSERT_INT_EQ(sm_json_get_int(bs, "index", -1), 0);
        ASSERT_INT_EQ(sm_json_get_int(bs, "total", -1), 2);
        cJSON_Delete(bs);
    }

    /* status_response carries a boot object: furthest=0, login not reached. */
    send_json(obs, sm_msg_status("s1"));
    cJSON *st = recv_type(obs, "status_response", 2000);
    ASSERT_NOT_NULL(st);
    if (st) {
        cJSON *boot = cJSON_GetObjectItem(st, "boot");
        ASSERT_NOT_NULL(boot);
        if (boot) {
            ASSERT_INT_EQ(sm_json_get_int(boot, "furthest", -99), 0);
            ASSERT_INT_EQ(sm_json_get_int(boot, "total", -1), 2);
            ASSERT_INT_EQ(sm_json_get_bool(boot, "terminal_reached", 1), 0);
            cJSON *stages = cJSON_GetObjectItem(boot, "stages");
            ASSERT(cJSON_IsArray(stages) && cJSON_GetArraySize(stages) == 2,
                   "two stages reported");
        }
        cJSON_Delete(st);
    }

    close(obs);
    teardown(&ctx);
}

/* --- Fake link for reset_and_interrupt ---
 * Data flows over a socketpair (so epoll + key streaming behave like a real
 * link), but set_param() succeeds and records dtr/rts transitions so a test can
 * assert the assert->release sequence a PTY can't (PTYs reject modem ioctls). */
typedef struct fake_link_data {
    int fd;                 /* broker side of the socketpair */
    char pin_log[16][16];   /* recorded "pin=action" set_param calls */
    int  pin_log_n;
} fake_link_data_t;

static int   fk_open(sm_link_t *s) { (void)s; return 0; }
static void  fk_close(sm_link_t *s) { (void)s; }
static int   fk_read_fd(sm_link_t *s) { return ((fake_link_data_t *)s->data)->fd; }
static int   fk_write_fd(sm_link_t *s) { return ((fake_link_data_t *)s->data)->fd; }
static int   fk_write_data(sm_link_t *s, const uint8_t *d, size_t n) {
    ssize_t w = write(((fake_link_data_t *)s->data)->fd, d, n);
    return w < 0 ? -1 : (int)w;
}
static int   fk_has_write_pending(sm_link_t *s) { (void)s; return 0; }
static int   fk_flush(sm_link_t *s) { (void)s; return 0; }
static int   fk_send_break(sm_link_t *s, int ms) { (void)s; (void)ms; return 0; }
static int   fk_set_param(sm_link_t *s, const char *k, const char *v) {
    fake_link_data_t *d = s->data;
    if (d->pin_log_n < 16)
        snprintf(d->pin_log[d->pin_log_n++], 16, "%s=%s", k, v);
    return 0;
}
static int   fk_get_status(sm_link_t *s, cJSON *o) { (void)s; (void)o; return 0; }
static void  fk_destroy(sm_link_t *s) { free(s->data); free(s); }

static sm_link_t *fake_link_new(int broker_fd)
{
    fake_link_data_t *d = calloc(1, sizeof(*d));
    d->fd = broker_fd;
    sm_link_t *l = calloc(1, sizeof(*l));
    l->name = "fake";
    l->open = fk_open; l->close = fk_close;
    l->read_fd = fk_read_fd; l->write_fd = fk_write_fd;
    l->write_data = fk_write_data; l->has_write_pending = fk_has_write_pending;
    l->flush_write_queue = fk_flush; l->send_break = fk_send_break;
    l->set_param = fk_set_param; l->get_status = fk_get_status;
    l->destroy = fk_destroy;
    l->silence_normal = 1;   /* skip idle health checks */
    l->data = d;
    return l;
}

/* reset_and_interrupt: the broker asserts the reset line, streams keys, and
 * releases the line after the hold — verified via the fake link's pin log. */
static void test_reset_and_interrupt(void)
{
    int sp[2];
    ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0, "socketpair");
    fcntl(sp[0], F_SETFL, O_NONBLOCK);
    fcntl(sp[1], F_SETFL, O_NONBLOCK);

    sm_link_t *link = fake_link_new(sp[0]);
    fake_link_data_t *ld = link->data;

    sm_broker_t broker;
    sm_broker_init(&broker, link, TEST_SOCK);
    snprintf(broker.port, sizeof(broker.port), "fake");
    pthread_t tid;
    pthread_create(&tid, NULL, broker_thread, &broker);
    usleep(STARTUP_DELAY);

    int fd = connect_unix(TEST_SOCK);
    send_json(fd, sm_msg_hello("t", "controller"));
    sm_msg_t w = recv_json(fd);
    sm_msg_free(&w);

    /* Reset via DTR, active-low (assert=clear), 80ms hold, 400ms flood. */
    cJSON *m = sm_msg_interrupt_autoboot("rai1", (const uint8_t *)" ", 1,
                                         5, 400, "NEVERMATCHXYZ");
    cJSON_AddStringToObject(m, "reset_pin", "dtr");
    cJSON_AddStringToObject(m, "reset_assert", "clear");
    cJSON_AddNumberToObject(m, "reset_hold_ms", 80);
    send_json(fd, m);   /* takes ownership of m */

    cJSON *res = recv_type(fd, "autoboot_result", 2000);
    ASSERT_NOT_NULL(res);
    if (res) {
        ASSERT(sm_json_get_int(res, "sent", 0) > 0, "keys streamed during reset");
        cJSON_Delete(res);
    }

    /* Pin log must be exactly: assert (dtr=clear) then release (dtr=set). */
    ASSERT_INT_EQ(ld->pin_log_n, 2);
    if (ld->pin_log_n >= 2) {
        ASSERT_STR_EQ(ld->pin_log[0], "dtr=clear");
        ASSERT_STR_EQ(ld->pin_log[1], "dtr=set");
    }

    close(fd);
    sm_broker_stop(&broker);
    pthread_join(tid, NULL);
    sm_broker_destroy(&broker);   /* frees the fake link */
    close(sp[1]);
}

/* reset_and_interrupt with hold >= duration is rejected (flood must outlast the
 * reset hold, else keys stop before the device boots). */
static void test_reset_and_interrupt_bad_hold(void)
{
    int sp[2];
    ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0, "socketpair");
    fcntl(sp[0], F_SETFL, O_NONBLOCK);

    sm_link_t *link = fake_link_new(sp[0]);
    fake_link_data_t *ld = link->data;

    sm_broker_t broker;
    sm_broker_init(&broker, link, TEST_SOCK);
    snprintf(broker.port, sizeof(broker.port), "fake");
    pthread_t tid;
    pthread_create(&tid, NULL, broker_thread, &broker);
    usleep(STARTUP_DELAY);

    int fd = connect_unix(TEST_SOCK);
    send_json(fd, sm_msg_hello("t", "controller"));
    sm_msg_t w = recv_json(fd);
    sm_msg_free(&w);

    cJSON *m = sm_msg_interrupt_autoboot("rai2", (const uint8_t *)" ", 1,
                                         5, 100, "x");
    cJSON_AddStringToObject(m, "reset_pin", "dtr");
    cJSON_AddNumberToObject(m, "reset_hold_ms", 200);   /* >= duration 100 */
    send_json(fd, m);   /* takes ownership of m */

    cJSON *err = recv_type(fd, "error", 1500);
    ASSERT_NOT_NULL(err);
    if (err) cJSON_Delete(err);
    /* Reset line must never have been touched on a rejected request. */
    ASSERT_INT_EQ(ld->pin_log_n, 0);

    close(fd);
    sm_broker_stop(&broker);
    pthread_join(tid, NULL);
    sm_broker_destroy(&broker);
    close(sp[1]);
}

/* An autoresponder rule matches device output in the read path and writes its
 * response to the device, broadcasting autoresponder_fired. */
static void test_autoresponder_fires(void)
{
    test_ctx_t ctx;
    setup(&ctx);
    fcntl(ctx.master, F_SETFL, O_NONBLOCK);   /* master is blocking by default */

    int fd = connect_unix(TEST_SOCK);
    send_json(fd, sm_msg_hello("c", "controller"));
    sm_msg_t w = recv_json(fd);
    sm_msg_free(&w);

    /* On "Press any key", send a space. */
    send_json(fd, sm_msg_configure_autoresponder("c1", "menu", "Press any key",
                                                 (const uint8_t *)" ", 1, 0, 1000, 0));
    cJSON *ack = recv_type(fd, "configure_autoresponder", 2000);
    ASSERT_NOT_NULL(ack);
    if (ack) cJSON_Delete(ack);

    /* Device prints the menu prompt. */
    (void)!write(ctx.master, "booting\r\nPress any key to stop autoboot\r\n", 40);

    /* Broker broadcasts autoresponder_fired... */
    cJSON *fired = recv_type(fd, "autoresponder_fired", 2000);
    ASSERT_NOT_NULL(fired);
    if (fired) {
        ASSERT_STR_EQ(sm_json_get_string(fired, "name"), "menu");
        ASSERT_INT_EQ(sm_json_get_int(fired, "sent", 0), 1);
        cJSON_Delete(fired);
    }

    /* ...and the response byte reaches the device (readable on the master). */
    usleep(50000);
    char dev[64];
    ssize_t rn = read(ctx.master, dev, sizeof(dev));
    int saw_space = 0;
    for (ssize_t i = 0; i < rn; i++)
        if (dev[i] == ' ') saw_space = 1;
    ASSERT(saw_space, "space response written to the device");

    close(fd);
    teardown(&ctx);
}

/* A boot that advances then hangs before the terminal stage fires exactly one
 * proactive boot_stall event after the (short) stall timeout. */
static void test_boot_stall_fires(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    sm_boot_add_stage(&ctx.broker.boot, "uboot", "U-Boot 20");
    sm_boot_add_stage(&ctx.broker.boot, "login", "login:");
    sm_boot_set_stall_timeout(&ctx.broker.boot, 250);  /* short for the test */

    int obs = connect_unix(TEST_SOCK);
    send_json(obs, sm_msg_hello("obs", "observer"));
    sm_msg_t w = recv_json(obs);
    sm_msg_free(&w);

    /* Reach U-Boot, then send nothing further — the boot is stuck. */
    (void)!write(ctx.master, "U-Boot 2024.01\r\n", 16);

    cJSON *stall = recv_type(obs, "boot_stall", 2000);
    ASSERT_NOT_NULL(stall);
    if (stall) {
        ASSERT_STR_EQ(sm_json_get_string(stall, "name"), "uboot");
        ASSERT_INT_EQ(sm_json_get_int(stall, "index", -1), 0);
        ASSERT_INT_EQ(sm_json_get_int(stall, "total", -1), 2);
        ASSERT(sm_json_get_int(stall, "stalled_ms", 0) > 0, "stalled_ms reported");
        cJSON_Delete(stall);
    }

    close(obs);
    teardown(&ctx);
}

/* A boot that reaches the terminal stage must NOT fire a stall event. */
static void test_boot_no_stall_when_complete(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    sm_boot_add_stage(&ctx.broker.boot, "uboot", "U-Boot 20");
    sm_boot_add_stage(&ctx.broker.boot, "login", "login:");
    sm_boot_set_stall_timeout(&ctx.broker.boot, 250);

    int obs = connect_unix(TEST_SOCK);
    send_json(obs, sm_msg_hello("obs", "observer"));
    sm_msg_t w = recv_json(obs);
    sm_msg_free(&w);

    /* Reach the terminal stage promptly. */
    (void)!write(ctx.master, "U-Boot 2024.01\r\nboard login:", 28);

    /* Wait well past the stall timeout: no boot_stall should ever arrive. */
    cJSON *stall = recv_type(obs, "boot_stall", 700);
    ASSERT(stall == NULL, "no stall after full boot");
    if (stall) cJSON_Delete(stall);

    close(obs);
    teardown(&ctx);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    setlinebuf(stdout);
    sm_shared_line_test_reset_hooks();
    printf("test_broker\n");

    RUN_TEST(test_start_stop);
    RUN_TEST(test_connect_hello);
    RUN_TEST(test_send_receive);
    RUN_TEST(test_observer_cannot_send);
    RUN_TEST(test_send_expect);
    RUN_TEST(test_send_expect_timeout);
    RUN_TEST(test_takeover_release);
    RUN_TEST(test_status_query);
    RUN_TEST(test_broker_probe);
    RUN_TEST(test_broker_discover_and_json);
    RUN_TEST(test_history_request);
    RUN_TEST(test_history_request_budget);
    RUN_TEST(test_suspend_resume);
    RUN_TEST(test_broadcast_to_observer);
    RUN_TEST(test_broadcast_no_per_client_memcpy);
    RUN_TEST(test_send_payload_limit);
    RUN_TEST(test_suspend_clears_expects);
    RUN_TEST(test_suspend_resume_data_flow);
    RUN_TEST(test_disconnect_during_expect);
    RUN_TEST(test_coalesce_shared_line_oom);
    RUN_TEST(test_coalesce_skips_partially_flushed_head);
    RUN_TEST(test_coalesce_head_size_cap);
    RUN_TEST(test_write_queue_overflow);
    RUN_TEST(test_link_disconnect_reconnect);
    RUN_TEST(test_link_drain_cap_enforced);
    RUN_TEST(test_link_drain_until_eagain);
    RUN_TEST(test_break_nonblocking);
    RUN_TEST(test_autoboot_flood_stops_on_match);
    RUN_TEST(test_autoboot_flood_timeout);
    RUN_TEST(test_reset_and_interrupt);
    RUN_TEST(test_reset_and_interrupt_bad_hold);
    RUN_TEST(test_autoresponder_fires);
    RUN_TEST(test_boot_stage_progress);
    RUN_TEST(test_boot_stall_fires);
    RUN_TEST(test_boot_no_stall_when_complete);
    RUN_TEST(test_broker_listens_on_derived_long_byid_socket);
    RUN_TEST(test_stop_by_socket);
    RUN_TEST(test_stop_by_socket_stale);
    RUN_TEST(test_find_broker_for_endpoint);

    TEST_REPORT();
}
