#include "test_main.h"
#include "util/sock_util.h"
#include "constants.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static void test_parse_host_and_port(void)
{
    char host[64];
    int port = 5555;

    sm_parse_host_port("example.com:1234", host, sizeof(host), &port);
    ASSERT_STR_EQ(host, "example.com");
    ASSERT_INT_EQ(port, 1234);

    port = 5555;
    sm_parse_host_port("192.168.1.5:9000", host, sizeof(host), &port);
    ASSERT_STR_EQ(host, "192.168.1.5");
    ASSERT_INT_EQ(port, 9000);
}

static void test_parse_bare_host_keeps_default_port(void)
{
    char host[64];
    int port = 4321;

    sm_parse_host_port("brokerbox", host, sizeof(host), &port);
    ASSERT_STR_EQ(host, "brokerbox");
    ASSERT_INT_EQ(port, 4321);
}

static void test_parse_ipv6_uses_last_colon(void)
{
    char host[64];
    int port = 0;

    sm_parse_host_port("::1:8080", host, sizeof(host), &port);
    ASSERT_STR_EQ(host, "::1");
    ASSERT_INT_EQ(port, 8080);
}

static void test_parse_leading_colon_is_bare_host(void)
{
    /* A colon at position 0 is not treated as a separator (empty host would
     * be useless); the whole spec becomes the host, port keeps its default. */
    char host[64];
    int port = 7777;

    sm_parse_host_port(":8080", host, sizeof(host), &port);
    ASSERT_STR_EQ(host, ":8080");
    ASSERT_INT_EQ(port, 7777);
}

static void test_parse_empty_port_yields_zero(void)
{
    /* "host:" → atoi("") == 0; callers see port 0 and fail the connect. */
    char host[64];
    int port = 1111;

    sm_parse_host_port("example.com:", host, sizeof(host), &port);
    ASSERT_STR_EQ(host, "example.com");
    ASSERT_INT_EQ(port, 0);
}

static void test_parse_host_truncated_to_buffer(void)
{
    char host[8];
    int port = 0;

    sm_parse_host_port("verylonghostname.example.com:80", host, sizeof(host), &port);
    ASSERT_STR_EQ(host, "verylon");   /* 7 chars + NUL */
    ASSERT_INT_EQ(port, 80);
}

static void test_write_all_roundtrip(void)
{
    int fds[2];
    ASSERT_INT_EQ(pipe(fds), 0);

    const char msg[] = "hello over the pipe";
    ASSERT_INT_EQ(sm_write_all(fds[1], msg, sizeof(msg)), 0);

    char buf[64] = {0};
    ssize_t n = read(fds[0], buf, sizeof(buf));
    ASSERT_INT_EQ((int)n, (int)sizeof(msg));
    ASSERT_STR_EQ(buf, msg);

    close(fds[0]);
    close(fds[1]);
}

static void test_write_all_fails_on_closed_fd(void)
{
    int fds[2];
    ASSERT_INT_EQ(pipe(fds), 0);
    close(fds[0]);
    close(fds[1]);
    ASSERT_INT_EQ(sm_write_all(fds[1], "x", 1), -1);
}

