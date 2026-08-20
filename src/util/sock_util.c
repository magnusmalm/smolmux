#include "util/sock_util.h"
#include "constants.h"
#include "logger.h"
#include "util/timeutil.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <glob.h>
#include <limits.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/netlink.h>
#include <linux/sock_diag.h>
#include <linux/unix_diag.h>

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

int sm_autodiscover_should_refuse(int explicit_pin)
{
    if (explicit_pin)
        return 0;
    char socks[32][SM_SOCK_PATH_MAX];
    return sm_discover_all_sockets(socks, 32) > 1;
}

int sm_client_name_is_mcp(const char *name)
{
    if (!name || !name[0])
        return 0;
    size_t n = strlen(name);
    if (n >= 4 && strcmp(name + n - 4, "-mcp") == 0)
        return 1;
    return strcmp(name, "claude-mcp") == 0;
}

int sm_gc_should_kill(const char *name, int pid_alive, int ppid,
                      int kill_all_mcp)
{
    if (!sm_client_name_is_mcp(name))
        return 0;
    if (kill_all_mcp)
        return 1;
    if (!pid_alive)
        return 1;
    if (ppid == 1)
        return 1;
    return 0;
}

static int comm_is_mcp(int pid)
{
    char path[64], comm[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    if (!fgets(comm, sizeof(comm), f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    size_t n = strlen(comm);
    if (n && comm[n - 1] == '\n')
        comm[n - 1] = '\0';
    return sm_client_name_is_mcp(comm);
}

/* stat(sock).st_ino is the filesystem inode of the socket *file*.
 * /proc/PID/fd/N for an AF_UNIX fd is socket:[sockfs_ino]. Those
 * numbers are not the same. SOCK_DIAG reports sockfs inodes and the
 * peer of each connected socket; /proc/net/unix names only the listen
 * and accept() sides, not the client. */

#define SM_UNIX_DIAG_NAMED_MAX 128
#define SM_UNIX_DIAG_INO_MAX   256

static int nla_ok(const struct nlattr *a, unsigned rem)
{
    return rem >= sizeof(*a) && a->nla_len >= sizeof(*a) &&
           a->nla_len <= rem;
}

static const struct nlattr *nla_next(const struct nlattr *a, unsigned *rem)
{
    unsigned tot = NLA_ALIGN(a->nla_len);
    *rem -= tot;
    return (const struct nlattr *)((const char *)a + tot);
}

int sm_unix_sock_name_matches(const char *name, const char *sock_path)
{
    if (!name || !name[0] || !sock_path || !sock_path[0])
        return 0;
    if (strcmp(name, sock_path) == 0)
        return 1;
    size_t n = strlen(sock_path);
    if (strncmp(name, sock_path, n) != 0 || name[n] != '.')
        return 0;
    const char *p = name + n + 1;
    if (!isdigit((unsigned char)*p))
        return 0;
    while (isdigit((unsigned char)*p))
        p++;
    return strcmp(p, ".tmp") == 0;
}

static int add_ino(unsigned long *v, int *n, int max, unsigned long ino)
{
    if (ino == 0)
        return 0;
    for (int i = 0; i < *n; i++) {
        if (v[i] == ino)
            return 0;
    }
    if (*n >= max)
        return -1;
    v[(*n)++] = ino;
    return 0;
}

static int ino_in(const unsigned long *v, int n, unsigned long ino)
{
    if (ino == 0)
        return 0;
    for (int i = 0; i < n; i++) {
        if (v[i] == ino)
            return 1;
    }
    return 0;
}

static int unix_diag_open(void)
{
    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_SOCK_DIAG);
    if (fd < 0)
        return -1;
    struct sockaddr_nl local;
    memset(&local, 0, sizeof(local));
    local.nl_family = AF_NETLINK;
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
        close(fd);
        return -1;
    }
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int unix_diag_send_dump(int fd)
{
    struct {
        struct nlmsghdr nlh;
        struct unix_diag_req req;
    } req;
    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = (uint32_t)sizeof(req);
    req.nlh.nlmsg_type = SOCK_DIAG_BY_FAMILY;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.req.sdiag_family = AF_UNIX;
    req.req.udiag_states = ~0u;
    req.req.udiag_show = UDIAG_SHOW_NAME | UDIAG_SHOW_PEER;
    if (send(fd, &req, sizeof(req), 0) != (ssize_t)sizeof(req))
        return -1;
    return 0;
}

typedef int (*unix_diag_fn)(unsigned long ino, unsigned long peer,
                            const char *name, void *user);

static int unix_diag_foreach(int fd, unix_diag_fn fn, void *user)
{
    if (unix_diag_send_dump(fd) != 0)
        return -1;
    char buf[8192];
    for (;;) {
        ssize_t nr = recv(fd, buf, sizeof(buf), 0);
        if (nr <= 0)
            return -1;
        int rem = (int)nr;
        struct nlmsghdr *h = (struct nlmsghdr *)buf;
        for (; NLMSG_OK(h, rem); h = NLMSG_NEXT(h, rem)) {
            if (h->nlmsg_type == NLMSG_DONE)
                return 0;
            if (h->nlmsg_type == NLMSG_ERROR)
                return -1;
            if (h->nlmsg_len < NLMSG_LENGTH(sizeof(struct unix_diag_msg)))
                return -1;
            struct unix_diag_msg *m = NLMSG_DATA(h);
            unsigned alen = h->nlmsg_len - NLMSG_LENGTH(sizeof(*m));
            const struct nlattr *a =
                (const struct nlattr *)((const char *)m +
                                        NLMSG_ALIGN(sizeof(*m)));
            char name[SM_SOCK_PATH_MAX];
            name[0] = '\0';
            unsigned int peer = 0;
            while (nla_ok(a, alen)) {
                int typ = a->nla_type & NLA_TYPE_MASK;
                int plen = (int)a->nla_len - NLA_HDRLEN;
                const char *pl = (const char *)a + NLA_HDRLEN;
                if (typ == UNIX_DIAG_NAME && plen > 0) {
                    if (plen > (int)sizeof(name) - 1)
                        plen = (int)sizeof(name) - 1;
                    memcpy(name, pl, (size_t)plen);
                    name[plen] = '\0';
                } else if (typ == UNIX_DIAG_PEER && plen >= 4) {
                    memcpy(&peer, pl, 4);
                }
                a = nla_next(a, &alen);
            }
            if (fn((unsigned long)m->udiag_ino, (unsigned long)peer,
                   name, user) != 0)
                return -1;
        }
    }
}

typedef struct {
    const char *path;
    unsigned long named[SM_UNIX_DIAG_NAMED_MAX];
    int nn;
    unsigned long *out;
    int *nout;
    int max;
    int pass;
} unix_diag_collect_t;

static int unix_diag_collect_one(unsigned long ino, unsigned long peer,
                                 const char *name, void *user)
{
    unix_diag_collect_t *c = user;
    if (c->pass == 1) {
        if (sm_unix_sock_name_matches(name, c->path)) {
            add_ino(c->named, &c->nn, SM_UNIX_DIAG_NAMED_MAX, ino);
            add_ino(c->out, c->nout, c->max, ino);
            add_ino(c->out, c->nout, c->max, peer);
        }
    } else if (ino_in(c->named, c->nn, peer)) {
        add_ino(c->out, c->nout, c->max, ino);
    }
    return 0;
}

static int unix_diag_inodes_for_path(const char *sock_path,
                                     unsigned long *out, int max)
{
    int fd = unix_diag_open();
    if (fd < 0)
        return -1;
    unix_diag_collect_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    int nout = 0;
    ctx.path = sock_path;
    ctx.out = out;
    ctx.nout = &nout;
    ctx.max = max;
    ctx.pass = 1;
    if (unix_diag_foreach(fd, unix_diag_collect_one, &ctx) != 0) {
        close(fd);
        return -1;
    }
    ctx.pass = 2;
    if (unix_diag_foreach(fd, unix_diag_collect_one, &ctx) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return nout;
}

static int pid_holds_any_inode(int pid, const unsigned long *inos, int ninos)
{
    if (ninos <= 0)
        return 0;
    char dir[64];
    snprintf(dir, sizeof(dir), "/proc/%d/fd", pid);
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    int hit = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        char linkpath[384], dest[64];
        snprintf(linkpath, sizeof(linkpath), "%s/%s", dir, de->d_name);
        ssize_t n = readlink(linkpath, dest, sizeof(dest) - 1);
        if (n <= 0)
            continue;
        dest[n] = '\0';
        unsigned long got;
        if (sscanf(dest, "socket:[%lu]", &got) != 1)
            continue;
        if (ino_in(inos, ninos, got)) {
            hit = 1;
            break;
        }
    }
    closedir(d);
    return hit;
}

int sm_unix_mcp_peer_pids(const char *sock_path, int skip_pid,
                          int *out, int max)
{
    if (!sock_path || !sock_path[0] || !out || max <= 0)
        return 0;
    struct stat st;
    if (stat(sock_path, &st) != 0)
        return 0;
    unsigned long inos[SM_UNIX_DIAG_INO_MAX];
    int ninos = unix_diag_inodes_for_path(sock_path, inos, SM_UNIX_DIAG_INO_MAX);
    if (ninos < 0)
        return -1;
    if (ninos == 0)
        return 0;

    DIR *proc = opendir("/proc");
    if (!proc)
        return -1;
    int n = 0;
    int self = (int)getpid();
    struct dirent *de;
    while ((de = readdir(proc)) != NULL && n < max) {
        if (!isdigit((unsigned char)de->d_name[0]))
            continue;
        int pid = atoi(de->d_name);
        if (pid <= 0 || pid == skip_pid || pid == self)
            continue;
        if (!comm_is_mcp(pid))
            continue;
        if (pid_holds_any_inode(pid, inos, ninos))
            out[n++] = pid;
    }
    closedir(proc);
    return n;
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

    /* Refuse a broker owned by someone else.
     *
     * Sockets normally live in $XDG_RUNTIME_DIR, but that is unset under sudo,
     * cron, system services and containers, and everything then falls back to
     * world-writable /tmp — which client discovery globs unconditionally, and
     * glob() returns sorted so a planted /tmp/smolmux-0.sock sorts first. A
     * hostile local user could therefore be handed our commands (including
     * anything typed at a console) and feed fabricated device output back,
     * which for an AI agent reading that output is its own problem.
     *
     * The broker's own socket is 0700, so a same-UID check costs nothing in
     * the legitimate case and removes the impersonation primitive. */
    struct ucred cred;
    socklen_t clen = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &clen) == 0 &&
        clen == sizeof(cred) && cred.uid != geteuid()) {
        SM_LOG_WARN("sock", "refusing broker socket %s: owned by uid %u, not %u",
                    path, (unsigned)cred.uid, (unsigned)geteuid());
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

static void read_sysfs_str(const char *path, char *out, size_t out_len)
{
    out[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(out, (int)out_len, f)) {
        size_t n = strlen(out);
        while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
            out[--n] = '\0';
    }
    fclose(f);
}

/* Walk /sys/class/tty/<name>/device up a few levels looking for idVendor. */
static void fill_usb_ids(const char *devnode, sm_serial_port_info_t *info)
{
    const char *base = strrchr(devnode, '/');
    base = base ? base + 1 : devnode;
    char path[512];
    snprintf(path, sizeof(path), "/sys/class/tty/%s/device", base);

    char resolved[512];
    if (!realpath(path, resolved))
        return;

    char walk[512];
    snprintf(walk, sizeof(walk), "%s", resolved);
    for (int depth = 0; depth < 8; depth++) {
        char vpath[600], ppath[600], mpath[600], rpath[600];
        snprintf(vpath, sizeof(vpath), "%s/idVendor", walk);
        snprintf(ppath, sizeof(ppath), "%s/idProduct", walk);
        snprintf(mpath, sizeof(mpath), "%s/manufacturer", walk);
        snprintf(rpath, sizeof(rpath), "%s/product", walk);
        if (access(vpath, R_OK) == 0) {
            read_sysfs_str(vpath, info->vid, sizeof(info->vid));
            read_sysfs_str(ppath, info->pid, sizeof(info->pid));
            read_sysfs_str(mpath, info->manufacturer, sizeof(info->manufacturer));
            read_sysfs_str(rpath, info->product, sizeof(info->product));
            return;
        }
        /* parent */
        char *slash = strrchr(walk, '/');
        if (!slash || slash == walk)
            break;
        *slash = '\0';
    }
}

static void fill_by_id(const char *devnode, sm_serial_port_info_t *info)
{
    char real_dev[512];
    if (!realpath(devnode, real_dev))
        snprintf(real_dev, sizeof(real_dev), "%s", devnode);

    glob_t g;
    memset(&g, 0, sizeof(g));
    if (glob("/dev/serial/by-id/*", 0, NULL, &g) != 0)
        return;
    for (size_t i = 0; i < g.gl_pathc; i++) {
        char target[512];
        if (!realpath(g.gl_pathv[i], target))
            continue;
        if (strcmp(target, real_dev) == 0) {
            snprintf(info->by_id, sizeof(info->by_id), "%s", g.gl_pathv[i]);
            break;
        }
    }
    globfree(&g);
}

size_t sm_list_serial_ports_info(sm_serial_port_info_t *out, size_t max)
{
    if (!out || max == 0)
        return 0;
    glob_t g;
    size_t n = sm_glob_serial_ports(&g);
    size_t written = 0;
    for (size_t i = 0; i < n && written < max; i++) {
        sm_serial_port_info_t *info = &out[written];
        memset(info, 0, sizeof(*info));
        snprintf(info->path, sizeof(info->path), "%s", g.gl_pathv[i]);
        fill_by_id(info->path, info);
        fill_usb_ids(info->path, info);
        (void)sm_serial_resolve_by_path(info->path, info->by_path,
                                        sizeof(info->by_path));
        written++;
    }
    globfree(&g);
    return written;
}

int sm_serial_by_id_is_weak(const char *path_or_by_id)
{
    if (!path_or_by_id || !path_or_by_id[0])
        return 0;
    /* Only classify by-id paths (or bare basenames that look like them). */
    const char *base = strrchr(path_or_by_id, '/');
    base = base ? base + 1 : path_or_by_id;
    if (!strstr(path_or_by_id, "by-id") && strncmp(base, "usb-", 4) != 0)
        return 0;
    /* Strong if basename contains a 6+ hex run (typical USB serial).
     * Class-only names like usb-1a86_USB_Serial-if00-port0 have only short
     * VID/PID hex (4 digits). */
    int run = 0;
    for (const char *p = base; *p; p++) {
        if (isxdigit((unsigned char)*p)) {
            if (++run >= 6)
                return 0;  /* strong */
        } else {
            run = 0;
        }
    }
    return 1;  /* weak */
}

int sm_serial_resolve_by_id(const char *dev_or_by_path, char *out, size_t out_len)
{
    if (!dev_or_by_path || !out || out_len == 0)
        return -1;
    out[0] = '\0';
    sm_serial_port_info_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    fill_by_id(dev_or_by_path, &tmp);
    if (!tmp.by_id[0])
        return -1;
    snprintf(out, out_len, "%s", tmp.by_id);
    return 0;
}

int sm_serial_resolve_by_path(const char *dev_or_by_id, char *out, size_t out_len)
{
    if (!dev_or_by_id || !out || out_len == 0)
        return -1;
    out[0] = '\0';
    char real_dev[512];
    if (!realpath(dev_or_by_id, real_dev))
        snprintf(real_dev, sizeof(real_dev), "%s", dev_or_by_id);

    glob_t g;
    memset(&g, 0, sizeof(g));
    if (glob("/dev/serial/by-path/*", 0, NULL, &g) != 0)
        return -1;
    int found = -1;
    for (size_t i = 0; i < g.gl_pathc; i++) {
        char target[512];
        if (!realpath(g.gl_pathv[i], target))
            continue;
        if (strcmp(target, real_dev) == 0) {
            snprintf(out, out_len, "%s", g.gl_pathv[i]);
            found = 0;
            break;
        }
    }
    globfree(&g);
    return found;
}

int sm_identity_refuse_weak_seat_change(int weak, const char *last_by_path,
                                        const char *now_by_path)
{
    if (!weak)
        return 0;
    /* Fail closed: unknown last or now seat is not a silent rebind. */
    if (!last_by_path || !last_by_path[0])
        return 1;
    if (!now_by_path || !now_by_path[0])
        return 1;
    return strcmp(last_by_path, now_by_path) != 0;
}

int sm_identity_named_board_is_ambiguous(int has_board, int weak,
                                         const char *policy,
                                         const char *pinned_by_path,
                                         const char *now_by_path,
                                         int identity_ok)
{
    if (!has_board || identity_ok || !weak)
        return 0;
    if (policy && strcmp(policy, "seat") == 0 &&
        pinned_by_path && pinned_by_path[0] &&
        now_by_path && now_by_path[0] &&
        strcmp(pinned_by_path, now_by_path) == 0)
        return 0;
    return 1;
}

int sm_wait_path_exists(const char *path, double timeout_s, int poll_us)
{
    if (!path || !path[0])
        return -1;
    if (timeout_s < 0)
        timeout_s = 0;
    if (poll_us < 1000)
        poll_us = 1000;

    double deadline = sm_now_monotonic() + timeout_s;
    while (access(path, F_OK) != 0) {
        if (sm_now_monotonic() >= deadline)
            return -1;
        usleep((useconds_t)poll_us);
    }
    return 0;
}

char *sm_format_serial_ports_text(void)
{
    sm_serial_port_info_t infos[SM_SERIAL_PORT_INFO_MAX];
    size_t n = sm_list_serial_ports_info(infos, SM_SERIAL_PORT_INFO_MAX);
    if (n == 0)
        return strdup("No serial ports found.");

    size_t cap = n * 480 + 64;
    char *buf = malloc(cap);
    if (!buf)
        return strdup("(allocation failed)");
    size_t off = 0;
    for (size_t i = 0; i < n; i++) {
        sm_serial_port_info_t *p = &infos[i];
        int k = snprintf(buf + off, cap - off, "%s", p->path);
        if (k < 0 || (size_t)k >= cap - off)
            break;
        off += (size_t)k;
        if (p->by_id[0]) {
            const char *tag = sm_serial_by_id_is_weak(p->by_id)
                                  ? " [WEAK: class-only by-id, no USB serial]"
                                  : " [STRONG]";
            k = snprintf(buf + off, cap - off, "\n  by-id: %s%s", p->by_id,
                         tag);
            if (k > 0 && (size_t)k < cap - off) off += (size_t)k;
        }
        if (p->by_path[0]) {
            k = snprintf(buf + off, cap - off, "\n  by-path: %s", p->by_path);
            if (k > 0 && (size_t)k < cap - off) off += (size_t)k;
        }
        if (p->vid[0] || p->pid[0]) {
            k = snprintf(buf + off, cap - off, "\n  usb: %s:%s",
                         p->vid[0] ? p->vid : "????",
                         p->pid[0] ? p->pid : "????");
            if (k > 0 && (size_t)k < cap - off) off += (size_t)k;
        }
        if (p->manufacturer[0] || p->product[0]) {
            k = snprintf(buf + off, cap - off, "\n  %s %s",
                         p->manufacturer, p->product);
            if (k > 0 && (size_t)k < cap - off) off += (size_t)k;
        }
        if (i + 1 < n) {
            if (off + 1 < cap)
                buf[off++] = '\n';
        }
    }
    buf[off < cap ? off : cap - 1] = '\0';
    return buf;
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
