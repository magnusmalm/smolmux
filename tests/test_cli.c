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
#include "util/sock_util.h"
#include "broker_info.h"
#include "board_manifest.h"

#include "util/json_helpers.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <pty.h>

/* Non-static test hooks from cli.c (main renamed to cli_main via the object
 * library, so its main() does not collide with this file's). */
extern int cli_test_read_messages(int fd,
                                  void (*handler)(sm_msg_t *msg, void *ctx),
                                  void *ctx, size_t *cap_out);
extern void cli_test_reset(void);
extern int cli_test_with_port(const char *sock_path, int argc, char **argv,
                              int timeout_ms);
extern int cli_test_board_up(const char *manifest_path);
extern int cli_test_board_down(const char *board_name);
extern int cli_test_trailing_option_after(int argc, char **argv, int first);

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

/* CTL-1: board up starts two wires; board down SIGTERMs them by board label.
 * Owning phase is board_up/board_down (not help text). Uses real smolmux next
 * to test binary and two PTYs as UART devices. */
static void test_board_up_down_two_wires(void)
{
    int m0 = -1, s0 = -1, m1 = -1, s1 = -1;
    char rundir[128] = "";
    char board[64];
    snprintf(board, sizeof(board), "ctl1d%d", (int)getpid());

    ASSERT(openpty(&m0, &s0, NULL, NULL, NULL) == 0, "pty0");
    ASSERT(openpty(&m1, &s1, NULL, NULL, NULL) == 0, "pty1");
    /* ttyname() returns a static buffer — copy before the second call. */
    char p0[64], p1[64];
    {
        char *t = ttyname(s0);
        ASSERT_NOT_NULL(t);
        snprintf(p0, sizeof(p0), "%s", t);
        t = ttyname(s1);
        ASSERT_NOT_NULL(t);
        snprintf(p1, sizeof(p1), "%s", t);
    }
    ASSERT(strcmp(p0, p1) != 0, "two distinct PTY paths");

    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !tmpdir[0]) tmpdir = "/tmp";
    /* Keep under /tmp short so board-role sockets fit sun_path. */
    snprintf(rundir, sizeof(rundir), "/tmp/smc1-XXXXXX");
    ASSERT_NOT_NULL(mkdtemp(rundir));
    setenv("XDG_RUNTIME_DIR", rundir, 1);

    char manpath[160];
    snprintf(manpath, sizeof(manpath), "%s/dual.board.json", rundir);
    FILE *fp = fopen(manpath, "w");
    ASSERT_NOT_NULL(fp);
    if (fp) {
        fprintf(fp,
                "{\"board\":\"%s\",\"wires\":["
                "{\"role\":\"console\",\"link\":\"uart\",\"device\":\"%s\","
                "\"baud\":115200},"
                "{\"role\":\"aux\",\"link\":\"uart\",\"device\":\"%s\","
                "\"baud\":115200}"
                "]}",
                board, p0, p1);
        fclose(fp);
    }

    int up_rc = cli_test_board_up(manpath);
    sm_board_manifest_t plan;
    memset(&plan, 0, sizeof(plan));
    char sock0[SM_SOCK_PATH_MAX] = "";
    char sock1[SM_SOCK_PATH_MAX] = "";
    if (sm_board_manifest_load(manpath, &plan) == 0) {
        sm_board_wire_socket(&plan, &plan.wires[0], sock0, sizeof(sock0));
        sm_board_wire_socket(&plan, &plan.wires[1], sock1, sizeof(sock1));
    }

    sm_broker_info_t info;
    int ok0 = 0, ok1 = 0;
    if (up_rc == 0 && sock0[0] && sock1[0]) {
        for (int i = 0; i < 80 && (!ok0 || !ok1); i++) {
            if (!ok0 && sm_broker_probe(sock0, &info, 150) == 0 &&
                info.reachable)
                ok0 = 1;
            if (!ok1 && sm_broker_probe(sock1, &info, 150) == 0 &&
                info.reachable)
                ok1 = 1;
            if (!ok0 || !ok1)
                usleep(50000);
        }
    }

    /* Always tear down board wires before asserts (ASSERT may abort the case). */
    cli_test_board_down(board);
    for (int i = 0; i < 40; i++) {
        int a = sock0[0] ? sm_broker_probe(sock0, &info, 80) : -1;
        int b = sock1[0] ? sm_broker_probe(sock1, &info, 80) : -1;
        if (a != 0 && b != 0)
            break;
        usleep(50000);
    }

    if (!ok0 || !ok1 || up_rc != 0) {
        char logp[320];
        snprintf(logp, sizeof(logp), "%s/smolmux-%s-console.log", rundir, board);
        FILE *lf = fopen(logp, "r");
        if (lf) {
            char buf[512];
            size_t n = fread(buf, 1, sizeof(buf) - 1, lf);
            buf[n] = '\0';
            fprintf(stderr, "console log:\n%s\n", buf);
            fclose(lf);
        }
        snprintf(logp, sizeof(logp), "%s/smolmux-%s-aux.log", rundir, board);
        lf = fopen(logp, "r");
        if (lf) {
            char buf[512];
            size_t n = fread(buf, 1, sizeof(buf) - 1, lf);
            buf[n] = '\0';
            fprintf(stderr, "aux log:\n%s\n", buf);
            fclose(lf);
        }
        fprintf(stderr, "socks: %s | %s  up_rc=%d ok0=%d ok1=%d\n",
                sock0, sock1, up_rc, ok0, ok1);
    }

    close(m0); close(s0); close(m1); close(s1);
    unlink(manpath);
    char logp[320];
    snprintf(logp, sizeof(logp), "%s/smolmux-%s-console.log", rundir, board);
    unlink(logp);
    snprintf(logp, sizeof(logp), "%s/smolmux-%s-aux.log", rundir, board);
    unlink(logp);
    unsetenv("XDG_RUNTIME_DIR");
    rmdir(rundir);

    ASSERT_INT_EQ(up_rc, 0);
    ASSERT(ok0, "console wire broker up after board up");
    ASSERT(ok1, "aux wire broker up after board up");
}