static void test_discover_all_sockets(void)
{
    const char *tmp = getenv("TMPDIR");
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/smolmux-disc-%d",
             tmp && tmp[0] ? tmp : "/tmp", (int)getpid());
    if (mkdir(dir, 0700) != 0)
        ASSERT(errno == EEXIST, "temp dir created");

    /* Two fake broker sockets in a private XDG_RUNTIME_DIR (glob matches by
     * name, so plain files are fine). */
    char a[512], b[512];
    snprintf(a, sizeof(a), "%s/smolmux-alpha.sock", dir);
    snprintf(b, sizeof(b), "%s/smolmux-beta.sock", dir);
    fclose(fopen(a, "w"));
    fclose(fopen(b, "w"));

    char saved_xdg[512] = {0};
    const char *prev = getenv("XDG_RUNTIME_DIR");
    if (prev) snprintf(saved_xdg, sizeof(saved_xdg), "%s", prev);
    setenv("XDG_RUNTIME_DIR", dir, 1);
    unsetenv(SM_SOCKET_ENV);

    char out[32][SM_SOCK_PATH_MAX];
    size_t n = sm_discover_all_sockets(out, 32);
    /* At least our two (the /tmp glob may add unrelated live sockets). */
    ASSERT(n >= 2, "found at least the two fake sockets");

    int found_a = 0, found_b = 0;
    for (size_t i = 0; i < n && i < 32; i++) {
        if (strcmp(out[i], a) == 0) found_a = 1;
        if (strcmp(out[i], b) == 0) found_b = 1;
    }
    ASSERT(found_a, "alpha socket discovered");
    ASSERT(found_b, "beta socket discovered");

    /* SMOLMUX_SOCKET is included and de-duplicated. */
    setenv(SM_SOCKET_ENV, a, 1);
    n = sm_discover_all_sockets(out, 32);
    int a_count = 0;
    for (size_t i = 0; i < n && i < 32; i++)
        if (strcmp(out[i], a) == 0) a_count++;
    ASSERT_INT_EQ(a_count, 1);   /* env dupes the glob hit -> counted once */

    unsetenv(SM_SOCKET_ENV);
    if (saved_xdg[0]) setenv("XDG_RUNTIME_DIR", saved_xdg, 1);
    else unsetenv("XDG_RUNTIME_DIR");
    unlink(a);
    unlink(b);
    rmdir(dir);
}

/* Synthetic long by-id basename (no real device serial) for golden + length. */
#define TEST_BYID_BASE \
    "usb-Prolific_Technology_Inc._USB-Serial_Controller_TESTSERIAL00-if00-port0"
#define TEST_BYID_PATH "/dev/serial/by-id/" TEST_BYID_BASE
/* Golden: FNV-1a of TEST_BYID_BASE under /run/user/1000, shortened to fit
 * SM_SOCK_FINAL_MAX (locks format against silent drift). */
#define TEST_BYID_GOLDEN \
    "/run/user/1000/smolmux-usb-Prolific_Technology_Inc._USB-Serial_" \
    "Controller_TES-10042336.sock"

static void test_derive_socket_short_device(void)
{
    setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1);
    char out[SM_SOCK_PATH_MAX];
    ASSERT_INT_EQ(sm_derive_socket_path(out, sizeof(out), "/dev/ttyUSB0"), 0);
    ASSERT_STR_EQ(out, "/run/user/1000/smolmux-ttyUSB0.sock");
    ASSERT(strlen(out) <= SM_SOCK_FINAL_MAX, "short path within final max");
    unsetenv("XDG_RUNTIME_DIR");
}

static void test_derive_socket_tmp_fallback(void)
{
    /* No XDG_RUNTIME_DIR => /tmp (still readable short name). */
    unsetenv("XDG_RUNTIME_DIR");
    char out[SM_SOCK_PATH_MAX];
    ASSERT_INT_EQ(sm_derive_socket_path(out, sizeof(out), "/dev/ttyACM1"), 0);
    ASSERT_STR_EQ(out, "/tmp/smolmux-ttyACM1.sock");
}

static void test_derive_socket_long_byid_fits_bind(void)
{
    /* Realistic long USB by-id basename that used to overflow sun_path with
     * the broker's "<sock>.<pid>.tmp" rename bind. */
    setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1);
    char out[SM_SOCK_PATH_MAX];
    ASSERT_INT_EQ(sm_derive_socket_path(out, sizeof(out), TEST_BYID_PATH), 0);
    ASSERT(strlen(out) <= SM_SOCK_FINAL_MAX, "long by-id path fits final max");
    ASSERT(strstr(out, "/run/user/1000/smolmux-") == out, "under runtime dir");
    ASSERT(strstr(out, ".sock") != NULL, "ends with .sock");
    /* Temp bind name must also fit sun_path (108). */
    char tmp_bind[SM_SOCK_PATH_MAX + 32];
    int tlen = snprintf(tmp_bind, sizeof(tmp_bind), "%s.%d.tmp", out, 1234567890);
    ASSERT(tlen > 0 && tlen < SM_SOCK_PATH_MAX, "temp bind name fits sun_path");
    /* Stable: same input -> same path */
    char out2[SM_SOCK_PATH_MAX];
    ASSERT_INT_EQ(sm_derive_socket_path(out2, sizeof(out2), TEST_BYID_PATH), 0);
    ASSERT_STR_EQ(out, out2);
    unsetenv("XDG_RUNTIME_DIR");
}

