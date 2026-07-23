/* Broker-level integration test for the GDB link: a fake gdb (shell script
 * that prints the MI banner and then execs cat) is driven through the real
 * fork/execvp spawn, non-blocking pipes, write queue and epoll loop. cat
 * echoes gdb's stdin back on stdout, so everything the broker writes to gdb
 * comes back to clients as output — which lets us assert on the exact wire
 * traffic. Guards the wiring that only live hardware dogfooding caught:
 * mi-async must be sent at spawn before "extended-remote", BREAK must reach
 * gdb as -exec-interrupt, and silence_normal must suppress idle link_health
 * broadcasts (found on the SAM C21, commit 01e8e12). */
#include "test_main.h"
#include "broker.h"
#include "links/gdb.h"
#include "protocol.h"
#include "util/base64.h"
#include "util/json_helpers.h"

#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

#define TEST_SOCK "/tmp/smolmux-test-gdb.sock"
#define STARTUP_DELAY 150000  /* 150ms */

static char g_fake_gdb[512];

/* --- Helpers (pattern from test_broker.c) --- */

static void write_fake_gdb_script(void)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(g_fake_gdb, sizeof(g_fake_gdb), "%s/smolmux-test-fake-gdb.sh",
             tmp && tmp[0] ? tmp : "/tmp");

    FILE *fp = fopen(g_fake_gdb, "w");
    ASSERT_NOT_NULL(fp);
    if (!fp) return;
    fputs("#!/bin/sh\n"
          "printf '=thread-group-added,id=\"i1\"\\n(gdb)\\n'\n"
          "exec cat\n", fp);
    fclose(fp);
    chmod(g_fake_gdb, 0755);
}

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

/* Line-buffered reader: fake gdb echoes several lines back-to-back, so a
 * single read can carry multiple JSON messages; naive read-then-decode
 * would drop all but the first. */
typedef struct line_reader {
    int fd;
    char buf[65536];
    size_t len;
} line_reader_t;

/* Next complete JSON line (NUL-terminated, newline stripped, valid until the
 * following call). NULL after ~attempts*10ms without one. */
static char *lr_next(line_reader_t *lr, int attempts)
{
    static char line[32768];
    for (int i = 0; i < attempts; i++) {
        char *nl = memchr(lr->buf, '\n', lr->len);
        if (nl) {
            size_t n = (size_t)(nl - lr->buf);
            if (n >= sizeof(line)) n = sizeof(line) - 1;
            memcpy(line, lr->buf, n);
            line[n] = '\0';
            memmove(lr->buf, nl + 1, lr->len - (size_t)(nl - lr->buf) - 1);
            lr->len -= (size_t)(nl - lr->buf) + 1;
            return line;
        }
        ssize_t r = read(lr->fd, lr->buf + lr->len, sizeof(lr->buf) - lr->len);
        if (r > 0)
            lr->len += (size_t)r;
        else
            usleep(10000);
    }
    return NULL;
}

/* Collect decoded output payloads into acc until needle appears (in acc) or
 * the reader times out. Non-output messages are ignored. Returns 1 if the
 * needle was seen. */
static int collect_output_until(line_reader_t *lr, char *acc, size_t acc_cap,
                                const char *needle, int attempts)
{
    while (strstr(acc, needle) == NULL) {
        char *l = lr_next(lr, attempts);
        if (!l) return 0;
        sm_msg_t m = sm_msg_decode(l, strlen(l));
        if (m.root && m.type == SM_MSG_OUTPUT) {
            const char *b64 = sm_json_get_string(m.root, "data");
            if (b64) {
                size_t raw_len;
                uint8_t *raw = sm_base64_decode(b64, strlen(b64), &raw_len);
                if (raw) {
                    size_t used = strlen(acc);
                    if (raw_len > acc_cap - used - 1)
                        raw_len = acc_cap - used - 1;
                    memcpy(acc + used, raw, raw_len);
                    acc[used + raw_len] = '\0';
                    free(raw);
                }
            }
        }
        sm_msg_free(&m);
    }
    return 1;
}

