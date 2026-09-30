/*
 * test_auth_token — ACT-029 generated tokens for loopback TCP/WS.
 *
 * Unit: util/auth_token (generate, write 0600, loopback test, client
 * autoload). End to end: the real smolmux binary on a PTY with --tcp-port
 * writes a 0600 token file, refuses a hello without the token, accepts one
 * with the autoloaded token, removes the file on exit, and --insecure-no-auth
 * still serves tokenless.
 */

#include "test_main.h"
#include "protocol.h"
#include "util/auth_token.h"
#include "util/json_helpers.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char g_dir[256];

static void make_dir(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(g_dir, sizeof(g_dir), "%s/smtok-XXXXXX",
             (base && base[0]) ? base : "/tmp");
    ASSERT_NOT_NULL(mkdtemp(g_dir));
    setenv("XDG_RUNTIME_DIR", g_dir, 1);
}

static const char *find_bin(char *buf, size_t len, const char *name)
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
    if ((size_t)snprintf(buf, len, "%s/%s", exe, name) >= len)
        return NULL;
    return access(buf, X_OK) == 0 ? buf : NULL;
}

/* --- unit --- */

static void test_generate_is_hex_and_random(void)
{
    char a[SM_AUTH_TOKEN_HEX_LEN + 1], b[SM_AUTH_TOKEN_HEX_LEN + 1];
    ASSERT_INT_EQ(sm_auth_token_generate(a, sizeof(a)), 0);
    ASSERT_INT_EQ(sm_auth_token_generate(b, sizeof(b)), 0);
    ASSERT_INT_EQ((int)strlen(a), SM_AUTH_TOKEN_HEX_LEN);
    ASSERT(strspn(a, "0123456789abcdef") == SM_AUTH_TOKEN_HEX_LEN, "hex only");
    ASSERT(strcmp(a, b) != 0, "two tokens differ");
    char small[16];
    ASSERT_INT_EQ(sm_auth_token_generate(small, sizeof(small)), -1);
}

static void test_write_is_0600_and_refuses_symlink(void)
{
    make_dir();
    char path[512];
    ASSERT_INT_EQ(sm_auth_token_path(path, sizeof(path), "tcp", 5555), 0);
    char want[300];
    snprintf(want, sizeof(want), "%s/smolmux-tcp-5555.token", g_dir);
    ASSERT_STR_EQ(path, want);

    ASSERT_INT_EQ(sm_auth_token_write(path, "abc123"), 0);
    struct stat st;
    ASSERT_INT_EQ(stat(path, &st), 0);
    ASSERT_INT_EQ((int)(st.st_mode & 0777), 0600);
    char buf[64] = {0};
    int fd = open(path, O_RDONLY);
    ASSERT(read(fd, buf, sizeof(buf) - 1) == 7, "token plus newline");
    close(fd);
    ASSERT_STR_EQ(buf, "abc123\n");

    /* Rewrite over an existing (own, 0644) file: truncated, back to 0600. */
    chmod(path, 0644);
    ASSERT_INT_EQ(sm_auth_token_write(path, "zz"), 0);
    ASSERT_INT_EQ(stat(path, &st), 0);
    ASSERT_INT_EQ((int)(st.st_mode & 0777), 0600);
    ASSERT_INT_EQ((int)st.st_size, 3);
    unlink(path);

    /* A symlink planted at the path is refused, and its target untouched. */
    char target[300], lnk[300];
    snprintf(target, sizeof(target), "%s/target", g_dir);
    snprintf(lnk, sizeof(lnk), "%s/smolmux-ws-7777.token", g_dir);
    int tfd = open(target, O_WRONLY | O_CREAT, 0600);
    ASSERT(tfd >= 0, "target");
    close(tfd);
    ASSERT_INT_EQ(symlink(target, lnk), 0);
    ASSERT_INT_EQ(sm_auth_token_write(lnk, "leak"), -1);
    ASSERT_INT_EQ(stat(target, &st), 0);
    ASSERT_INT_EQ((int)st.st_size, 0);

    /* A second hard link means someone else can reach the file: refused. */
    char hl[300];
    snprintf(hl, sizeof(hl), "%s/hardlink", g_dir);
    ASSERT_INT_EQ(link(target, hl), 0);
    ASSERT_INT_EQ(sm_auth_token_write(target, "tok"), -1);

    unlink(lnk);
    unlink(hl);
    unlink(target);
    rmdir(g_dir);
}

