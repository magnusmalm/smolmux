#include "auth_token.h"
#include "sock_util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

const char *sm_auth_token_dir(void)
{
    const char *d = getenv("XDG_RUNTIME_DIR");
    return (d && d[0]) ? d : "/tmp";
}

int sm_auth_token_path(char *out, size_t out_len, const char *kind, int port)
{
    if (!out || !kind || port <= 0)
        return -1;
    int n = snprintf(out, out_len, "%s/smolmux-%s-%d.token", sm_auth_token_dir(),
                     kind, port);
    return (n < 0 || (size_t)n >= out_len) ? -1 : 0;
}

int sm_auth_token_dirs(const char *dirs[2])
{
    int n = 0;
    dirs[n++] = sm_auth_token_dir();
    if (strcmp(dirs[0], "/tmp") != 0)
        dirs[n++] = "/tmp";
    return n;
}

int sm_auth_token_find(char *out, size_t out_len, const char *kind, int port)
{
    const char *dirs[2];
    int nd = sm_auth_token_dirs(dirs);
    for (int i = 0; i < nd; i++) {
        int n = snprintf(out, out_len, "%s/smolmux-%s-%d.token", dirs[i], kind,
                         port);
        struct stat st;
        if (n > 0 && (size_t)n < out_len && lstat(out, &st) == 0)
            return 0;
    }
    return -1;
}

int sm_auth_token_generate(char *out, size_t out_len)
{
    unsigned char raw[SM_AUTH_TOKEN_HEX_LEN / 2];
    if (!out || out_len < SM_AUTH_TOKEN_HEX_LEN + 1)
        return -1;
    size_t got = 0;
    while (got < sizeof(raw)) {
        ssize_t n = getrandom(raw + got, sizeof(raw) - got, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        got += (size_t)n;
    }
    for (size_t i = 0; i < sizeof(raw); i++)
        snprintf(out + 2 * i, 3, "%02x", raw[i]);
    return 0;
}

int sm_auth_token_write(const char *path, const char *token)
{
    if (!path || !token || !token[0])
        return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != geteuid() || st.st_nlink != 1) {
        close(fd);
        errno = EPERM;
        return -1;
    }
    size_t len = strlen(token);
    if (fchmod(fd, 0600) != 0 || ftruncate(fd, 0) != 0 ||
        write(fd, token, len) != (ssize_t)len || write(fd, "\n", 1) != 1) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return close(fd);
}

int sm_host_is_loopback(const char *host)
{
    if (!host || !host[0])
        return 0;
    if (strcmp(host, "localhost") == 0 || strcmp(host, "::1") == 0)
        return 1;
    struct in_addr a;
    if (inet_pton(AF_INET, host, &a) != 1)
        return 0;
    return (ntohl(a.s_addr) >> 24) == 127;
}

/* Set once this process put a token file's content into the environment.
 * A token the user exported is never replaced; one loaded here is re-read
 * on the next connect, because a restarted broker generates a new one. */
static int token_from_file;

int sm_auth_token_autoload(const char *host, int port)
{
    const char *env = getenv("SMOLMUX_AUTH_TOKEN");
    if ((env && env[0] && !token_from_file) || !sm_host_is_loopback(host))
        return 0;

    char path[512];
    if (sm_auth_token_find(path, sizeof(path), "tcp", port) != 0) {
        if (token_from_file) {   /* broker now runs without a token */
            unsetenv("SMOLMUX_AUTH_TOKEN");
            token_from_file = 0;
        }
        return 0;
    }

    char tok[256];
    if (sm_read_owner_secret_file(path, tok, sizeof(tok)) != 0)
        return -1;
    if (setenv("SMOLMUX_AUTH_TOKEN", tok, 1) != 0)
        return -1;
    token_from_file = 1;
    return 1;
}
