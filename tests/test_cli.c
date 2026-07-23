/*
 * Tests for smolmux-cli's response reader (read_messages).
 *
 * Regression guard for the fixed-buffer bug: the old char[65536] read buffer
 * requested `sizeof - read_len - 1 == 0` bytes once a newline-less line filled
 * it, so read() returned 0 and a large single-line response (history/report
 * can exceed 64 KB) was misread as "broker disconnected". The buffer now grows
 * on demand; this drives a >64 KB line through and asserts it decodes.
 */

#include "test_main.h"
#include "sm_features.h"
#include "protocol.h"
#include "util/base64.h"

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>

/* Non-static test hooks from cli.c (main renamed to cli_main via the object
 * library, so its main() does not collide with this file's). */
extern int cli_test_read_messages(int fd,
                                  void (*handler)(sm_msg_t *msg, void *ctx),
                                  void *ctx, size_t *cap_out);
extern void cli_test_reset(void);
extern int cli_test_with_port(const char *sock_path, int argc, char **argv,
                              int timeout_ms);

typedef struct {
    int count;
    size_t last_data_len;
    int type;
} capture_t;

static void capture_handler(sm_msg_t *msg, void *ctx)
{
    capture_t *cap = ctx;
    cap->count++;
    cap->type = msg->type;
    /* Decode the base64 output payload to confirm the whole line survived. */
    cJSON *data = cJSON_GetObjectItemCaseSensitive(msg->root, "data");
    if (cJSON_IsString(data) && data->valuestring) {
        size_t raw_len = 0;
        uint8_t *raw = sm_base64_decode(data->valuestring,
                                        strlen(data->valuestring), &raw_len);
        if (raw) {
            cap->last_data_len = raw_len;
            free(raw);
        }
    }
}

/* A >64 KB single-line response must be reassembled and decoded, not treated
 * as a disconnect, and the buffer must have grown past its initial capacity. */
static void test_large_response_line(void)
{
    cli_test_reset();

    /* ~192 KB raw payload -> ~256 KB base64 -> a single JSON line well past
     * the 64 KB initial buffer, forcing at least two growth steps. */
    const size_t payload_len = 192 * 1024;
    uint8_t *payload = malloc(payload_len);
    ASSERT_NOT_NULL(payload);
    for (size_t i = 0; i < payload_len; i++)
        payload[i] = (uint8_t)('A' + (i % 26));

    cJSON *msg = sm_msg_output(payload, payload_len, 1234.5);
    ASSERT_NOT_NULL(msg);
    size_t line_len = 0;
    char *line = sm_msg_encode(msg, &line_len);  /* includes trailing '\n' */
    cJSON_Delete(msg);
    ASSERT_NOT_NULL(line);
    ASSERT(line_len > 64 * 1024, "encoded line exceeds initial buffer size");

    int sp[2];
    ASSERT_INT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sp), 0);

    capture_t cap = {0};
    size_t grown_cap = 0;
    int last_rc = 0;

    /* Write the line in chunks, draining after each so the socketpair buffer
     * never blocks. Only the final chunk carries the newline. */
    const size_t chunk = 16 * 1024;
    size_t written = 0;
    while (written < line_len) {
        size_t w = line_len - written < chunk ? line_len - written : chunk;
        ssize_t nw = write(sp[1], line + written, w);
        ASSERT(nw == (ssize_t)w, "chunk written to socketpair");
        written += w;
        last_rc = cli_test_read_messages(sp[0], capture_handler, &cap, &grown_cap);
        ASSERT_INT_EQ(last_rc, 0);  /* never a false disconnect mid-line */
    }

    ASSERT_INT_EQ(cap.count, 1);
    ASSERT_INT_EQ(cap.type, SM_MSG_OUTPUT);
    ASSERT(cap.last_data_len == payload_len, "full payload decoded intact");
    ASSERT(grown_cap > 64 * 1024, "read buffer grew beyond initial capacity");

    close(sp[0]);
    close(sp[1]);
    free(line);
    free(payload);
    cli_test_reset();
}

/* Two normal-sized responses in one read must both decode (framing intact). */
static void test_two_small_responses(void)
{
    cli_test_reset();

    cJSON *m1 = sm_msg_output((const uint8_t *)"first", 5, 1.0);
    cJSON *m2 = sm_msg_output((const uint8_t *)"second", 6, 2.0);
    size_t l1 = 0, l2 = 0;
    char *s1 = sm_msg_encode(m1, &l1);
    char *s2 = sm_msg_encode(m2, &l2);
    cJSON_Delete(m1);
    cJSON_Delete(m2);

    int sp[2];
    ASSERT_INT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sp), 0);
    ASSERT(write(sp[1], s1, l1) == (ssize_t)l1, "wrote first");
    ASSERT(write(sp[1], s2, l2) == (ssize_t)l2, "wrote second");

    capture_t cap = {0};
    int rc = cli_test_read_messages(sp[0], capture_handler, &cap, NULL);
    ASSERT_INT_EQ(rc, 0);
    ASSERT_INT_EQ(cap.count, 2);

    close(sp[0]);
    close(sp[1]);
    free(s1);
    free(s2);
    cli_test_reset();
}