static void test_derive_socket_long_byid_golden(void)
{
    setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1);
    char out[SM_SOCK_PATH_MAX];
    ASSERT_INT_EQ(sm_derive_socket_path(out, sizeof(out), TEST_BYID_PATH), 0);
    ASSERT_STR_EQ(out, TEST_BYID_GOLDEN);
    unsetenv("XDG_RUNTIME_DIR");
}

static void test_derive_socket_rejects_bad_args(void)
{
    char out[SM_SOCK_PATH_MAX];
    ASSERT_INT_EQ(sm_derive_socket_path(out, sizeof(out), ""), -1);
    ASSERT_INT_EQ(sm_derive_socket_path(out, sizeof(out), NULL), -1);
    ASSERT_INT_EQ(sm_derive_socket_path(NULL, sizeof(out), "/dev/ttyUSB0"), -1);
    ASSERT_INT_EQ(sm_derive_socket_path(out, 0, "/dev/ttyUSB0"), -1);
    /* Tiny buffer cannot hold even a shortened path. */
    char tiny[8];
    ASSERT_INT_EQ(sm_derive_socket_path(tiny, sizeof(tiny), "/dev/ttyUSB0"), -1);
    ASSERT_INT_EQ(sm_derive_board_socket_path(out, sizeof(out), "", "console"), -1);
    ASSERT_INT_EQ(sm_derive_board_socket_path(out, sizeof(out), "b", ""), -1);
}

static void test_derive_board_socket_shortens(void)
{
    setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1);
    char out[SM_SOCK_PATH_MAX];
    ASSERT_INT_EQ(sm_derive_board_socket_path(out, sizeof(out),
                                              "myboard", "console"), 0);
    ASSERT_STR_EQ(out, "/run/user/1000/smolmux-myboard-console.sock");

    char long_board[96];
    memset(long_board, 'B', sizeof(long_board) - 1);
    long_board[sizeof(long_board) - 1] = '\0';
    ASSERT_INT_EQ(sm_derive_board_socket_path(out, sizeof(out),
                                              long_board, "console"), 0);
    ASSERT(strlen(out) <= SM_SOCK_FINAL_MAX, "long board tag fits");
    unsetenv("XDG_RUNTIME_DIR");
}

static void test_by_id_weak_heuristic(void)
{
    ASSERT_INT_EQ(sm_serial_by_id_is_weak(
        "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0"), 1);
    /* Synthetic 6+ hex run (not a desk serial). */
    ASSERT_INT_EQ(sm_serial_by_id_is_weak(
        "/dev/serial/by-id/usb-1a86_USB_Single_Serial_ABCDEF012345-if00"), 0);
    ASSERT_INT_EQ(sm_serial_by_id_is_weak("/dev/ttyUSB0"), 0);
    ASSERT_INT_EQ(sm_serial_by_id_is_weak(NULL), 0);
}

static void test_refuse_weak_seat_change(void)
{
    ASSERT_INT_EQ(sm_identity_refuse_weak_seat_change(0, "seat-a", "seat-b"), 0);
    /* Fail closed: empty last seat is not a silent rebind. */
    ASSERT_INT_EQ(sm_identity_refuse_weak_seat_change(1, "", "seat-b"), 1);
    ASSERT_INT_EQ(sm_identity_refuse_weak_seat_change(1, NULL, "seat-b"), 1);
    ASSERT_INT_EQ(sm_identity_refuse_weak_seat_change(1, "seat-a", "seat-a"), 0);
    ASSERT_INT_EQ(sm_identity_refuse_weak_seat_change(1, "seat-a", "seat-b"), 1);
    /* Fail closed: unresolved now-seat is not a silent rebind. */
    ASSERT_INT_EQ(sm_identity_refuse_weak_seat_change(1, "seat-a", ""), 1);
    ASSERT_INT_EQ(sm_identity_refuse_weak_seat_change(1, "seat-a", NULL), 1);
}