static void test_host_is_loopback(void)
{
    ASSERT(sm_host_is_loopback("127.0.0.1"), "127.0.0.1");
    ASSERT(sm_host_is_loopback("127.1.2.3"), "127/8");
    ASSERT(sm_host_is_loopback("localhost"), "localhost");
    ASSERT(sm_host_is_loopback("::1"), "::1");
    ASSERT(!sm_host_is_loopback("192.168.1.10"), "LAN");
    ASSERT(!sm_host_is_loopback("example.com"), "name");
    ASSERT(!sm_host_is_loopback(""), "empty");
}

static void test_autoload_rules(void)
{
    make_dir();
    char path[512];
    sm_auth_token_path(path, sizeof(path), "tcp", 6001);

    /* No file: nothing to load. */
    unsetenv("SMOLMUX_AUTH_TOKEN");
    ASSERT_INT_EQ(sm_auth_token_autoload("127.0.0.1", 6001), 0);
    ASSERT(getenv("SMOLMUX_AUTH_TOKEN") == NULL, "env untouched");

    /* File present, loopback: loaded. */
    ASSERT_INT_EQ(sm_auth_token_write(path, "first"), 0);
    ASSERT_INT_EQ(sm_auth_token_autoload("127.0.0.1", 6001), 1);
    ASSERT_STR_EQ(getenv("SMOLMUX_AUTH_TOKEN"), "first");

    /* A restarted broker wrote a new token: re-read on the next connect. */
    ASSERT_INT_EQ(sm_auth_token_write(path, "second"), 0);
    ASSERT_INT_EQ(sm_auth_token_autoload("localhost", 6001), 1);
    ASSERT_STR_EQ(getenv("SMOLMUX_AUTH_TOKEN"), "second");

    /* Broker gone, file removed: the loaded token is dropped. */
    unlink(path);
    ASSERT_INT_EQ(sm_auth_token_autoload("127.0.0.1", 6001), 0);
    ASSERT(getenv("SMOLMUX_AUTH_TOKEN") == NULL, "stale token dropped");

    /* A token the user exported is never replaced. */
    ASSERT_INT_EQ(sm_auth_token_write(path, "fromfile"), 0);
    setenv("SMOLMUX_AUTH_TOKEN", "mine", 1);
    ASSERT_INT_EQ(sm_auth_token_autoload("127.0.0.1", 6001), 0);
    ASSERT_STR_EQ(getenv("SMOLMUX_AUTH_TOKEN"), "mine");
    unsetenv("SMOLMUX_AUTH_TOKEN");

    /* Remote host: the local file is not for it. */
    ASSERT_INT_EQ(sm_auth_token_autoload("192.168.1.10", 6001), 0);
    ASSERT(getenv("SMOLMUX_AUTH_TOKEN") == NULL, "remote: no load");

    /* A file with the wrong mode is rejected loudly, not used. */
    chmod(path, 0644);
    ASSERT_INT_EQ(sm_auth_token_autoload("127.0.0.1", 6001), -1);
    ASSERT(getenv("SMOLMUX_AUTH_TOKEN") == NULL, "rejected file not used");

    unlink(path);
    rmdir(g_dir);
}

/* --- end to end: real smolmux binary --- */

static int free_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET,
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t al = sizeof(a);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        getsockname(fd, (struct sockaddr *)&a, &al) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return ntohs(a.sin_port);
}

/* errfd >= 0 receives the broker's stderr; no_xdg starts it without
 * XDG_RUNTIME_DIR (cron, a system unit), so its token goes to /tmp. */
static pid_t start_broker_env(const char *bin, const char *dev, int port,
                              int insecure, int no_xdg, int errfd)
{
    char sock[400], portstr[16];
    snprintf(sock, sizeof(sock), "%s/b.sock", g_dir);
    snprintf(portstr, sizeof(portstr), "%d", port);
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        dup2(devnull, STDOUT_FILENO);
        dup2(errfd >= 0 ? errfd : devnull, STDERR_FILENO);
        unsetenv("SMOLMUX_AUTH_TOKEN");
        if (no_xdg)
            unsetenv("XDG_RUNTIME_DIR");
        if (insecure)
            execl(bin, "smolmux", dev, "-s", sock, "--no-io-log",
                  "--tcp-port", portstr, "--insecure-no-auth", (char *)NULL);
        else
            execl(bin, "smolmux", dev, "-s", sock, "--no-io-log",
                  "--tcp-port", portstr, (char *)NULL);
        _exit(127);
    }
    return pid;
}

