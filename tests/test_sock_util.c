#include "test_main.h"
#include "util/sock_util.h"
#include "constants.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

    TEST_REPORT();
}
