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
#include <time.h>
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
extern int cli_test_wait_for_parse(int argc, char **argv, int default_timeout_ms,
                                   const char **pattern_out, int *timeout_out);
extern void cli_hello_name(char *out, size_t n, const char *cmd);

static int capture_out_err(char *const argv[], char *out, size_t on,
                           char *err, size_t en);

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

static const char *find_smolmux(char *buf, size_t len)
{
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0)
        return NULL;
    exe[n] = '\0';
    char *slash = strrchr(exe, '/');
    if (!slash)
        return NULL;
    *slash = '\0';
    snprintf(buf, len, "%s/smolmux", exe);
    if (access(buf, X_OK) != 0)
        return NULL;
    return buf;
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

    fflush(stdout);
    int outp[2];
    ASSERT_INT_EQ(pipe(outp), 0);
    int saved_out = dup(STDOUT_FILENO);
    ASSERT(saved_out >= 0, "dup stdout");
    ASSERT_INT_EQ(dup2(outp[1], STDOUT_FILENO), STDOUT_FILENO);
    close(outp[1]);
    int up_rc = cli_test_board_up(manpath);
    fflush(stdout);
    dup2(saved_out, STDOUT_FILENO);
    close(saved_out);
    int fl = fcntl(outp[0], F_GETFL, 0);
    fcntl(outp[0], F_SETFL, fl | O_NONBLOCK);
    char up_out[4096];
    memset(up_out, 0, sizeof(up_out));
    size_t up_got = 0;
    while (up_got < sizeof(up_out) - 1) {
        ssize_t nr = read(outp[0], up_out + up_got, sizeof(up_out) - 1 - up_got);
        if (nr <= 0)
            break;
        up_got += (size_t)nr;
    }
    close(outp[0]);
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
    char expect_log[320];
    snprintf(expect_log, sizeof(expect_log),
             "%s/smolmux-%s-console.log", rundir, board);
    ASSERT(strstr(up_out, "log ") != NULL, "board up prints log label");
    ASSERT(strstr(up_out, expect_log) != NULL,
           "board up names the console wire log path");
    snprintf(expect_log, sizeof(expect_log),
             "%s/smolmux-%s-aux.log", rundir, board);
    ASSERT(strstr(up_out, expect_log) != NULL,
           "board up names the aux wire log path");
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