/* D3: board up of a named board on a WEAK by-id must not spawn. */
static void test_board_up_weak_by_id_refuses(void)
{
    unsetenv("SMOLMUX_IDENTITY_OK");
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !tmpdir[0]) tmpdir = "/tmp";
    char manpath[256];
    snprintf(manpath, sizeof(manpath), "%s/smolmux-weak-%d.board.json",
             tmpdir, (int)getpid());
    FILE *fp = fopen(manpath, "w");
    ASSERT_NOT_NULL(fp);
    fputs("{\"board\":\"weakcam\",\"wires\":[{"
          "\"role\":\"console\",\"link\":\"uart\","
          "\"device\":\"/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0\","
          "\"baud\":115200}]}", fp);
    fclose(fp);

    int rc = cli_test_board_up(manpath);
    unlink(manpath);
    ASSERT_INT_EQ(rc, 1);
}

static const char *find_smolmux_cli(char *buf, size_t len)
{
    const char *env = getenv("SMOLMUX_CLI");
    if (env && env[0]) {
        snprintf(buf, len, "%s", env);
        return buf;
    }
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0)
        return NULL;
    exe[n] = '\0';
    char *slash = strrchr(exe, '/');
    if (!slash)
        return NULL;
    *slash = '\0';
    snprintf(buf, len, "%s/smolmux-cli", exe);
    if (access(buf, X_OK) != 0)
        return NULL;
    return buf;
}

/* I2 e2e: real smolmux-cli vs PTY — trailing --expect must not hit the wire. */
static void test_send_trailing_flags_real_cli_pty(void)
{
    char clipath[4096];
    const char *cli = find_smolmux_cli(clipath, sizeof(clipath));
    ASSERT_NOT_NULL(cli);

    wp_ctx_t ctx;
    wp_setup(&ctx);

    int errp[2];
    ASSERT_INT_EQ(pipe(errp), 0);
    pid_t pid = fork();
    ASSERT(pid >= 0, "fork");
    if (pid == 0) {
        close(errp[0]);
        dup2(errp[1], STDERR_FILENO);
        close(errp[1]);
        execl(cli, "smolmux-cli", "-s", WP_SOCK,
              "send", "echo hi", "--expect", "X", (char *)NULL);
        _exit(127);
    }
    close(errp[1]);
    char err[1024];
    memset(err, 0, sizeof(err));
    ssize_t en = read(errp[0], err, sizeof(err) - 1);
    if (en < 0)
        en = 0;
    err[en] = '\0';
    close(errp[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : 99;

    int flags = fcntl(ctx.master, F_GETFL, 0);
    fcntl(ctx.master, F_SETFL, flags | O_NONBLOCK);
    char got[512];
    memset(got, 0, sizeof(got));
    ssize_t n = read(ctx.master, got, sizeof(got) - 1);
    if (n < 0)
        n = 0;
    got[n] = '\0';

    wp_teardown(&ctx);

    ASSERT_INT_EQ(code, 1);
    ASSERT(strstr(err, "options after the command") != NULL,
           "I2 error on stderr");
    ASSERT(strstr(got, "--expect") == NULL, "PTY must not contain --expect");
}

#endif /* SM_ENABLE_UART */

static void test_identity_ambiguous_json(void)
{
    cJSON *err = sm_identity_ambiguous_json("cam",
        "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0",
        "test reason");
    ASSERT_NOT_NULL(err);
    ASSERT_STR_EQ(sm_json_get_string(err, "error"), "identity_ambiguous");
    ASSERT_STR_EQ(sm_json_get_string(err, "board"), "cam");
    ASSERT(cJSON_IsArray(cJSON_GetObjectItem(err, "candidates")),
           "candidates array");
    cJSON_Delete(err);
}

/* I2: flags after the command must be detected (owning phase = cmd_send). */
static void test_send_trailing_options_rejected(void)
{
    char *bad[] = {"send", "printenv", "--expect", "Versal>", NULL};
    ASSERT_INT_EQ(cli_test_trailing_option_after(4, bad, 1), 1);
    char *ok[] = {"send", "--expect", "X", "--timeout", "1000", "echo hi", NULL};
    ASSERT_INT_EQ(cli_test_trailing_option_after(6, ok, 5), 0);
    char *dash[] = {"send", "--", "--weird", NULL};
    ASSERT_INT_EQ(cli_test_trailing_option_after(3, dash, 2), 0);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    printf("test_cli\n");
    RUN_TEST(test_large_response_line);
    RUN_TEST(test_two_small_responses);
    RUN_TEST(test_send_trailing_options_rejected);
    RUN_TEST(test_identity_ambiguous_json);
#if SM_ENABLE_UART
    RUN_TEST(test_with_port_success_resumes);
    RUN_TEST(test_with_port_propagates_exit_code);
    RUN_TEST(test_with_port_exec_failure);
    RUN_TEST(test_with_port_missing_command);
    RUN_TEST(test_board_up_down_two_wires);
    RUN_TEST(test_board_up_weak_by_id_refuses);
    RUN_TEST(test_send_trailing_flags_real_cli_pty);
#endif
    TEST_REPORT();
}