static pid_t start_broker(const char *bin, const char *dev, int port,
                          int insecure)
{
    return start_broker_env(bin, dev, port, insecure, 0, -1);
}

static int connect_port(int port)
{
    for (int i = 0; i < 60; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a = { .sin_family = AF_INET,
                                 .sin_port = htons((uint16_t)port),
                                 .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
        if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0)
            return fd;
        close(fd);
        usleep(50000);
    }
    return -1;
}

/* Send a hello (token from SMOLMUX_AUTH_TOKEN, as every client does) and
 * return the reply's "type", or "" if the broker closed without one. */
static void hello_reply(int port, char *type, size_t len)
{
    type[0] = '\0';
    int fd = connect_port(port);
    if (fd < 0)
        return;
    size_t n;
    char *line = sm_msg_encode(sm_msg_hello("tok-test", "observer"), &n);
    if (line) {
        (void)!write(fd, line, n);
        free(line);
    }
    char buf[4096];
    size_t got = 0;
    struct pollfd p = { .fd = fd, .events = POLLIN };
    while (got < sizeof(buf) - 1 && poll(&p, 1, 2000) > 0) {
        ssize_t r = read(fd, buf + got, sizeof(buf) - 1 - got);
        if (r <= 0)
            break;
        got += (size_t)r;
        buf[got] = '\0';
        char *nl = strchr(buf, '\n');
        if (nl) {
            *nl = '\0';
            sm_msg_t m = sm_msg_decode(buf, (size_t)(nl - buf));
            const char *t = m.root ? sm_json_get_string(m.root, "type") : NULL;
            snprintf(type, len, "%s", t ? t : "");
            sm_msg_free(&m);
            break;
        }
    }
    close(fd);
}

static int wait_for_file(const char *path, int present)
{
    for (int i = 0; i < 60; i++) {
        if ((access(path, F_OK) == 0) == present)
            return 1;
        usleep(50000);
    }
    return 0;
}

static void test_broker_generates_and_enforces_token(void)
{
    char bin[4096];
    const char *sm = find_bin(bin, sizeof(bin), "smolmux");
    ASSERT_NOT_NULL(sm);
    make_dir();
    int m, s;
    ASSERT_INT_EQ(openpty(&m, &s, NULL, NULL, NULL), 0);
    int port = free_port();
    ASSERT(port > 0, "free port");

    pid_t pid = start_broker(sm, ttyname(s), port, 0);
    ASSERT(pid > 0, "fork");
    char path[512];
    sm_auth_token_path(path, sizeof(path), "tcp", port);
    ASSERT(wait_for_file(path, 1), "token file appears");

    struct stat st;
    ASSERT_INT_EQ(stat(path, &st), 0);
    ASSERT_INT_EQ((int)(st.st_mode & 0777), 0600);
    ASSERT_INT_EQ((int)st.st_uid, (int)geteuid());

    char type[64];
    unsetenv("SMOLMUX_AUTH_TOKEN");
    hello_reply(port, type, sizeof(type));
    ASSERT_STR_EQ(type, "error");   /* "authentication failed" */

    ASSERT_INT_EQ(sm_auth_token_autoload("127.0.0.1", port), 1);
    const char *tok = getenv("SMOLMUX_AUTH_TOKEN");
    ASSERT(tok && strlen(tok) == SM_AUTH_TOKEN_HEX_LEN, "48-hex token");
    hello_reply(port, type, sizeof(type));
    ASSERT_STR_EQ(type, "welcome");

    /* smolmux-cli token prints it for the far end of a tunnel. */
    char clibuf[4096];
    const char *cli = find_bin(clibuf, sizeof(clibuf), "smolmux-cli");
    ASSERT_NOT_NULL(cli);
    char cmd[4600], want[128], out[512] = {0};
    snprintf(cmd, sizeof(cmd), "'%s' token", cli);
    FILE *pf = popen(cmd, "r");
    ASSERT_NOT_NULL(pf);
    (void)!fread(out, 1, sizeof(out) - 1, pf);
    ASSERT_INT_EQ(WEXITSTATUS(pclose(pf)), 0);
    snprintf(want, sizeof(want), "tcp %d %s\n", port, tok);
    ASSERT(strstr(out, want) != NULL, "token command prints kind port token");
    unsetenv("SMOLMUX_AUTH_TOKEN");

    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    ASSERT(wait_for_file(path, 0), "token file removed on exit");

    close(m);
    close(s);
    rmdir(g_dir);
}