/* ACT-051: argv policy=seat + mismatched by_path refuses before open. */
static void test_argv_policy_seat_refuses_mismatch(void)
{
    unsetenv("SMOLMUX_IDENTITY_OK");
    char bin[4096];
    const char *sm = find_smolmux(bin, sizeof(bin));
    ASSERT_NOT_NULL(sm);

    int errp[2];
    ASSERT_INT_EQ(pipe(errp), 0);
    pid_t pid = fork();
    ASSERT(pid >= 0, "fork");
    if (pid == 0) {
        close(errp[0]);
        dup2(errp[1], STDERR_FILENO);
        dup2(errp[1], STDOUT_FILENO);
        close(errp[1]);
        execl(sm, "smolmux",
              "/dev/serial/by-id/usb-fake-noserial-if00",
              "--board", "seatcam",
              "--identity-policy", "seat",
              "--by-path", "/dev/ttySEAT-NOT-THIS",
              "-s", "/tmp/smolmux-argv-seat.sock",
              (char *)NULL);
        _exit(127);
    }
    close(errp[1]);
    int st = 0;
    waitpid(pid, &st, 0);
    char err[2048];
    memset(err, 0, sizeof(err));
    ssize_t en = read(errp[0], err, sizeof(err) - 1);
    if (en < 0) en = 0;
    err[en] = '\0';
    close(errp[0]);
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : 99;
    ASSERT_INT_EQ(code, 1);
    ASSERT(strstr(err, "identity_ambiguous") != NULL,
           "argv seat mismatch refuses before open");
    ASSERT(strstr(err, "listening on") == NULL, "broker did not bind");
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

/* Trailing --timeout must bound listen_expect, not the 5s default. */
static void test_wait_for_trailing_timeout_pty(void)
{
    char clipath[4096];
    const char *cli = find_smolmux_cli(clipath, sizeof(clipath));
    ASSERT_NOT_NULL(cli);

    wp_ctx_t ctx;
    wp_setup(&ctx);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pid_t pid = fork();
    ASSERT(pid >= 0, "fork");
    if (pid == 0) {
        execl(cli, "smolmux-cli", "-s", WP_SOCK,
              "wait-for", "READY", "--timeout", "250", (char *)NULL);
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    wp_teardown(&ctx);

    ASSERT(WIFEXITED(st) && WEXITSTATUS(st) == 1, "no READY match");
    double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
                (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    ASSERT(ms < 2000.0, "trailing --timeout 250 finishes under 2s");
    ASSERT(ms >= 150.0, "waited at least the timeout");

    wp_setup(&ctx);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pid = fork();
    ASSERT(pid >= 0, "fork leading");
    if (pid == 0) {
        execl(cli, "smolmux-cli", "-s", WP_SOCK,
              "wait-for", "--timeout", "250", "READY", (char *)NULL);
        _exit(127);
    }
    st = 0;
    waitpid(pid, &st, 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    wp_teardown(&ctx);
    ASSERT(WIFEXITED(st) && WEXITSTATUS(st) == 1, "leading: no READY match");
    ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
         (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    ASSERT(ms < 2000.0, "leading --timeout 250 finishes under 2s");
    ASSERT(ms >= 150.0, "leading waited at least the timeout");
}

static void test_weak_by_id_warn_cites_packed_doc(void)
{
    char bin[4096];
    const char *sm = find_smolmux(bin, sizeof(bin));
    ASSERT_NOT_NULL(sm);

    char out[4096], err[8192];
    /* Class-only by-id that cannot exist — do not open a live hub port. */
    char *argv[] = {
        bin,
        "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0-smolmux-test-missing",
        "-s", "/tmp/smolmux-weak-warn-test.sock",
        NULL
    };
    int rc = capture_out_err(argv, out, sizeof(out), err, sizeof(err));
    ASSERT(rc != 0, "missing weak by-id fails to open");
    ASSERT(strstr(err, "docs/PERSISTENT-SERIAL.md") != NULL,
           "WEAK warn cites packed doc");
    ASSERT(strstr(err, "source tree") == NULL,
           "WEAK warn has no source-tree parenthetical");
}

/* ACT-066 CTL-1: wait-for is listen_expect in the CLI owning phase. */
static void test_wait_for_matches_listen_expect(void)
{
    char clipath[4096];
    const char *cli = find_smolmux_cli(clipath, sizeof(clipath));
    ASSERT_NOT_NULL(cli);

    wp_ctx_t ctx;
    wp_setup(&ctx);

    pid_t pid = fork();
    ASSERT(pid >= 0, "fork");
    if (pid == 0) {
        execl(cli, "smolmux-cli", "-s", WP_SOCK, "-t", "3000",
              "wait-for", "READY", (char *)NULL);
        _exit(127);
    }
    usleep(250000);
    write(ctx.master, "boot READY now\n", 15);
    int st = 0;
    waitpid(pid, &st, 0);
    wp_teardown(&ctx);
    ASSERT(WIFEXITED(st) && WEXITSTATUS(st) == 0, "wait-for matched READY");
}

static void test_history_since_seq_cli_e2e(void)
{
    char clipath[4096];
    const char *cli = find_smolmux_cli(clipath, sizeof(clipath));
    ASSERT_NOT_NULL(cli);

    wp_ctx_t ctx;
    wp_setup(&ctx);
    write(ctx.master, "A4-since-seq-early\n", 19);
    write(ctx.master, "A4-since-seq-late\n", 18);
    usleep(250000);

    int outp[2];
    ASSERT_INT_EQ(pipe(outp), 0);
    pid_t pid = fork();
    ASSERT(pid >= 0, "fork");
    if (pid == 0) {
        close(outp[0]);
        dup2(outp[1], STDOUT_FILENO);
        close(outp[1]);
        /* Seq far past the ring: ignoring --since-seq would still dump both
         * markers; a working filter returns neither. */
        execl(cli, "smolmux-cli", "-s", WP_SOCK, "-t", "3000",
              "history", "--since-seq", "999999999", (char *)NULL);
        _exit(127);
    }
    close(outp[1]);
    char out[4096];
    memset(out, 0, sizeof(out));
    size_t got = 0;
    while (got < sizeof(out) - 1) {
        ssize_t n = read(outp[0], out + got, sizeof(out) - 1 - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    close(outp[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    wp_teardown(&ctx);
    ASSERT(WIFEXITED(st) && WEXITSTATUS(st) == 0, "history --since-seq exits 0");
    ASSERT(strstr(out, "A4-since-seq-early") == NULL,
           "--since-seq must drop earlier bytes");
    ASSERT(strstr(out, "A4-since-seq-late") == NULL,
           "--since-seq 999999999 is past the ring");
}

static void test_cli_s_device_node_prints_derive(void)
{
    char clipath[4096];
    const char *cli = find_smolmux_cli(clipath, sizeof(clipath));
    ASSERT_NOT_NULL(cli);

    int errp[2];
    ASSERT_INT_EQ(pipe(errp), 0);
    pid_t pid = fork();
    ASSERT(pid >= 0, "fork");
    if (pid == 0) {
        close(errp[0]);
        dup2(errp[1], STDERR_FILENO);
        dup2(errp[1], STDOUT_FILENO);
        close(errp[1]);
        execl(cli, "smolmux-cli", "-s", "/dev/null", "status", (char *)NULL);
        _exit(127);
    }
    close(errp[1]);
    char err[2048];
    memset(err, 0, sizeof(err));
    size_t got = 0;
    while (got < sizeof(err) - 1) {
        ssize_t n = read(errp[0], err + got, sizeof(err) - 1 - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    close(errp[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    ASSERT(WIFEXITED(st) && WEXITSTATUS(st) != 0,
           "CLI exits nonzero with no broker");
    ASSERT(strstr(err, "is a device node; using socket") != NULL,
           "CLI -s /dev/null prints derive note");
    ASSERT(strstr(err, "smolmux-null.sock") != NULL,
           "CLI names the derived socket");
    ASSERT(strstr(err, "cannot connect to /dev/null") == NULL,
           "connect target is not AF_UNIX /dev/null");
    {
        const char *cc = strstr(err, "cannot connect to ");
        ASSERT(cc && strstr(cc, "smolmux-null.sock") != NULL,
               "connect error names the derived sock");
    }
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

static int drain_fd(int fd, char *buf, size_t n)
{
    size_t got = 0;
    while (got < n - 1) {
        ssize_t nrd = read(fd, buf + got, n - 1 - got);
        if (nrd <= 0)
            break;
        got += (size_t)nrd;
    }
    buf[got] = '\0';
    return (int)got;
}

static char *capture_cli_stdout(char *const argv[])
{
    int outp[2];
    if (pipe(outp) != 0)
        return NULL;
    pid_t pid = fork();
    if (pid < 0)
        return NULL;
    if (pid == 0) {
        close(outp[0]);
        dup2(outp[1], STDOUT_FILENO);
        close(outp[1]);
        execv(argv[0], argv);
        _exit(127);
    }
    close(outp[1]);
    char buf[4096];
    memset(buf, 0, sizeof(buf));
    ssize_t n = read(outp[0], buf, sizeof(buf) - 1);
    close(outp[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0 || n < 0)
        return NULL;
    return strdup(buf);
}

static int capture_out_err(char *const argv[], char *out, size_t on,
                            char *err, size_t en)
{
    int op[2], ep[2];
    if (pipe(op) != 0 || pipe(ep) != 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        close(op[0]);
        close(ep[0]);
        dup2(op[1], STDOUT_FILENO);
        dup2(ep[1], STDERR_FILENO);
        close(op[1]);
        close(ep[1]);
        execv(argv[0], argv);
        _exit(127);
    }
    close(op[1]);
    close(ep[1]);
    drain_fd(op[0], out, on);
    drain_fd(ep[0], err, en);
    close(op[0]);
    close(ep[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st))
        return -1;
    return (int)WEXITSTATUS(st);
}

static char *capture_cli_merged(char *const argv[], int *code)
{
    int p[2];
    if (pipe(p) != 0)
        return NULL;
    pid_t pid = fork();
    if (pid < 0)
        return NULL;
    if (pid == 0) {
        close(p[0]);
        dup2(p[1], STDOUT_FILENO);
        dup2(p[1], STDERR_FILENO);
        close(p[1]);
        execv(argv[0], argv);
        _exit(127);
    }
    close(p[1]);
    char buf[8192];
    memset(buf, 0, sizeof(buf));
    size_t got = 0;
    while (got < sizeof(buf) - 1) {
        ssize_t nrd = read(p[0], buf + got, sizeof(buf) - 1 - got);
        if (nrd <= 0)
            break;
        got += (size_t)nrd;
    }
    close(p[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    if (code)
        *code = WIFEXITED(st) ? (int)WEXITSTATUS(st) : -1;
    return strdup(buf);
}

static void test_cli_hello_name_format(void)
{
    char name[64];
    cli_hello_name(name, sizeof(name), "status");
    ASSERT(strncmp(name, "smolmux-cli-status-", 19) == 0, "hello prefix");
    ASSERT(strlen(name) < sizeof(name), "fits name[64]");
    char want[64];
    snprintf(want, sizeof(want), "smolmux-cli-status-%d", (int)getpid());
    ASSERT_STR_EQ(name, want);
}

static void test_help_on_stdout(void)
{
    char bin[4096], clipath[4096];
    const char *sm = find_smolmux(bin, sizeof(bin));
    const char *cli = find_smolmux_cli(clipath, sizeof(clipath));
    ASSERT_NOT_NULL(sm);
    ASSERT_NOT_NULL(cli);

    char out[4096], err[4096];
    char *argv_sm[] = { bin, "--help", NULL };
    ASSERT_INT_EQ(capture_out_err(argv_sm, out, sizeof(out), err, sizeof(err)),
                  0);
    ASSERT(strlen(out) > 0, "smolmux --help writes stdout");
    ASSERT(err[0] == '\0', "smolmux --help leaves stderr empty");

    char *argv_cli[] = { clipath, "-h", NULL };
    ASSERT_INT_EQ(capture_out_err(argv_cli, out, sizeof(out), err, sizeof(err)),
                  0);
    ASSERT(strlen(out) > 0, "smolmux-cli -h writes stdout");
    ASSERT(err[0] == '\0', "smolmux-cli -h leaves stderr empty");

    char *argv_bad[] = { bin, "--not-a-real-option", NULL };
    ASSERT_INT_EQ(capture_out_err(argv_bad, out, sizeof(out), err, sizeof(err)),
                  1);
    ASSERT(out[0] == '\0', "unknown option leaves stdout empty");
    ASSERT(err[0] != '\0', "unknown option writes usage on stderr");
}

static void test_identity_ambiguous_cli_human_and_json(void)
{
    unsetenv("SMOLMUX_IDENTITY_OK");
    char clipath[4096];
    const char *cli = find_smolmux_cli(clipath, sizeof(clipath));
    ASSERT_NOT_NULL(cli);

    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !tmpdir[0])
        tmpdir = "/tmp";
    char manpath[256];
    snprintf(manpath, sizeof(manpath), "%s/smolmux-weak-json-%d.board.json",
             tmpdir, (int)getpid());
    FILE *fp = fopen(manpath, "w");
    ASSERT_NOT_NULL(fp);
    fputs("{\"board\":\"weakcam\",\"wires\":[{"
          "\"role\":\"console\",\"link\":\"uart\","
          "\"device\":\"/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0\","
          "\"baud\":115200}]}", fp);
    fclose(fp);

    char *argv_h[] = { clipath, "board", "up", manpath, NULL };
    int rc = -1;
    char *merged = capture_cli_merged(argv_h, &rc);
    ASSERT_NOT_NULL(merged);
    ASSERT_INT_EQ(rc, 1);
    ASSERT(strstr(merged, "identity_ambiguous") != NULL, "human names error");
    ASSERT(strstr(merged, "\"candidates\"") == NULL,
           "human has no candidates JSON");
    ASSERT(strstr(merged, "smolmux --list-ports") != NULL,
           "human names list-ports");
    free(merged);

    char *argv_j[] = { clipath, "--json", "board", "up", manpath, NULL };
    merged = capture_cli_merged(argv_j, &rc);
    ASSERT_NOT_NULL(merged);
    ASSERT_INT_EQ(rc, 1);
    {
        const char *brace = strchr(merged, '{');
        ASSERT(brace != NULL, "--json prints a JSON object");
        cJSON *j = cJSON_Parse(brace);
        ASSERT_NOT_NULL(j);
        ASSERT_STR_EQ(sm_json_get_string(j, "error"), "identity_ambiguous");
        ASSERT(cJSON_IsArray(cJSON_GetObjectItem(j, "candidates")),
               "--json has candidates");
        cJSON_Delete(j);
    }
    free(merged);
    unlink(manpath);
}

#if SM_ENABLE_UART
static char *capture_stdout_in(char *const argv[], const char *cwd, const char *home)
{
    int outp[2];
    if (pipe(outp) != 0)
        return NULL;
    pid_t pid = fork();
    if (pid < 0)
        return NULL;
    if (pid == 0) {
        close(outp[0]);
        dup2(outp[1], STDOUT_FILENO);
        close(outp[1]);
        if (home)
            setenv("HOME", home, 1);
        unsetenv("SMOLMUX_DEVICE_PROFILE");
        if (cwd && chdir(cwd) != 0)
            _exit(127);
        execv(argv[0], argv);
        _exit(127);
    }
    close(outp[1]);
    char buf[4096];
    memset(buf, 0, sizeof(buf));
    size_t got = 0;
    while (got < sizeof(buf) - 1) {
        ssize_t nrd = read(outp[0], buf + got, sizeof(buf) - 1 - got);
        if (nrd <= 0)
            break;
        got += (size_t)nrd;
    }
    close(outp[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0 || got == 0)
        return NULL;
    return strdup(buf);
}

static void write_tiny_profile(const char *path, const char *name, const char *desc)
{
    FILE *fp = fopen(path, "w");
    ASSERT_NOT_NULL(fp);
    fprintf(fp, "{\"name\":\"%s\",\"description\":\"%s\"}\n", name, desc);
    fclose(fp);
}

static void test_list_profiles_skips_cwd_configs(void)
{
    char bin[4096];
    const char *sm = find_smolmux(bin, sizeof(bin));
    ASSERT_NOT_NULL(sm);

    char root[128];
    snprintf(root, sizeof(root), "/tmp/smlp-%d", (int)getpid());
    ASSERT_INT_EQ(mkdir(root, 0700), 0);
    char home[160], cfg[200], cwd[160], configs[180], profiles[180], cfgdir[220];
    snprintf(home, sizeof(home), "%s/home", root);
    snprintf(cfg, sizeof(cfg), "%s/.config", home);
    snprintf(cfgdir, sizeof(cfgdir), "%s/smolmux", cfg);
    snprintf(cwd, sizeof(cwd), "%s/work", root);
    snprintf(configs, sizeof(configs), "%s/configs", cwd);
    snprintf(profiles, sizeof(profiles), "%s/profiles", cwd);
    ASSERT_INT_EQ(mkdir(home, 0700), 0);
    ASSERT_INT_EQ(mkdir(cfg, 0700), 0);
    ASSERT_INT_EQ(mkdir(cfgdir, 0700), 0);
    ASSERT_INT_EQ(mkdir(cwd, 0700), 0);
    ASSERT_INT_EQ(mkdir(configs, 0700), 0);
    ASSERT_INT_EQ(mkdir(profiles, 0700), 0);

    char p_home[256], p_leak[256], p_zip[256], p_home_dup[256], p_dup[256];
    snprintf(p_home, sizeof(p_home), "%s/homeok.smolmux-profile.json", cfgdir);
    snprintf(p_home_dup, sizeof(p_home_dup), "%s/shared.smolmux-profile.json",
             cfgdir);
    snprintf(p_leak, sizeof(p_leak), "%s/leak.smolmux-profile.json", configs);
    snprintf(p_zip, sizeof(p_zip), "%s/zipok.smolmux-profile.json", profiles);
    snprintf(p_dup, sizeof(p_dup), "%s/dup.smolmux-profile.json", profiles);
    write_tiny_profile(p_home, "home-config-ok", "from HOME");
    write_tiny_profile(p_home_dup, "dup-in-both", "from HOME");
    write_tiny_profile(p_leak, "cwd-configs-leak", "must not list");
    write_tiny_profile(p_zip, "cwd-profiles-ok", "from zip profiles");
    write_tiny_profile(p_dup, "dup-in-both", "from zip profiles");

    char *argv[] = { bin, "--list-profiles", NULL };
    char *out = capture_stdout_in(argv, cwd, home);
    ASSERT_NOT_NULL(out);
    ASSERT(strstr(out, "home-config-ok") != NULL, "lists ~/.config");
    ASSERT(strstr(out, "cwd-profiles-ok") != NULL, "lists cwd profiles/");
    ASSERT(strstr(out, "cwd-configs-leak") == NULL, "does not list cwd configs/");
    ASSERT(strstr(out, "(bundled)") == NULL, "does not label bundled");
    {
        const char *hit = strstr(out, "dup-in-both");
        ASSERT(hit != NULL, "lists shared name");
        ASSERT(strstr(hit + 1, "dup-in-both") == NULL, "shared name once");
        const char *eol = strchr(hit, '\n');
        size_t ln = eol ? (size_t)(eol - hit) : strlen(hit);
        char line[256];
        if (ln >= sizeof(line))
            ln = sizeof(line) - 1;
        memcpy(line, hit, ln);
        line[ln] = '\0';
        ASSERT(strstr(line, "(profiles/)") == NULL,
               "HOME config wins, no profiles/ tag");
    }
    free(out);

    unlink(p_home);
    unlink(p_home_dup);
    unlink(p_leak);
    unlink(p_zip);
    unlink(p_dup);
    rmdir(cfgdir);
    rmdir(cfg);
    rmdir(home);
    rmdir(configs);
    rmdir(profiles);
    rmdir(cwd);
    rmdir(root);
}
#endif /* SM_ENABLE_UART */

static void test_list_ports_json_flag_before_or_after(void)
{
    char clipath[4096];
    const char *cli = find_smolmux_cli(clipath, sizeof(clipath));
    ASSERT_NOT_NULL(cli);

    char *argv_before[] = { clipath, "--json", "list-ports", NULL };
    char *out_b = capture_cli_stdout(argv_before);
    ASSERT_NOT_NULL(out_b);
    ASSERT(out_b[0] == '[', "--json list-ports starts a JSON array");
    free(out_b);

    char *argv_after[] = { clipath, "list-ports", "--json", NULL };
    char *out_a = capture_cli_stdout(argv_after);
    ASSERT_NOT_NULL(out_a);
    ASSERT(out_a[0] == '[', "list-ports --json starts a JSON array");
    free(out_a);

    char *argv_brokers[] = { clipath, "brokers", "--json", NULL };
    char *out_br = capture_cli_stdout(argv_brokers);
    ASSERT_NOT_NULL(out_br);
    ASSERT(out_br[0] == '[', "brokers --json starts a JSON array");
    free(out_br);
}

static void test_serial_port_info_json_includes_usb_ids(void)
{
    sm_serial_port_info_t info;
    memset(&info, 0, sizeof(info));
    snprintf(info.path, sizeof(info.path), "/dev/ttyUSB2");
    snprintf(info.by_id, sizeof(info.by_id),
             "/dev/serial/by-id/usb-Prolific_Technology_Inc._USB-Serial_Controller_DPAZb137C01-if00-port0");
    snprintf(info.by_path, sizeof(info.by_path),
             "/dev/serial/by-path/pci-0000:00:14.0-usb-0:9.3.4:1.0-port0");
    snprintf(info.vid, sizeof(info.vid), "067b");
    snprintf(info.pid, sizeof(info.pid), "23a3");
    snprintf(info.manufacturer, sizeof(info.manufacturer), "Prolific");
    snprintf(info.product, sizeof(info.product), "USB-Serial Controller");

    cJSON *o = sm_serial_port_info_to_json(&info);
    ASSERT_NOT_NULL(o);
    ASSERT_STR_EQ(sm_json_get_string(o, "path"), "/dev/ttyUSB2");
    ASSERT_STR_EQ(sm_json_get_string(o, "vid"), "067b");
    ASSERT_STR_EQ(sm_json_get_string(o, "pid"), "23a3");
    ASSERT_STR_EQ(sm_json_get_string(o, "manufacturer"), "Prolific");
    ASSERT_STR_EQ(sm_json_get_string(o, "product"), "USB-Serial Controller");
    ASSERT_STR_EQ(sm_json_get_string(o, "identity"), "STRONG");
    cJSON_Delete(o);
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

/* Honor --timeout on either side of the wait-for pattern (owning parse). */
static void test_wait_for_timeout_either_side(void)
{
    const char *pat = NULL;
    int to = -1;
    char *trail[] = {"wait-for", "READY", "--timeout", "250", NULL};
    ASSERT_INT_EQ(cli_test_wait_for_parse(4, trail, 5000, &pat, &to), 0);
    ASSERT_STR_EQ(pat, "READY");
    ASSERT_INT_EQ(to, 250);

    pat = NULL;
    to = -1;
    char *lead[] = {"wait-for", "--timeout", "250", "READY", NULL};
    ASSERT_INT_EQ(cli_test_wait_for_parse(4, lead, 5000, &pat, &to), 0);
    ASSERT_STR_EQ(pat, "READY");
    ASSERT_INT_EQ(to, 250);

    pat = NULL;
    to = -1;
    char *def[] = {"wait-for", "READY", NULL};
    ASSERT_INT_EQ(cli_test_wait_for_parse(2, def, 5000, &pat, &to), 0);
    ASSERT_STR_EQ(pat, "READY");
    ASSERT_INT_EQ(to, 5000);

    char *none[] = {"wait-for", NULL};
    ASSERT_INT_EQ(cli_test_wait_for_parse(1, none, 5000, &pat, &to), -1);
}

static void write_weak_board_manifest(char *manpath, size_t n)
{
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !tmpdir[0])
        tmpdir = "/tmp";
    snprintf(manpath, n, "%s/smolmux-weak-json-%d.board.json",
             tmpdir, (int)getpid());
    FILE *fp = fopen(manpath, "w");
    ASSERT_NOT_NULL(fp);
    fputs("{\"board\":\"weakcam\",\"wires\":[{"
          "\"role\":\"console\",\"link\":\"uart\","
          "\"device\":\"/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0\","
          "\"baud\":115200}]}", fp);
    fclose(fp);
}

/* --json board up: one JSON object on stdout; stderr is not that object. */
static void test_json_board_up_stdout_not_stderr(void)
{
    unsetenv("SMOLMUX_IDENTITY_OK");
    char clipath[4096];
    const char *cli = find_smolmux_cli(clipath, sizeof(clipath));
    ASSERT_NOT_NULL(cli);

    char manpath[256];
    write_weak_board_manifest(manpath, sizeof(manpath));

    char out[8192], err[8192];
    char *argv_before[] = { clipath, "--json", "board", "up", manpath, NULL };
    int rc = capture_out_err(argv_before, out, sizeof(out), err, sizeof(err));
    ASSERT_INT_EQ(rc, 1);
    ASSERT(out[0] == '{', "--json board up JSON on stdout");
    cJSON *j = cJSON_Parse(out);
    ASSERT_NOT_NULL(j);
    ASSERT_STR_EQ(sm_json_get_string(j, "error"), "identity_ambiguous");
    cJSON_Delete(j);
    ASSERT(strchr(err, '{') == NULL,
           "stderr is not the identity_ambiguous JSON object");

    memset(out, 0, sizeof(out));
    memset(err, 0, sizeof(err));
    char *argv_after[] = { clipath, "board", "up", "--json", manpath, NULL };
    rc = capture_out_err(argv_after, out, sizeof(out), err, sizeof(err));
    ASSERT_INT_EQ(rc, 1);
    ASSERT(out[0] == '{', "board up --json JSON on stdout");
    j = cJSON_Parse(out);
    ASSERT_NOT_NULL(j);
    ASSERT_STR_EQ(sm_json_get_string(j, "error"), "identity_ambiguous");
    cJSON_Delete(j);
    ASSERT(strchr(err, '{') == NULL,
           "board up --json stderr is not the JSON object");

    unlink(manpath);
}

#if SM_ENABLE_UART
/* Success-path --json board up: stdout is one JSON object, not human lines. */
static void test_json_board_up_success_stdout(void)
{
    int m0 = -1, s0 = -1, m1 = -1, s1 = -1;
    char rundir[128] = "";
    char board[64];
    snprintf(board, sizeof(board), "jsond%d", (int)getpid());

    ASSERT(openpty(&m0, &s0, NULL, NULL, NULL) == 0, "pty0");
    ASSERT(openpty(&m1, &s1, NULL, NULL, NULL) == 0, "pty1");
    char p0[64], p1[64];
    {
        char *t = ttyname(s0);
        ASSERT_NOT_NULL(t);
        snprintf(p0, sizeof(p0), "%s", t);
        t = ttyname(s1);
        ASSERT_NOT_NULL(t);
        snprintf(p1, sizeof(p1), "%s", t);
    }

    snprintf(rundir, sizeof(rundir), "/tmp/smj1-XXXXXX");
    ASSERT_NOT_NULL(mkdtemp(rundir));
    setenv("XDG_RUNTIME_DIR", rundir, 1);

    char clipath[4096];
    const char *cli = find_smolmux_cli(clipath, sizeof(clipath));
    ASSERT_NOT_NULL(cli);

    char manpath[160];
    snprintf(manpath, sizeof(manpath), "%s/dual.board.json", rundir);
    FILE *fp = fopen(manpath, "w");
    ASSERT_NOT_NULL(fp);
    fprintf(fp,
            "{\"board\":\"%s\",\"wires\":["
            "{\"role\":\"console\",\"link\":\"uart\",\"device\":\"%s\","
            "\"baud\":115200},"
            "{\"role\":\"aux\",\"link\":\"uart\",\"device\":\"%s\","
            "\"baud\":115200}"
            "]}",
            board, p0, p1);
    fclose(fp);

    char out[8192], err[8192];
    char *argv[] = { clipath, "--json", "board", "up", manpath, NULL };
    int rc = capture_out_err(argv, out, sizeof(out), err, sizeof(err));
    cli_test_board_down(board);

    close(m0); close(s0); close(m1); close(s1);
    unlink(manpath);
    char logp[320];
    snprintf(logp, sizeof(logp), "%s/smolmux-%s-console.log", rundir, board);
    unlink(logp);
    snprintf(logp, sizeof(logp), "%s/smolmux-%s-aux.log", rundir, board);
    unlink(logp);
    unsetenv("XDG_RUNTIME_DIR");
    rmdir(rundir);

    ASSERT_INT_EQ(rc, 0);
    ASSERT(strstr(out, "Bringing up board") == NULL,
           "JSON mode keeps human banner off stdout");
    cJSON *j = cJSON_Parse(out);
    ASSERT_NOT_NULL(j);
    ASSERT_STR_EQ(sm_json_get_string(j, "board"), board);
    ASSERT(cJSON_IsArray(cJSON_GetObjectItem(j, "wires")), "wires array");
    cJSON_Delete(j);
    ASSERT(strchr(err, '{') == NULL, "stderr is not the JSON object");
}
#endif

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    printf("test_cli\n");
    RUN_TEST(test_large_response_line);
    RUN_TEST(test_two_small_responses);
    RUN_TEST(test_send_trailing_options_rejected);
    RUN_TEST(test_wait_for_timeout_either_side);
    RUN_TEST(test_json_board_up_stdout_not_stderr);
    RUN_TEST(test_identity_ambiguous_json);
    RUN_TEST(test_cli_hello_name_format);
    RUN_TEST(test_help_on_stdout);
    RUN_TEST(test_identity_ambiguous_cli_human_and_json);
    RUN_TEST(test_serial_port_info_json_includes_usb_ids);
#if SM_ENABLE_UART
    RUN_TEST(test_list_profiles_skips_cwd_configs);
#endif
    RUN_TEST(test_list_ports_json_flag_before_or_after);
#if SM_ENABLE_UART
    RUN_TEST(test_with_port_success_resumes);
    RUN_TEST(test_with_port_propagates_exit_code);
    RUN_TEST(test_with_port_exec_failure);
    RUN_TEST(test_with_port_missing_command);
    RUN_TEST(test_board_up_down_two_wires);
    RUN_TEST(test_json_board_up_success_stdout);
    RUN_TEST(test_board_up_weak_by_id_refuses);
    RUN_TEST(test_argv_policy_seat_refuses_mismatch);
    RUN_TEST(test_send_trailing_flags_real_cli_pty);
    RUN_TEST(test_wait_for_trailing_timeout_pty);
    RUN_TEST(test_weak_by_id_warn_cites_packed_doc);
    RUN_TEST(test_wait_for_matches_listen_expect);
    RUN_TEST(test_history_since_seq_cli_e2e);
    RUN_TEST(test_cli_s_device_node_prints_derive);
#endif
    TEST_REPORT();
}
