#include "test_main.h"
#include "broker.h"
#include "links/uart.h"
#include "sinks/ws.h"
#include "protocol.h"
#include "util/base64.h"
#include "util/sha1.h"
#include "util/json_helpers.h"

#include <pthread.h>
#include <unistd.h>
#include <pty.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>

#define TEST_SOCK "/tmp/smolmux-test-ws.sock"
#define TEST_WS_PORT 15556
#define STARTUP_DELAY 150000  /* 150ms */
#define WS_GUID "258EAFA5-E914-47DA-95CA-5AB5443F11F3"

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
    return fd;
}

/* Write all bytes */
static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, p + off, len - off);
        if (w <= 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

/* Read with timeout */
static ssize_t read_timeout(int fd, void *buf, size_t len, int timeout_ms)
{
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    if (poll(&pfd, 1, timeout_ms) <= 0) return -1;
    return read(fd, buf, len);
}

/* Perform WS handshake, returns 0 on success */
static int ws_handshake(int fd)
{
    const char *key = "dGhlIHNhbXBsZSBub25jZQ=="; /* standard test key */
    char req[512];
    int rlen = snprintf(req, sizeof(req),
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n", key);

    if (write_all(fd, req, (size_t)rlen) < 0) return -1;

    /* Read 101 response */
    char resp[4096];
    size_t total = 0;
    for (int i = 0; i < 50; i++) {
        ssize_t n = read_timeout(fd, resp + total, sizeof(resp) - total - 1, 100);
        if (n > 0) {
            total += (size_t)n;
            resp[total] = '\0';
            if (strstr(resp, "\r\n\r\n")) break;
        }
    }

    if (!strstr(resp, "101")) return -1;
    return 0;
}

/* Encode a masked WS text frame */
static size_t ws_encode_masked(uint8_t *out, size_t out_cap,
                                const uint8_t *data, size_t len)
{
    size_t hdr;
    if (len < 126)        hdr = 2;
    else if (len < 65536) hdr = 4;
    else                  hdr = 10;

    if (hdr + 4 + len > out_cap) return 0;

    out[0] = 0x81; /* FIN + TEXT */
    if (len < 126) {
        out[1] = (uint8_t)(0x80 | len); /* MASK bit + len */
    } else if (len < 65536) {
        out[1] = 0x80 | 126;
        out[2] = (uint8_t)(len >> 8);
        out[3] = (uint8_t)(len);
    } else {
        out[1] = 0x80 | 127;
        for (int i = 0; i < 8; i++)
            out[2 + i] = (uint8_t)(len >> (56 - 8 * i));
    }

    /* Masking key (simple fixed key for test) */
    uint8_t mask[4] = {0x37, 0xfa, 0x21, 0x3d};
    memcpy(out + hdr, mask, 4);

    for (size_t i = 0; i < len; i++)
        out[hdr + 4 + i] = data[i] ^ mask[i & 3];

    return hdr + 4 + len;
}

/* Decode a server WS frame (unmasked). Returns payload in *payload, *plen. */
static size_t ws_decode_server(const uint8_t *buf, size_t len,
                               int *opcode, const uint8_t **payload, size_t *plen)
{
    if (len < 2) return 0;

    *opcode = buf[0] & 0x0F;
    uint64_t pl = buf[1] & 0x7F;
    size_t hdr = 2;

    if (pl == 126) {
        if (len < 4) return 0;
        pl = ((uint64_t)buf[2] << 8) | buf[3];
        hdr = 4;
    } else if (pl == 127) {
        if (len < 10) return 0;
        pl = 0;
        for (int i = 0; i < 8; i++)
            pl = (pl << 8) | buf[2 + i];
        hdr = 10;
    }

    if (len < hdr + (size_t)pl) return 0;

    *payload = buf + hdr;
    *plen = (size_t)pl;
    return hdr + (size_t)pl;
}

/* Send a JSON message as a masked WS text frame */
static void ws_send_json(int fd, cJSON *msg)
{
    size_t jlen;
    char *line = sm_msg_encode(msg, &jlen);
    /* Strip trailing newline for WS frame */
    if (jlen > 0 && line[jlen - 1] == '\n') jlen--;

    uint8_t frame[8192];
    size_t flen = ws_encode_masked(frame, sizeof(frame),
                                    (const uint8_t *)line, jlen);
    write_all(fd, frame, flen);
    free(line);
    cJSON_Delete(msg);
}

/* Receive a JSON message from a WS text frame */
static sm_msg_t ws_recv_json(int fd)
{
    uint8_t buf[8192];
    size_t total = 0;

    for (int i = 0; i < 50; i++) {
        ssize_t n = read_timeout(fd, buf + total, sizeof(buf) - total, 100);
        if (n > 0) {
            total += (size_t)n;
            /* Try to decode a frame */
            int op;
            const uint8_t *payload;
            size_t plen;
            if (ws_decode_server(buf, total, &op, &payload, &plen) > 0) {
                return sm_msg_decode((const char *)payload, plen);
            }
        }
    }

    sm_msg_t empty = {SM_MSG_UNKNOWN, NULL};
    return empty;
}

typedef struct test_ctx {
    int master;
    int slave;
    sm_broker_t broker;
    sm_link_t *link;
    sm_sink_t *ws_sink;
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

    ctx->ws_sink = sm_ws_sink_new(TEST_WS_PORT);
    sm_broker_add_sink(&ctx->broker, ctx->ws_sink);

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

static void test_ws_handshake(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    int fd = connect_tcp(TEST_WS_PORT);
    ASSERT(fd >= 0, "connected via TCP");

    int rc = ws_handshake(fd);
    ASSERT_INT_EQ(rc, 0);

    close(fd);
    teardown(&ctx);
}

static void test_ws_send_receive(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    int fd = connect_tcp(TEST_WS_PORT);
    ASSERT(fd >= 0, "connected");
    ASSERT_INT_EQ(ws_handshake(fd), 0);

    /* Send hello via WS */
    ws_send_json(fd, sm_msg_hello("ws-test", "controller"));
    sm_msg_t welcome = ws_recv_json(fd);
    ASSERT_NOT_NULL(welcome.root);
    ASSERT_INT_EQ(welcome.type, SM_MSG_WELCOME);
    sm_msg_free(&welcome);

    /* Send data to device via WS */
    ws_send_json(fd, sm_msg_send("s1", (const uint8_t *)"hello\n", 6));

    /* Verify it arrived at PTY master */
    usleep(100000);
    char buf[256];
    ssize_t n = read(ctx.master, buf, sizeof(buf));
    ASSERT(n > 0, "data reached device");
    ASSERT(memcmp(buf, "hello\n", 6) == 0, "data matches");

    close(fd);
    teardown(&ctx);
}

static void test_ws_output_broadcast(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    int fd = connect_tcp(TEST_WS_PORT);
    ASSERT(fd >= 0, "connected");
    ASSERT_INT_EQ(ws_handshake(fd), 0);

    ws_send_json(fd, sm_msg_hello("ws-test", "observer"));
    sm_msg_t welcome = ws_recv_json(fd);
    ASSERT_NOT_NULL(welcome.root);
    sm_msg_free(&welcome);

    /* Device sends output */
    write(ctx.master, "device output\n", 14);
    usleep(200000);

    /* WS client should receive output as WS text frame */
    sm_msg_t out = ws_recv_json(fd);
    ASSERT_NOT_NULL(out.root);
    ASSERT_INT_EQ(out.type, SM_MSG_OUTPUT);
    sm_msg_free(&out);

    close(fd);
    teardown(&ctx);
}

/* T5: Send a WS close frame — server should handle cleanly */
static void test_ws_close_frame(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    int fd = connect_tcp(TEST_WS_PORT);
    ASSERT(fd >= 0, "connected");
    ASSERT_INT_EQ(ws_handshake(fd), 0);

    /* Complete hello/welcome handshake */
    ws_send_json(fd, sm_msg_hello("ws-close-test", "observer"));
    sm_msg_t welcome = ws_recv_json(fd);
    ASSERT_NOT_NULL(welcome.root);
    sm_msg_free(&welcome);

    /* Send a close frame (opcode 0x8) with status code 1000 (normal closure) */
    uint8_t close_payload[2] = {0x03, 0xE8}; /* 1000 in big-endian */
    uint8_t frame[32];
    size_t hdr = 2;
    frame[0] = 0x88; /* FIN + CLOSE */
    frame[1] = 0x80 | 2; /* MASK bit + payload len 2 */
    uint8_t mask[4] = {0x37, 0xfa, 0x21, 0x3d};
    memcpy(frame + hdr, mask, 4);
    for (size_t i = 0; i < 2; i++)
        frame[hdr + 4 + i] = close_payload[i] ^ mask[i & 3];
    write_all(fd, frame, hdr + 4 + 2);

    usleep(200000);

    /* Broker should still be alive — connect another client */
    int fd2 = connect_tcp(TEST_WS_PORT);
    ASSERT(fd2 >= 0, "broker alive after WS close frame");
    ASSERT_INT_EQ(ws_handshake(fd2), 0);

    ws_send_json(fd2, sm_msg_hello("ws-after-close", "observer"));
    sm_msg_t w2 = ws_recv_json(fd2);
    ASSERT_NOT_NULL(w2.root);
    ASSERT_INT_EQ(w2.type, SM_MSG_WELCOME);
    sm_msg_free(&w2);

    close(fd);
    close(fd2);
    teardown(&ctx);
}

/* A frame whose total size (header + mask + payload) exceeds the WS read
 * buffer can never fully buffer. Pre-fix it passed the plen<=BUF check, the
 * decoder returned "incomplete" forever, the next read got 0 bytes, and the
 * client was dropped as a silent EOF. It must now be rejected with a WS close
 * (1009), and the broker must stay alive. */
static void test_ws_oversized_frame_rejected(void)
{
    test_ctx_t ctx;
    setup(&ctx);

    int fd = connect_tcp(TEST_WS_PORT);
    ASSERT(fd >= 0, "connected");
    ASSERT_INT_EQ(ws_handshake(fd), 0);

    /* total = hdr(4) + mask(4) + payload; pick payload so total = BUF + 1. */
    size_t payload_len = SM_WS_READ_BUF_SIZE - 8 + 1;
    uint8_t *payload = malloc(payload_len);
    uint8_t *frame = malloc(payload_len + 16);
    ASSERT_NOT_NULL(payload);
    ASSERT_NOT_NULL(frame);
    memset(payload, 'x', payload_len);
    size_t flen = ws_encode_masked(frame, payload_len + 16, payload, payload_len);
    ASSERT(flen > 0, "encoded oversized frame");
    ASSERT_INT_EQ(write_all(fd, frame, flen), 0);

    /* Expect a server WS close frame (opcode 0x8), not a silent drop. */
    uint8_t rbuf[64];
    size_t total = 0;
    int got_close = 0;
    for (int i = 0; i < 50 && !got_close; i++) {
        ssize_t n = read_timeout(fd, rbuf + total, sizeof(rbuf) - total, 100);
        if (n > 0) {
            total += (size_t)n;
            int op; const uint8_t *pl; size_t pn;
            if (ws_decode_server(rbuf, total, &op, &pl, &pn) > 0) {
                got_close = (op == 0x8);
                break;
            }
        } else if (n == 0) {
            break; /* EOF without a close frame == the old buggy behavior */
        }
    }
    ASSERT(got_close, "oversized frame rejected with WS close, not silent EOF");

    /* Broker survives — a fresh client can still connect and hello. */
    int fd2 = connect_tcp(TEST_WS_PORT);
    ASSERT(fd2 >= 0, "broker alive after oversized frame");
    ASSERT_INT_EQ(ws_handshake(fd2), 0);
    ws_send_json(fd2, sm_msg_hello("ws-after-oversize", "observer"));
    sm_msg_t w2 = ws_recv_json(fd2);
    ASSERT_NOT_NULL(w2.root);
    ASSERT_INT_EQ(w2.type, SM_MSG_WELCOME);
    sm_msg_free(&w2);

    free(payload);
    free(frame);
    close(fd);
    close(fd2);
    teardown(&ctx);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    printf("test_ws\n");

    RUN_TEST(test_ws_handshake);
    RUN_TEST(test_ws_send_receive);
    RUN_TEST(test_ws_output_broadcast);
    RUN_TEST(test_ws_close_frame);
    RUN_TEST(test_ws_oversized_frame_rejected);

    TEST_REPORT();
}