static void test_named_board_ambiguous(void)
{
    ASSERT_INT_EQ(sm_identity_named_board_is_ambiguous(
        0, 1, NULL, NULL, "seat-b", 0), 0);
    ASSERT_INT_EQ(sm_identity_named_board_is_ambiguous(
        1, 0, NULL, NULL, "seat-b", 0), 0);
    ASSERT_INT_EQ(sm_identity_named_board_is_ambiguous(
        1, 1, NULL, NULL, "seat-b", 1), 0);
    ASSERT_INT_EQ(sm_identity_named_board_is_ambiguous(
        1, 1, NULL, NULL, "seat-b", 0), 1);
    ASSERT_INT_EQ(sm_identity_named_board_is_ambiguous(
        1, 1, "seat", "seat-a", "seat-a", 0), 0);
    ASSERT_INT_EQ(sm_identity_named_board_is_ambiguous(
        1, 1, "seat", "seat-a", "seat-b", 0), 1);
    ASSERT_INT_EQ(sm_identity_named_board_is_ambiguous(
        1, 1, "seat", "seat-a", "", 0), 1);
    ASSERT_INT_EQ(sm_identity_named_board_is_ambiguous(
        1, 1, "unique_serial", NULL, "seat-a", 0), 1);
}

static const char *scratch_tmp(void)
{
    const char *t = getenv("TMPDIR");
    return (t && t[0]) ? t : "/tmp";
}

static void test_wait_path_exists_now(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/smolmux-wait-now-%d", scratch_tmp(),
             (int)getpid());
    FILE *f = fopen(path, "w");
    ASSERT_NOT_NULL(f);
    fclose(f);
    ASSERT_INT_EQ(sm_wait_path_exists(path, 0.0, 1000), 0);
    unlink(path);
}

static void test_wait_path_timeout(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/smolmux-wait-miss-%d", scratch_tmp(),
             (int)getpid());
    unlink(path);
    ASSERT_INT_EQ(sm_wait_path_exists(path, 0.0, 1000), -1);
    ASSERT_INT_EQ(sm_wait_path_exists(NULL, 1.0, 1000), -1);
}

static void test_wait_path_appears_mid_wait(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/smolmux-wait-mid-%d", scratch_tmp(),
             (int)getpid());
    unlink(path);
    pid_t pid = fork();
    ASSERT(pid >= 0, "fork");
    if (pid == 0) {
        usleep(80000);
        FILE *f = fopen(path, "w");
        if (f) fclose(f);
        _exit(0);
    }
    int rc = sm_wait_path_exists(path, 2.0, 10000);
    int st = 0;
    waitpid(pid, &st, 0);
    unlink(path);
    ASSERT_INT_EQ(rc, 0);
}

static void test_autodiscover_should_refuse(void)
{
    unsetenv("SMOLMUX_SOCKET");
    ASSERT_INT_EQ(sm_autodiscover_should_refuse(1), 0);

    char socks[32][SM_SOCK_PATH_MAX];
    size_t n = sm_discover_all_sockets(socks, 32);
    if (n > 1) {
        ASSERT_INT_EQ(sm_autodiscover_should_refuse(0), 1);
        return;
    }

    char dir[128];
    snprintf(dir, sizeof(dir), "%s/smad-%d", scratch_tmp(), (int)getpid());
    ASSERT_INT_EQ(mkdir(dir, 0700), 0);
    setenv("XDG_RUNTIME_DIR", dir, 1);
    char a[160], b[160];
    snprintf(a, sizeof(a), "%s/smolmux-a.sock", dir);
    snprintf(b, sizeof(b), "%s/smolmux-b.sock", dir);
    FILE *fa = fopen(a, "w");
    FILE *fb = fopen(b, "w");
    ASSERT_NOT_NULL(fa);
    ASSERT_NOT_NULL(fb);
    fclose(fa);
    fclose(fb);
    ASSERT_INT_EQ(sm_autodiscover_should_refuse(0), 1);
    ASSERT_INT_EQ(sm_autodiscover_should_refuse(1), 0);
    unlink(a);
    unlink(b);
    unsetenv("XDG_RUNTIME_DIR");
    rmdir(dir);
}

