#include "test_main.h"
#include "links/link.h"

#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

/* Test-only constructor — not declared in gdb.h */
extern sm_link_t *sm_gdb_new_test(int stdin_fd, int stdout_fd);

/* Helper: create a pipe-based test link.
 * cmd_read: test reads commands GDB would receive (link's stdin)
 * out_write: test writes data as if GDB produced it (link's stdout) */
static sm_link_t *make_test_link(int *cmd_read, int *out_write)
{
    int cmd_pipe[2];  /* link writes to [1], test reads from [0] */
    int out_pipe[2];  /* test writes to [1], link reads from [0] */

    pipe(cmd_pipe);
    pipe(out_pipe);

    /* Non-blocking pipe ends (matches gdb_open / sm_gdb_new_test) */
    int flags = fcntl(cmd_pipe[1], F_GETFL, 0);
    fcntl(cmd_pipe[1], F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(out_pipe[0], F_GETFL, 0);
    fcntl(out_pipe[0], F_SETFL, flags | O_NONBLOCK);

    *cmd_read = cmd_pipe[0];
    *out_write = out_pipe[1];

    return sm_gdb_new_test(cmd_pipe[1], out_pipe[0]);
}

static void test_gdb_write_read(void)
{
    int cmd_read, out_write;
    sm_link_t *link = make_test_link(&cmd_read, &out_write);

    /* Write through link -> verify on cmd pipe */
    const uint8_t data[] = "-exec-continue\n";
    ASSERT_INT_EQ(link->write_data(link, data, sizeof(data) - 1), 0);

    char buf[256];
    ssize_t n = read(cmd_read, buf, sizeof(buf));
    ASSERT(n == 15, "read 15 bytes from cmd pipe");
    ASSERT(memcmp(buf, "-exec-continue\n", 15) == 0, "data matches");

    /* Write to out pipe -> verify via read_fd */
    const char *response = "^running\n(gdb)\n";
    write(out_write, response, strlen(response));

    usleep(10000);
    int fd = link->read_fd(link);
    ASSERT(fd >= 0, "read_fd valid");
    n = read(fd, buf, sizeof(buf));
    ASSERT(n > 0, "read from link fd");
    ASSERT(memcmp(buf, "^running\n(gdb)\n", (size_t)n) == 0, "response matches");

    link->close(link);
    link->destroy(link);
    close(cmd_read);
    close(out_write);
}

static void test_gdb_send_break(void)
{
    int cmd_read, out_write;
    sm_link_t *link = make_test_link(&cmd_read, &out_write);

    /* Effective on a live target only because gdb_open enables mi-async;
     * see the comment in gdb_send_break. */
    ASSERT_INT_EQ(link->send_break(link, 0), 0);

    char buf[256];
    ssize_t n = read(cmd_read, buf, sizeof(buf));
    ASSERT(n == 16, "read 16 bytes");
    ASSERT(memcmp(buf, "-exec-interrupt\n", 16) == 0, "-exec-interrupt sent");

    link->close(link);
    link->destroy(link);
    close(cmd_read);
    close(out_write);
}

static void test_gdb_get_status(void)
{
    int cmd_read, out_write;
    sm_link_t *link = make_test_link(&cmd_read, &out_write);

    cJSON *status = cJSON_CreateObject();
    link->get_status(link, status);

    ASSERT_STR_EQ(cJSON_GetObjectItem(status, "link_type")->valuestring, "gdb");
    ASSERT_NOT_NULL(cJSON_GetObjectItem(status, "gdb_path"));
    /* pid=0 in test mode, so connected is false */
    ASSERT(cJSON_IsFalse(cJSON_GetObjectItem(status, "connected")),
           "not connected (test mode)");

    cJSON_Delete(status);
    link->close(link);
    link->destroy(link);
    close(cmd_read);
    close(out_write);
}

static void test_gdb_set_param_target(void)
{
    int cmd_read, out_write;
    sm_link_t *link = make_test_link(&cmd_read, &out_write);

    ASSERT_INT_EQ(link->set_param(link, "target", "localhost:4444"), 0);

    char buf[256];
    ssize_t n = read(cmd_read, buf, sizeof(buf));
    ASSERT(n > 0, "read target command");
    buf[n] = '\0';
    ASSERT(strstr(buf, "extended-remote localhost:4444") != NULL,
           "target command sent");

    link->close(link);
    link->destroy(link);
    close(cmd_read);
    close(out_write);
}

static void test_gdb_set_param_unknown(void)
{
    int cmd_read, out_write;
    sm_link_t *link = make_test_link(&cmd_read, &out_write);

    ASSERT_INT_EQ(link->set_param(link, "baud", "115200"), -1);

    link->close(link);
    link->destroy(link);
    close(cmd_read);
    close(out_write);
}

static void test_gdb_close(void)
{
    int cmd_read, out_write;
    sm_link_t *link = make_test_link(&cmd_read, &out_write);

    link->close(link);
    ASSERT(link->read_fd(link) < 0, "fd closed after close()");

    link->destroy(link);
    close(cmd_read);
    close(out_write);
}

static void test_gdb_fork_exec(void)
{
    /* Create a temp script that ignores args and cats stdin.
     * GDB link passes --interpreter=mi3 --quiet, which /bin/cat would
     * try to open as files, so we need a wrapper that ignores them. */
    char tmppath[] = "/tmp/test_gdb_fake_XXXXXX";
    int tmpfd = mkstemp(tmppath);
    ASSERT(tmpfd >= 0, "mkstemp");
    const char *script = "#!/bin/sh\nexec cat\n";
    write(tmpfd, script, strlen(script));
    close(tmpfd);
    chmod(tmppath, 0755);

    extern sm_link_t *sm_gdb_new(const char *gdb_path, const char *target_spec);
    sm_link_t *link = sm_gdb_new(tmppath, NULL);
    ASSERT_NOT_NULL(link);

    ASSERT_INT_EQ(link->open(link), 0);

    int fd = link->read_fd(link);
    ASSERT(fd >= 0, "read_fd valid after open");

    /* Write data, wrapper cats stdin back to stdout */
    const uint8_t msg[] = "hello gdb\n";
    ASSERT_INT_EQ(link->write_data(link, msg, sizeof(msg) - 1), 0);

    /* Wait for echo */
    usleep(50000);

    char buf[256] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    ASSERT(n > 0, "read echoed data");
    /* open() sends "-gdb-set mi-async on" at spawn, so the echo stream is
     * that line followed by ours. */
    ASSERT(strstr(buf, "hello gdb\n") != NULL, "echo matches");

    link->close(link);
    ASSERT(link->read_fd(link) < 0, "fd closed");

    link->destroy(link);
    unlink(tmppath);
}

static void test_gdb_shell_blocked(void)
{
    int cmd_read, out_write;
    sm_link_t *link = make_test_link(&cmd_read, &out_write);

    /* Host code-execution commands are rejected by default (M5). Covers the
     * shell/pipe escapes and the interpreter commands (python/guile/make) plus
     * GDB's prefix abbreviations, which all reach arbitrary code. */
    const char *bad[] = {
        "shell rm -rf /\n",
        "!ls\n",
        "pipe info registers | grep r0\n",
        "42shell id\n",                                /* MI token prefix */
        "-interpreter-exec console \"shell id\"\n",
        "python import os; os.system('id')\n",          /* arbitrary Python */
        "py import os\n",                               /* python abbreviation */
        "python-interactive\n",                         /* longer spelling */
        "guile (system \"id\")\n",                      /* arbitrary Guile */
        "gu (system \"id\")\n",                         /* guile abbreviation */
        "pi info registers\n",                          /* pipe abbreviation */
        "make -f /tmp/evil.mk\n",                       /* spawns make -> shell */
        "-interpreter-exec console \"python import os\"\n",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        ASSERT(link->write_data(link, (const uint8_t *)bad[i],
                                strlen(bad[i])) != 0,
               "shell-invoking command blocked");

    /* Benign commands pass, including symbols containing 'shell' and console
     * commands whose names share no prefix with a dangerous verb. */
    const char *ok = "-break-insert my_shell_handler\n";
    ASSERT_INT_EQ(link->write_data(link, (const uint8_t *)ok, strlen(ok)), 0);
    char buf[256];
    ssize_t n = read(cmd_read, buf, sizeof(buf));
    ASSERT(n == (ssize_t)strlen(ok), "benign command passed through");

    const char *ok2 = "print make_counter\n";  /* 'print' != make/pipe/python */
    ASSERT_INT_EQ(link->write_data(link, (const uint8_t *)ok2, strlen(ok2)), 0);
    n = read(cmd_read, buf, sizeof(buf));
    ASSERT(n == (ssize_t)strlen(ok2), "benign 'print' passed through");

    /* Explicit opt-in re-enables shell commands */
    ASSERT_INT_EQ(link->set_param(link, "allow_shell", "1"), 0);
    const char *sh = "shell echo hi\n";
    ASSERT_INT_EQ(link->write_data(link, (const uint8_t *)sh, strlen(sh)), 0);
    n = read(cmd_read, buf, sizeof(buf));
    ASSERT(n == (ssize_t)strlen(sh), "shell passes after allow_shell=1");

    link->close(link);
    link->destroy(link);
    close(cmd_read);
    close(out_write);
}

static void test_gdb_write_nonblocking(void)
{
    int cmd_read, out_write;
    sm_link_t *link = make_test_link(&cmd_read, &out_write);

    const uint8_t chunk[] = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n";
    for (int i = 0; i < 4096; i++) {
        if (link->write_data(link, chunk, sizeof(chunk) - 1) != 0)
            break;
    }

    uint8_t big[8192];
    memset(big, 'y', sizeof(big));

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    link->write_data(link, big, sizeof(big));
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double elapsed_ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
                        (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    ASSERT(elapsed_ms < 100.0, "gdb write returns within single poll budget");

    link->close(link);
    link->destroy(link);
    close(cmd_read);
    close(out_write);
}

/* gdb_open must not block on the (gdb) prompt: with a fake gdb that never
 * emits the prompt, open() (called on the broker event loop during initial
 * open / resume / reconnect) returns promptly instead of waiting ~10s, and the
 * target-connect command is queued for delivery once gdb reads stdin. */
static void test_gdb_open_nonblocking_target(void)
{
    const char *tmp = getenv("TMPDIR");
    char tmppath[256];
    snprintf(tmppath, sizeof(tmppath), "%s/test_gdb_fake_XXXXXX",
             tmp && tmp[0] ? tmp : "/tmp");
    int tmpfd = mkstemp(tmppath);
    ASSERT(tmpfd >= 0, "mkstemp");
    const char *script = "#!/bin/sh\nexec cat\n";  /* echoes stdin, never prints (gdb) */
    write(tmpfd, script, strlen(script));
    close(tmpfd);
    chmod(tmppath, 0755);

    extern sm_link_t *sm_gdb_new(const char *gdb_path, const char *target_spec);
    sm_link_t *link = sm_gdb_new(tmppath, "localhost:3333");
    ASSERT_NOT_NULL(link);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    ASSERT_INT_EQ(link->open(link), 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
                (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    ASSERT(ms < 2000.0, "gdb_open returned promptly (did not block on the prompt)");

    /* Flush the queued 'extended-remote' to the fake gdb; cat echoes it back. */
    for (int i = 0; i < 100 && link->has_write_pending(link); i++)
        link->flush_write_queue(link);

    int fd = link->read_fd(link);
    char buf[256] = {0};
    size_t total = 0;
    for (int i = 0; i < 100 && total < sizeof(buf) - 1; i++) {
        ssize_t n = read(fd, buf + total, sizeof(buf) - 1 - total);
        if (n > 0) {
            total += (size_t)n;
            buf[total] = '\0';
            if (strstr(buf, "extended-remote")) break;
        } else {
            usleep(10000);
        }
    }
    ASSERT(strstr(buf, "extended-remote localhost:3333") != NULL,
           "target-connect command was queued and delivered");

    /* mi-async must be enabled before the target connect — gdb rejects the
     * setting once attached to a live inferior. */
    char *async_at = strstr(buf, "-gdb-set mi-async on");
    ASSERT(async_at != NULL, "mi-async enable was sent");
    ASSERT(async_at < strstr(buf, "extended-remote"),
           "mi-async precedes the target connect");

    link->close(link);
    link->destroy(link);
    unlink(tmppath);
}

static void test_gdb_silence_normal(void)
{
    int cmd_read, out_write;
    sm_link_t *link = make_test_link(&cmd_read, &out_write);

    /* GDB links are legitimately silent while the target runs or sits
     * halted; the broker must not raise idle-degraded link_health for them. */
    ASSERT(link->silence_normal == 1, "gdb link declares silence as normal");

    link->close(link);
    link->destroy(link);
    close(cmd_read);
    close(out_write);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    printf("test_gdb\n");

    RUN_TEST(test_gdb_write_read);
    RUN_TEST(test_gdb_send_break);
    RUN_TEST(test_gdb_get_status);
    RUN_TEST(test_gdb_set_param_target);
    RUN_TEST(test_gdb_set_param_unknown);
    RUN_TEST(test_gdb_close);
    RUN_TEST(test_gdb_fork_exec);
    RUN_TEST(test_gdb_shell_blocked);
    RUN_TEST(test_gdb_write_nonblocking);
    RUN_TEST(test_gdb_open_nonblocking_target);
    RUN_TEST(test_gdb_silence_normal);

    TEST_REPORT();
}