/* Fetch the full output history into acc (concatenated decoded chunks).
 * The spawn-time traffic (mi-async, extended-remote) is echoed by the fake
 * gdb before any client can connect, so it is only observable through the
 * ring buffer, not live output broadcasts. Returns 0 on success. */
static int fetch_history(line_reader_t *lr, int fd, char *acc, size_t acc_cap)
{
    send_json(fd, sm_msg_history_request("h1", 0.0, 0));
    acc[0] = '\0';

    for (;;) {
        char *l = lr_next(lr, 300);
        if (!l) return -1;
        sm_msg_t m = sm_msg_decode(l, strlen(l));
        if (!m.root) { sm_msg_free(&m); continue; }
        if (m.type != SM_MSG_HISTORY_RESPONSE) { sm_msg_free(&m); continue; }

        cJSON *chunks = cJSON_GetObjectItem(m.root, "chunks");
        cJSON *ch;
        size_t used = 0;
        cJSON_ArrayForEach(ch, chunks) {
            const char *b64 = sm_json_get_string(ch, "data");
            if (!b64) continue;
            size_t raw_len;
            uint8_t *raw = sm_base64_decode(b64, strlen(b64), &raw_len);
            if (!raw) continue;
            if (raw_len > acc_cap - used - 1)
                raw_len = acc_cap - used - 1;
            memcpy(acc + used, raw, raw_len);
            used += raw_len;
            free(raw);
        }
        acc[used] = '\0';
        sm_msg_free(&m);
        return 0;
    }
}

typedef struct test_ctx {
    sm_broker_t broker;
    sm_link_t *link;
    pthread_t tid;
} test_ctx_t;

static void setup(test_ctx_t *ctx, const char *target_spec, int silence_normal)
{
    memset(&ctx->broker, 0, sizeof(ctx->broker));
    ctx->link = sm_gdb_new(g_fake_gdb, target_spec);
    ctx->link->silence_normal = silence_normal;
    sm_broker_init(&ctx->broker, ctx->link, TEST_SOCK);
    snprintf(ctx->broker.port, sizeof(ctx->broker.port), "fake-gdb");

    pthread_create(&ctx->tid, NULL, broker_thread, &ctx->broker);
    usleep(STARTUP_DELAY);
}

static void teardown(test_ctx_t *ctx)
{
    sm_broker_stop(&ctx->broker);
    pthread_join(ctx->tid, NULL);
    sm_broker_destroy(&ctx->broker);
}

/* Connect + hello + consume welcome; returns client fd with reader primed. */
static int hello(line_reader_t *lr)
{
    int fd = connect_unix(TEST_SOCK);
    if (fd < 0) return -1;
    memset(lr, 0, sizeof(*lr));
    lr->fd = fd;

    send_json(fd, sm_msg_hello("test", "controller"));
    char *l = lr_next(lr, 100);
    if (!l) { close(fd); return -1; }
    sm_msg_t m = sm_msg_decode(l, strlen(l));
    int ok = m.root && m.type == SM_MSG_WELCOME;
    sm_msg_free(&m);
    if (!ok) { close(fd); return -1; }
    return fd;
}

/* --- Tests --- */

/* The two live-hardware findings from 01e8e12, end to end: gdb must receive
 * "-gdb-set mi-async on" at spawn and BEFORE the queued "extended-remote"
 * (gdb refuses to change mi-async once attached). */
static void test_spawn_sends_miasync_before_target(void)
{
    test_ctx_t ctx;
    setup(&ctx, "localhost:3333", 1);

    line_reader_t lr;
    int fd = hello(&lr);
    ASSERT(fd >= 0, "controller connected");
    if (fd < 0) { teardown(&ctx); return; }

    /* The spawn-time echoes land before the client exists — read them from
     * history. Retry a few times in case the echo is still in flight. */
    static char acc[65536];
    int seen = 0;
    for (int tries = 0; tries < 20 && !seen; tries++) {
        if (fetch_history(&lr, fd, acc, sizeof(acc)) != 0) continue;
        seen = strstr(acc, "extended-remote localhost:3333") != NULL;
        if (!seen) usleep(50000);
    }
    ASSERT(seen, "extended-remote echoed by fake gdb");
    ASSERT(strstr(acc, "(gdb)") != NULL, "MI banner came through");

    char *async_pos = strstr(acc, "-gdb-set mi-async on");
    char *target_pos = strstr(acc, "extended-remote localhost:3333");
    ASSERT(async_pos != NULL, "mi-async sent at spawn");
    ASSERT(async_pos && target_pos && async_pos < target_pos,
           "mi-async precedes extended-remote");

    close(fd);
    teardown(&ctx);
}