static void test_gc_helpers(void)
{
    ASSERT_INT_EQ(sm_client_name_is_mcp("claude-mcp"), 1);
    ASSERT_INT_EQ(sm_client_name_is_mcp("smolmux-mcp"), 1);
    ASSERT_INT_EQ(sm_client_name_is_mcp("foo-mcp"), 1);
    ASSERT_INT_EQ(sm_client_name_is_mcp("monitor"), 0);
    ASSERT_INT_EQ(sm_client_name_is_mcp("smolmux-cli"), 0);
    ASSERT_INT_EQ(sm_gc_should_kill("claude-mcp", 0, 10, 0), 1);
    ASSERT_INT_EQ(sm_gc_should_kill("claude-mcp", 1, 1, 0), 1);
    ASSERT_INT_EQ(sm_gc_should_kill("claude-mcp", 1, 400, 0), 0);
    ASSERT_INT_EQ(sm_gc_should_kill("claude-mcp", 1, 400, 1), 1);
    ASSERT_INT_EQ(sm_gc_should_kill("monitor", 1, 1, 1), 0);
    int pids[4];
    ASSERT_INT_EQ(sm_unix_mcp_peer_pids("/no/such.sock", 0, pids, 4), 0);
    ASSERT_INT_EQ(sm_unix_mcp_peer_pids("", 0, pids, 4), 0);
    ASSERT_INT_EQ(sm_unix_mcp_peer_pids(NULL, 0, pids, 4), 0);

    ASSERT_INT_EQ(sm_unix_sock_name_matches("/tmp/a.sock", "/tmp/a.sock"), 1);
    ASSERT_INT_EQ(sm_unix_sock_name_matches("/tmp/a.sock.2227926.tmp",
                                            "/tmp/a.sock"), 1);
    ASSERT_INT_EQ(sm_unix_sock_name_matches("/tmp/a.sock.1.tmp",
                                            "/tmp/a.sock"), 1);
    ASSERT_INT_EQ(sm_unix_sock_name_matches("/tmp/a.sock.tmp",
                                            "/tmp/a.sock"), 0);
    ASSERT_INT_EQ(sm_unix_sock_name_matches("/tmp/a.sock.12a.tmp",
                                            "/tmp/a.sock"), 0);
    ASSERT_INT_EQ(sm_unix_sock_name_matches("/tmp/a.sock.1.tmpx",
                                            "/tmp/a.sock"), 0);
    ASSERT_INT_EQ(sm_unix_sock_name_matches("/tmp/a.sockx.1.tmp",
                                            "/tmp/a.sock"), 0);
    ASSERT_INT_EQ(sm_unix_sock_name_matches("/tmp/a.sock-b.sock",
                                            "/tmp/a.sock"), 0);
    ASSERT_INT_EQ(sm_unix_sock_name_matches("/tmp/b.sock.1.tmp",
                                            "/tmp/a.sock"), 0);
    ASSERT_INT_EQ(sm_unix_sock_name_matches("", "/tmp/a.sock"), 0);
    ASSERT_INT_EQ(sm_unix_sock_name_matches(NULL, "/tmp/a.sock"), 0);
}

/* Same bind-then-rename as the broker: kernel name stays path.<pid>.tmp. */
static int listen_unix(const char *path)
{
    char tmp[sizeof(((struct sockaddr_un *)0)->sun_path)];
    int n = snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid());
    if (n < 0 || n >= (int)sizeof(tmp))
        return -1;
    unlink(path);
    unlink(tmp);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    memcpy(a.sun_path, tmp, (size_t)n + 1);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        listen(fd, 8) != 0 ||
        rename(tmp, path) != 0) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    return fd;
}

static pid_t spawn_named_unix_client(const char *comm, const char *path)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        /* fork keeps the parent's listen fds; drop them so we only
         * hold the client socket we are about to create. */
        int fdlimit = (int)sysconf(_SC_OPEN_MAX);
        if (fdlimit < 0 || fdlimit > 1024)
            fdlimit = 1024;
        for (int fd = 3; fd < fdlimit; fd++)
            close(fd);
        prctl(PR_SET_NAME, comm, 0, 0, 0);
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un a;
        memset(&a, 0, sizeof(a));
        a.sun_family = AF_UNIX;
        snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
        if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0)
            _exit(2);
        pause();
        _exit(0);
    }
    return pid;
}

