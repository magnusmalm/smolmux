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

int main(void)
{
    printf("test_monitor\n");

    RUN_TEST(test_connect_observe);
    RUN_TEST(test_send_keystroke);
    RUN_TEST(test_role_upgrade_rejected);
    RUN_TEST(test_status_request);

    TEST_REPORT();
}
