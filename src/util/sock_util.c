#include "util/sock_util.h"
#include "constants.h"

#include <errno.h>
#include <glob.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* FNV-1a 32-bit — stable short id for long device basenames (by-id). */
static uint32_t fnv1a32(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 16777619u;
    }
    return h;
}

static const char *sock_runtime_dir(void)
{
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir && runtime_dir[0])
        return runtime_dir;
    return "/tmp";
}

/* baselen of path without directory (handles trailing slashes loosely). */
static void path_basename(const char *path, char *base, size_t base_len)
{
    if (!path || !path[0]) {
        snprintf(base, base_len, "unknown");
        return;
    }
    const char *slash = strrchr(path, '/');
    const char *b = slash ? slash + 1 : path;
    if (!b[0])
        b = "unknown";
    snprintf(base, base_len, "%s", b);
}

/* Build dir/smolmux-<tag>.sock, shortening <tag> so strlen(path) <= max_len. */
static int format_smolmux_sock(char *out, size_t out_len, const char *dir,
                               const char *tag, size_t max_strlen)
{
    if (!out || out_len == 0 || !dir || !dir[0] || !tag || !tag[0])
        return -1;

    /* Prefer the readable form when it fits. */
    int n = snprintf(out, out_len, "%s/smolmux-%s.sock", dir, tag);
    if (n < 0)
        return -1;
    if ((size_t)n < out_len && (size_t)n <= max_strlen)
        return 0;

    /* Short stable form: smolmux-<prefix>-<8hex>.sock
     * Shrink prefix until the full path fits max_strlen and out_len. */
    uint32_t h = fnv1a32(tag);
    char hex[9];
    snprintf(hex, sizeof(hex), "%08x", (unsigned)h);

    /* Fixed parts: dir + "/smolmux-" + "-" + hex + ".sock" */
    size_t fixed = strlen(dir) + strlen("/smolmux-") + 1 + 8 + strlen(".sock");
    if (fixed >= out_len || fixed > max_strlen)
        return -1;

    size_t prefix_max = max_strlen - fixed;
    if (prefix_max + fixed >= out_len)
        prefix_max = out_len - 1 - fixed;
    if (prefix_max < 1)
        return -1;

    char prefix[128];
    size_t tlen = strlen(tag);
    size_t plen = tlen < prefix_max ? tlen : prefix_max;
    /* Avoid cutting mid-path weirdness: prefer alnum tail of prefix. */
    memcpy(prefix, tag, plen);
    prefix[plen] = '\0';

    n = snprintf(out, out_len, "%s/smolmux-%s-%s.sock", dir, prefix, hex);
    if (n < 0 || (size_t)n >= out_len || (size_t)n > max_strlen)
        return -1;
    return 0;
}

int sm_derive_socket_path(char *out, size_t out_len, const char *device_or_label)
{
    if (!device_or_label || !device_or_label[0])
        return -1;

    char base[256];
    path_basename(device_or_label, base, sizeof(base));
    return format_smolmux_sock(out, out_len, sock_runtime_dir(), base,
                               SM_SOCK_FINAL_MAX);
}

int sm_derive_board_socket_path(char *out, size_t out_len,
                                const char *board, const char *role)
{
    if (!board || !board[0] || !role || !role[0])
        return -1;

    char tag[256];
    snprintf(tag, sizeof(tag), "%s-%s", board, role);
    return format_smolmux_sock(out, out_len, sock_runtime_dir(), tag,
                               SM_SOCK_FINAL_MAX);
}

int sm_discover_socket(char *out, size_t out_len)
{
    /* 1. Environment variable */
    const char *env = getenv(SM_SOCKET_ENV);
    if (env && env[0]) {
        snprintf(out, out_len, "%s", env);
        return 0;
    }

    /* 2. Check XDG_RUNTIME_DIR first (secure location) */
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir && runtime_dir[0]) {
        char xdg_pattern[256];
        snprintf(xdg_pattern, sizeof(xdg_pattern), SM_SOCKET_GLOB_XDG_FMT, runtime_dir);
        glob_t g;
        memset(&g, 0, sizeof(g));
        if (glob(xdg_pattern, 0, NULL, &g) == 0 && g.gl_pathc > 0) {
            snprintf(out, out_len, "%s", g.gl_pathv[0]);
            globfree(&g);
            return 0;
        }
        globfree(&g);
    }

    /* 3. Fall back to /tmp glob */
    glob_t g;
    memset(&g, 0, sizeof(g));
    if (glob(SM_SOCKET_GLOB, 0, NULL, &g) == 0 && g.gl_pathc > 0) {
        snprintf(out, out_len, "%s", g.gl_pathv[0]);
        globfree(&g);
        return 0;
    }
    globfree(&g);

    return -1;
}

/* Append path to out[] if not already present and room remains. Always bumps
 * *count (so the caller learns the true total even past max). */
static void add_unique(char (*out)[SM_SOCK_PATH_MAX], size_t max, size_t *count,
                       const char *path)
{
    for (size_t i = 0; i < *count && i < max; i++)
        if (strcmp(out[i], path) == 0)
            return;
    if (*count < max)
        snprintf(out[*count], SM_SOCK_PATH_MAX, "%s", path);
    (*count)++;
}

static void add_glob(const char *pattern, char (*out)[SM_SOCK_PATH_MAX],
                     size_t max, size_t *count)
{
    glob_t g;
    memset(&g, 0, sizeof(g));
    if (glob(pattern, 0, NULL, &g) == 0)
        for (size_t i = 0; i < g.gl_pathc; i++)
            add_unique(out, max, count, g.gl_pathv[i]);
    globfree(&g);
}

size_t sm_discover_all_sockets(char (*out)[SM_SOCK_PATH_MAX], size_t max)
{
    size_t count = 0;

    const char *env = getenv(SM_SOCKET_ENV);
    if (env && env[0])
        add_unique(out, max, &count, env);

    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir && runtime_dir[0]) {
        char pattern[256];
        snprintf(pattern, sizeof(pattern), SM_SOCKET_GLOB_XDG_FMT, runtime_dir);
        add_glob(pattern, out, max, &count);
    }

    add_glob(SM_SOCKET_GLOB, out, max, &count);
    return count;
}

int sm_connect_unix(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

int sm_connect_tcp(const char *host, int port)
{
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    int rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0)
        return -1;

    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype | SOCK_CLOEXEC, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

int sm_write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, p + written, len - written);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)  /* no progress — avoid spinning forever */
            return -1;
        written += (size_t)n;
    }
    return 0;
}

size_t sm_glob_serial_ports(glob_t *g)
{
    memset(g, 0, sizeof(*g));
    glob("/dev/ttyUSB*", 0, NULL, g);
    glob("/dev/ttyACM*", GLOB_APPEND, NULL, g);
    return g->gl_pathc;
}

void sm_parse_host_port(const char *spec, char *host, size_t host_len, int *port)
{
    const char *colon = strrchr(spec, ':');
    if (colon && colon != spec) {
        size_t hlen = (size_t)(colon - spec);
        if (hlen >= host_len) hlen = host_len - 1;
        memcpy(host, spec, hlen);
        host[hlen] = '\0';
        *port = atoi(colon + 1);
    } else {
        snprintf(host, host_len, "%s", spec);
    }
}