static void test_insecure_no_auth_keeps_tokenless(void)
{
    char bin[4096];
    const char *sm = find_bin(bin, sizeof(bin), "smolmux");
    ASSERT_NOT_NULL(sm);
    make_dir();
    int m, s;
    ASSERT_INT_EQ(openpty(&m, &s, NULL, NULL, NULL), 0);
    int port = free_port();
    ASSERT(port > 0, "free port");

    pid_t pid = start_broker(sm, ttyname(s), port, 1);
    ASSERT(pid > 0, "fork");
    char type[64];
    unsetenv("SMOLMUX_AUTH_TOKEN");
    hello_reply(port, type, sizeof(type));
    ASSERT_STR_EQ(type, "welcome");
    char path[512];
    sm_auth_token_path(path, sizeof(path), "tcp", port);
    ASSERT(access(path, F_OK) != 0, "no token file with --insecure-no-auth");

    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    close(m);
    close(s);
    rmdir(g_dir);
}

/* Readers look in XDG_RUNTIME_DIR, then /tmp; XDG_RUNTIME_DIR wins. */
static void test_find_falls_back_to_tmp(void)
{
    make_dir();                       /* XDG_RUNTIME_DIR: empty private dir */
    int port = 40000 + (int)(getpid() % 20000);
    char tmp_path[128], xdg_path[512], found[512];
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/smolmux-tcp-%d.token", port);
    unlink(tmp_path);
    ASSERT_INT_EQ(sm_auth_token_write(tmp_path, "fromtmp"), 0);

    ASSERT_INT_EQ(sm_auth_token_find(found, sizeof(found), "tcp", port), 0);
    ASSERT_STR_EQ(found, tmp_path);
    unsetenv("SMOLMUX_AUTH_TOKEN");
    ASSERT_INT_EQ(sm_auth_token_autoload("127.0.0.1", port), 1);
    ASSERT_STR_EQ(getenv("SMOLMUX_AUTH_TOKEN"), "fromtmp");

    ASSERT_INT_EQ(sm_auth_token_path(xdg_path, sizeof(xdg_path), "tcp", port), 0);
    ASSERT_INT_EQ(sm_auth_token_write(xdg_path, "fromxdg"), 0);
    ASSERT_INT_EQ(sm_auth_token_autoload("127.0.0.1", port), 1);
    ASSERT_STR_EQ(getenv("SMOLMUX_AUTH_TOKEN"), "fromxdg");

    unsetenv("SMOLMUX_AUTH_TOKEN");
    unlink(xdg_path);
    unlink(tmp_path);
    rmdir(g_dir);
}

/* A broker started without XDG_RUNTIME_DIR writes to /tmp; a client with
 * XDG_RUNTIME_DIR set still finds the token and gets in. */