static void reap_pid(pid_t pid)
{
    if (pid <= 0)
        return;
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

static int pids_contain(const int *pids, int n, int want)
{
    for (int i = 0; i < n; i++) {
        if (pids[i] == want)
            return 1;
    }
    return 0;
}

/* SOCK_DIAG + comm filter: MCP child on A, not the B peer or a monitor. */
static void test_unix_mcp_peer_pids_live(void)
{
    char pa[160], pb[160];
    snprintf(pa, sizeof(pa), "%s/smolmux-gc-a-%d.sock", scratch_tmp(),
             (int)getpid());
    snprintf(pb, sizeof(pb), "%s/smolmux-gc-b-%d.sock", scratch_tmp(),
             (int)getpid());

    int la = listen_unix(pa);
    int lb = listen_unix(pb);
    ASSERT(la >= 0, "listen A");
    ASSERT(lb >= 0, "listen B");
    if (la < 0 || lb < 0) {
        if (la >= 0)
            close(la);
        if (lb >= 0)
            close(lb);
        unlink(pa);
        unlink(pb);
        return;
    }
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(la, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(lb, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    pid_t ca = spawn_named_unix_client("smolmux-mcp", pa);
    pid_t cb = spawn_named_unix_client("smolmux-mcp", pb);
    pid_t cc = spawn_named_unix_client("smolmux-monitor", pa);
    ASSERT(ca > 0 && cb > 0 && cc > 0, "spawn clients");

    /* The ready pipes were closed in spawn; wait by accepting instead.
     * connect() on AF_UNIX STREAM completes once queued. */
    int aa = accept(la, NULL, NULL);
    int ab = accept(lb, NULL, NULL);
    int ac = accept(la, NULL, NULL);
    ASSERT(aa >= 0 && ab >= 0 && ac >= 0, "accept clients");

    int pids[8];
    int na = sm_unix_mcp_peer_pids(pa, (int)getpid(), pids, 8);
    if (na < 0) {
        /* SOCK_DIAG is missing under qemu-user (aarch64 release tests). */
        printf("    skip live peer pids (SOCK_DIAG unavailable)\n");
    } else {
        ASSERT_INT_EQ(na, 1);
        ASSERT_INT_EQ(pids_contain(pids, na, (int)ca), 1);
        ASSERT_INT_EQ(pids_contain(pids, na, (int)cb), 0);
        ASSERT_INT_EQ(pids_contain(pids, na, (int)cc), 0);

        int nb = sm_unix_mcp_peer_pids(pb, (int)getpid(), pids, 8);
        ASSERT_INT_EQ(nb, 1);
        ASSERT_INT_EQ(pids_contain(pids, nb, (int)cb), 1);
        ASSERT_INT_EQ(pids_contain(pids, nb, (int)ca), 0);
    }

    reap_pid(ca);
    reap_pid(cb);
    reap_pid(cc);
    if (aa >= 0)
        close(aa);
    if (ab >= 0)
        close(ab);
    if (ac >= 0)
        close(ac);
    close(la);
    close(lb);
    unlink(pa);
    unlink(pb);
}

int main(void)
{
    printf("test_sock_util\n");

    RUN_TEST(test_discover_all_sockets);
    RUN_TEST(test_derive_socket_short_device);
    RUN_TEST(test_derive_socket_tmp_fallback);
    RUN_TEST(test_derive_socket_long_byid_fits_bind);
    RUN_TEST(test_derive_socket_long_byid_golden);
    RUN_TEST(test_derive_socket_rejects_bad_args);
    RUN_TEST(test_derive_board_socket_shortens);
    RUN_TEST(test_parse_host_and_port);
    RUN_TEST(test_parse_bare_host_keeps_default_port);
    RUN_TEST(test_parse_ipv6_uses_last_colon);
    RUN_TEST(test_parse_leading_colon_is_bare_host);
    RUN_TEST(test_parse_empty_port_yields_zero);
    RUN_TEST(test_parse_host_truncated_to_buffer);
    RUN_TEST(test_write_all_roundtrip);
    RUN_TEST(test_write_all_fails_on_closed_fd);
    RUN_TEST(test_by_id_weak_heuristic);
    RUN_TEST(test_refuse_weak_seat_change);
    RUN_TEST(test_autodiscover_should_refuse);
    RUN_TEST(test_gc_helpers);
    RUN_TEST(test_unix_mcp_peer_pids_live);
    RUN_TEST(test_named_board_ambiguous);
    RUN_TEST(test_wait_path_exists_now);
    RUN_TEST(test_wait_path_timeout);
    RUN_TEST(test_wait_path_appears_mid_wait);

    TEST_REPORT();
}
