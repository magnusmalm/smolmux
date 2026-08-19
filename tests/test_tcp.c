#include "test_main.h"
#include "broker.h"
#include "links/uart.h"
#include "sinks/tcp.h"
#include "protocol.h"
#include "util/base64.h"
#include "util/json_helpers.h"

#include <pthread.h>
#include <unistd.h>
#include <pty.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>

#define TEST_SOCK "/tmp/smolmux-test-tcp.sock"
#define TEST_TCP_PORT 15555
#define STARTUP_DELAY 150000  /* 150ms */

/* --- Helpers --- */

static void *broker_thread(void *arg)
{
    sm_broker_t *b = arg;
    sm_broker_run(b);
    return NULL;
}

static int connect_tcp(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);

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
    sm_sink_t *tcp_sink;
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

    ctx->tcp_sink = sm_tcp_sink_new(TEST_TCP_PORT, NULL);
    sm_broker_add_sink(&ctx->broker, ctx->tcp_sink);

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

static void test_tcp_connect_hello(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    int fd = connect_tcp(TEST_TCP_PORT);
    ASSERT(fd >= 0, "connected via TCP");

    send_json(fd, sm_msg_hello("tcp-test", "controller"));
    sm_msg_t resp = recv_json(fd);
    ASSERT_NOT_NULL(resp.root);
    ASSERT_INT_EQ(resp.type, SM_MSG_WELCOME);
    ASSERT_STR_EQ(sm_json_get_string(resp.root, "your_role"), "controller");
    sm_msg_free(&resp);

    close(fd);
    teardown(&ctx);
}

static void test_tcp_send_receive(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    int fd = connect_tcp(TEST_TCP_PORT);
    send_json(fd, sm_msg_hello("tcp-test", "controller"));
    sm_msg_t welcome = recv_json(fd);
    sm_msg_free(&welcome);

    /* Send data to device via TCP */
    send_json(fd, sm_msg_send("s1", (const uint8_t *)"hello\n", 6));

    /* Verify it arrived at PTY master */
    usleep(50000);
    char buf[256];
    ssize_t n = read(ctx.master, buf, sizeof(buf));
    ASSERT(n > 0, "data reached device");
    ASSERT(memcmp(buf, "hello\n", 6) == 0, "data matches");

    close(fd);
    teardown(&ctx);
}

static void test_tcp_output_broadcast(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    int fd = connect_tcp(TEST_TCP_PORT);
    send_json(fd, sm_msg_hello("tcp-test", "observer"));
    sm_msg_t welcome = recv_json(fd);
    sm_msg_free(&welcome);

    /* Device sends output */
    write(ctx.master, "device output\n", 14);
    usleep(100000);

    /* TCP client should receive output */
    sm_msg_t out = recv_json(fd);
    ASSERT_NOT_NULL(out.root);
    ASSERT_INT_EQ(out.type, SM_MSG_OUTPUT);
    sm_msg_free(&out);

    close(fd);
    teardown(&ctx);
}

static void test_tcp_auth_token(void)
{
    test_ctx_t ctx;
    setup(&ctx);
    snprintf(ctx.broker.auth_token, sizeof(ctx.broker.auth_token), "sekrit");

    /* Missing token → error, no welcome */
    unsetenv("SMOLMUX_AUTH_TOKEN");
    int fd = connect_tcp(TEST_TCP_PORT);
    ASSERT(fd >= 0, "connected via TCP");
    send_json(fd, sm_msg_hello("intruder", "controller"));
    sm_msg_t resp = recv_json(fd);
    ASSERT_NOT_NULL(resp.root);
    ASSERT_INT_EQ(resp.type, SM_MSG_ERROR);
    sm_msg_free(&resp);
    close(fd);

    /* Correct token (picked up from env by sm_msg_hello) → welcome */
    setenv("SMOLMUX_AUTH_TOKEN", "sekrit", 1);
    int fd2 = connect_tcp(TEST_TCP_PORT);
    send_json(fd2, sm_msg_hello("friend", "controller"));
    unsetenv("SMOLMUX_AUTH_TOKEN");
    sm_msg_t resp2 = recv_json(fd2);
    ASSERT_NOT_NULL(resp2.root);
    ASSERT_INT_EQ(resp2.type, SM_MSG_WELCOME);
    sm_msg_free(&resp2);
    close(fd2);

    teardown(&ctx);
}

int main(void)
{
    printf("test_tcp\n");

    RUN_TEST(test_tcp_connect_hello);
    RUN_TEST(test_tcp_send_receive);
    RUN_TEST(test_tcp_output_broadcast);
    RUN_TEST(test_tcp_auth_token);

    TEST_REPORT();
}