static void test_broker_without_xdg_client_with_xdg(void)
{
    char bin[4096];
    const char *sm = find_bin(bin, sizeof(bin), "smolmux");
    ASSERT_NOT_NULL(sm);
    make_dir();
    int m, s;
    ASSERT_INT_EQ(openpty(&m, &s, NULL, NULL, NULL), 0);
    int port = free_port();
    ASSERT(port > 0, "free port");

    pid_t pid = start_broker_env(sm, ttyname(s), port, 0, 1, -1);
    ASSERT(pid > 0, "fork");
    char tmp_path[128], xdg_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/smolmux-tcp-%d.token", port);
    sm_auth_token_path(xdg_path, sizeof(xdg_path), "tcp", port);
    ASSERT(wait_for_file(tmp_path, 1), "token file in /tmp");
    ASSERT(access(xdg_path, F_OK) != 0, "nothing in the client's XDG dir");

    char type[64];
    unsetenv("SMOLMUX_AUTH_TOKEN");
    ASSERT_INT_EQ(sm_auth_token_autoload("127.0.0.1", port), 1);
    const char *tok = getenv("SMOLMUX_AUTH_TOKEN");
    ASSERT(tok && strlen(tok) == SM_AUTH_TOKEN_HEX_LEN, "token from /tmp");
    hello_reply(port, type, sizeof(type));
    ASSERT_STR_EQ(type, "welcome");

    char clibuf[4096];
    const char *cli = find_bin(clibuf, sizeof(clibuf), "smolmux-cli");
    ASSERT_NOT_NULL(cli);
    char cmd[4600], want[128], out[1024] = {0};
    snprintf(cmd, sizeof(cmd), "'%s' token", cli);
    FILE *pf = popen(cmd, "r");
    ASSERT_NOT_NULL(pf);
    (void)!fread(out, 1, sizeof(out) - 1, pf);
    pclose(pf);
    snprintf(want, sizeof(want), "tcp %d %s\n", port, tok);
    ASSERT(strstr(out, want) != NULL, "token command finds the /tmp file");
    unsetenv("SMOLMUX_AUTH_TOKEN");

    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    ASSERT(wait_for_file(tmp_path, 0), "/tmp token file removed on exit");
    close(m);
    close(s);
    rmdir(g_dir);
}

/* Someone else holds the /tmp name (here: a symlink). The broker refuses
 * to start, says why, and does not write through the link. */
static void test_squatted_tmp_name_refuses_with_hint(void)
{
    char bin[4096];
    const char *sm = find_bin(bin, sizeof(bin), "smolmux");
    ASSERT_NOT_NULL(sm);
    make_dir();
    int m, s;
    ASSERT_INT_EQ(openpty(&m, &s, NULL, NULL, NULL), 0);
    int port = free_port();
    ASSERT(port > 0, "free port");

    char tmp_path[128], target[300];
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/smolmux-tcp-%d.token", port);
    snprintf(target, sizeof(target), "%s/victim", g_dir);
    int tfd = open(target, O_WRONLY | O_CREAT, 0600);
    ASSERT(tfd >= 0, "target");
    close(tfd);
    unlink(tmp_path);
    ASSERT_INT_EQ(symlink(target, tmp_path), 0);

    int errp[2];
    ASSERT_INT_EQ(pipe(errp), 0);
    pid_t pid = start_broker_env(sm, ttyname(s), port, 0, 1, errp[1]);
    close(errp[1]);
    int st = 0, exited = 0;
    for (int i = 0; i < 60 && !exited; i++) {
        if (waitpid(pid, &st, WNOHANG) == pid)
            exited = 1;
        else
            usleep(50000);
    }
    if (!exited) {
        kill(pid, SIGTERM);
        waitpid(pid, &st, 0);
    }
    char err[2048] = {0};
    (void)!read(errp[0], err, sizeof(err) - 1);
    close(errp[0]);

    ASSERT(exited && WIFEXITED(st) && WEXITSTATUS(st) == 1,
           "broker refuses to start");
    ASSERT(strstr(err, "cannot write auth token file") != NULL, "says what");
    ASSERT(strstr(err, "XDG_RUNTIME_DIR") != NULL, "says how to fix it");
    struct stat sb;
    ASSERT_INT_EQ(stat(target, &sb), 0);
    ASSERT_INT_EQ((int)sb.st_size, 0);

    unlink(tmp_path);
    unlink(target);
    close(m);
    close(s);
    rmdir(g_dir);
}

int main(void)
{
    printf("test_auth_token\n");
    signal(SIGPIPE, SIG_IGN);
    RUN_TEST(test_generate_is_hex_and_random);
    RUN_TEST(test_write_is_0600_and_refuses_symlink);
    RUN_TEST(test_host_is_loopback);
    RUN_TEST(test_autoload_rules);
    RUN_TEST(test_broker_generates_and_enforces_token);
    RUN_TEST(test_insecure_no_auth_keeps_tokenless);
    RUN_TEST(test_find_falls_back_to_tmp);
    RUN_TEST(test_broker_without_xdg_client_with_xdg);
    RUN_TEST(test_squatted_tmp_name_refuses_with_hint);
    TEST_REPORT();
}