/* --- with-port: broker-driven tests (PTY + broker-in-thread) ---
 * Gated on UART: the driver stands up a real UART broker over a PTY, so it
 * only compiles/links when the UART link is built. */
#if SM_ENABLE_UART

#include "broker.h"
#include "links/uart.h"
#include <pthread.h>
#include <pty.h>

#define WP_SOCK "/tmp/smolmux-test-cliwp.sock"

typedef struct {
    int master;
    int slave;
    sm_broker_t broker;
    sm_link_t *link;
    pthread_t tid;
} wp_ctx_t;

static void *wp_broker_thread(void *arg)
{
    sm_broker_run((sm_broker_t *)arg);
    return NULL;
}

static void wp_setup(wp_ctx_t *ctx)
{
    openpty(&ctx->master, &ctx->slave, NULL, NULL, NULL);
    char *slave_name = ttyname(ctx->slave);
    /* exclusive=0: a real device releases fully on close, but a held-master PTY
     * keeps TTY_EXCLUSIVE set under TIOCEXCL and would fail the reopen. With
     * exclusive off the suspend->close->resume->reopen cycle works on a PTY. */
    ctx->link = sm_uart_new(slave_name, 115200, 0);
    sm_broker_init(&ctx->broker, ctx->link, WP_SOCK);
    snprintf(ctx->broker.port, sizeof(ctx->broker.port), "%s", slave_name);
    ctx->broker.baudrate = 115200;
    pthread_create(&ctx->tid, NULL, wp_broker_thread, &ctx->broker);
    usleep(150000);
}

static void wp_teardown(wp_ctx_t *ctx)
{
    sm_broker_stop(&ctx->broker);
    pthread_join(ctx->tid, NULL);
    sm_broker_destroy(&ctx->broker);
    close(ctx->master);
    close(ctx->slave);
}

/* A command that exits 0 -> with-port returns 0 and the broker is resumed. */
static void test_with_port_success_resumes(void)
{
    wp_ctx_t ctx;
    wp_setup(&ctx);

    char *argv[] = { "with-port", "/bin/true", NULL };
    int rc = cli_test_with_port(WP_SOCK, 2, argv, 2000);

    ASSERT_INT_EQ(rc, 0);
    ASSERT_INT_EQ(ctx.broker.suspended, 0);  /* always resumed */

    wp_teardown(&ctx);
}

/* The command's exit code is propagated as with-port's exit code, and the
 * broker is still resumed even though the command failed. */
static void test_with_port_propagates_exit_code(void)
{
    wp_ctx_t ctx;
    wp_setup(&ctx);

    char *argv[] = { "with-port", "/bin/sh", "-c", "exit 3", NULL };
    int rc = cli_test_with_port(WP_SOCK, 4, argv, 2000);

    ASSERT_INT_EQ(rc, 3);
    ASSERT_INT_EQ(ctx.broker.suspended, 0);  /* resumed despite failure */

    wp_teardown(&ctx);
}

/* A command that cannot be exec'd -> 127, and the port is still re-acquired. */
static void test_with_port_exec_failure(void)
{
    wp_ctx_t ctx;
    wp_setup(&ctx);

    char *argv[] = { "with-port", "/no/such/binary-xyz", NULL };
    int rc = cli_test_with_port(WP_SOCK, 2, argv, 2000);

    ASSERT_INT_EQ(rc, 127);
    ASSERT_INT_EQ(ctx.broker.suspended, 0);

    wp_teardown(&ctx);
}

/* No command given -> usage error (2), broker untouched (never suspended). */
static void test_with_port_missing_command(void)
{
    wp_ctx_t ctx;
    wp_setup(&ctx);

    char *argv[] = { "with-port", NULL };
    int rc = cli_test_with_port(WP_SOCK, 1, argv, 2000);

    ASSERT_INT_EQ(rc, 2);
    ASSERT_INT_EQ(ctx.broker.suspended, 0);

    wp_teardown(&ctx);
}

#endif /* SM_ENABLE_UART */

int main(void)
{
    printf("test_cli\n");
    RUN_TEST(test_large_response_line);
    RUN_TEST(test_two_small_responses);
#if SM_ENABLE_UART
    RUN_TEST(test_with_port_success_resumes);
    RUN_TEST(test_with_port_propagates_exit_code);
    RUN_TEST(test_with_port_exec_failure);
    RUN_TEST(test_with_port_missing_command);
#endif
    TEST_REPORT();
}