/* Client traffic through the full loop: send reaches gdb stdin; BREAK via
 * pin_control arrives as -exec-interrupt (the MI command that works under
 * mi-async — SIGINT does not; found live, 01e8e12). */
static void test_send_and_break_roundtrip(void)
{
    test_ctx_t ctx;
    setup(&ctx, NULL, 1);

    line_reader_t lr;
    int fd = hello(&lr);
    ASSERT(fd >= 0, "controller connected");
    if (fd < 0) { teardown(&ctx); return; }

    static char acc[65536];
    acc[0] = '\0';

    const char *cmd = "-data-list-target-features\n";
    send_json(fd, sm_msg_send("s1", (const uint8_t *)cmd, strlen(cmd)));
    ASSERT(collect_output_until(&lr, acc, sizeof(acc),
                                "-data-list-target-features", 300),
           "client command reached gdb stdin");

    send_json(fd, sm_msg_pin_control("b1", "break", "pulse", 0));
    ASSERT(collect_output_until(&lr, acc, sizeof(acc),
                                "-exec-interrupt", 300),
           "break delivered as -exec-interrupt");

    close(fd);
    teardown(&ctx);
}

/* The silence_normal broker guard (01e8e12): a quiet GDB link must NOT
 * degrade link health, while the same idle link without the flag must.
 * Health timing is shrunk via the test hooks so both cases run in ~1s. */
static void test_silence_normal_suppresses_idle_health(void)
{
    sm_broker_test_health_period_s = 0.05;
    sm_broker_test_idle_degraded_s = 0.2;
    sm_broker_test_idle_recovered_s = 0.1;

    /* GDB link (silence_normal=1): banner arrives, then silence well past
     * the degraded threshold — no link_health message may show up. */
    test_ctx_t ctx;
    setup(&ctx, NULL, 1);

    line_reader_t lr;
    int fd = hello(&lr);
    ASSERT(fd >= 0, "controller connected");
    if (fd >= 0) {
        int saw_health = 0;
        /* ~800ms = 4x the degraded threshold */
        for (int i = 0; i < 80; i++) {
            char *l = lr_next(&lr, 1);
            if (l && strstr(l, "\"link_health\""))
                saw_health = 1;
        }
        ASSERT(!saw_health, "no link_health broadcast for silent GDB link");
        close(fd);
    }
    teardown(&ctx);

    /* Control: identical setup with silence_normal cleared must broadcast
     * degraded — proves the guard (not slow timing) kept the case above
     * quiet. */
    setup(&ctx, NULL, 0);
    fd = hello(&lr);
    ASSERT(fd >= 0, "controller connected (control)");
    if (fd >= 0) {
        int saw_health = 0;
        for (int i = 0; i < 100 && !saw_health; i++) {
            char *l = lr_next(&lr, 1);
            if (l && strstr(l, "\"link_health\"") && strstr(l, "degraded"))
                saw_health = 1;
        }
        ASSERT(saw_health, "link without silence_normal degrades when idle");
        close(fd);
    }
    teardown(&ctx);

    sm_broker_test_health_period_s = 2.0;
    sm_broker_test_idle_degraded_s = 8.0;
    sm_broker_test_idle_recovered_s = 3.0;
}

int main(void)
{
    printf("test_gdb_broker\n");
    signal(SIGPIPE, SIG_IGN);
    unlink(TEST_SOCK);

    write_fake_gdb_script();

    RUN_TEST(test_spawn_sends_miasync_before_target);
    RUN_TEST(test_send_and_break_roundtrip);
    RUN_TEST(test_silence_normal_suppresses_idle_health);

    unlink(g_fake_gdb);
    TEST_REPORT();
}
