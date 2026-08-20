#ifndef SM_SOCK_UTIL_H
#define SM_SOCK_UTIL_H

#include <stddef.h>
#include <glob.h>

/* Glob the serial-port device nodes (/dev/ttyUSB*, /dev/ttyACM*) into a single
 * *g (caller must globfree it). Returns the number of matches (g->gl_pathc). */
size_t sm_glob_serial_ports(glob_t *g);

/* Enriched port listing (Linux best-effort: by-id + USB VID/PID from sysfs). */
#define SM_SERIAL_PORT_INFO_MAX 64

typedef struct sm_serial_port_info {
    char path[128];
    char by_id[256];         /* /dev/serial/by-id/... or empty */
    char by_path[256];       /* /dev/serial/by-path/... or empty */
    char vid[8];             /* hex or empty */
    char pid[8];
    char manufacturer[64];
    char product[64];
} sm_serial_port_info_t;

/* Fill up to max entries; returns count written. */
size_t sm_list_serial_ports_info(sm_serial_port_info_t *out, size_t max);

/* Human-readable multi-line listing (malloc'd). */
char *sm_format_serial_ports_text(void);

/* 1 if path is a /dev/serial/by-id/... node whose basename has no long
 * USB serial run (class-only CH340-style names). 0 otherwise. */
int sm_serial_by_id_is_weak(const char *path_or_by_id);

/* Resolve matching /dev/serial/by-path/… for a device node or by-id path.
 * Returns 0 and fills out on success, -1 if none. */
int sm_serial_resolve_by_path(const char *dev_or_by_id, char *out, size_t out_len);

/* Resolve matching /dev/serial/by-id/… for a device node or by-path.
 * Returns 0 and fills out on success, -1 if none. */
int sm_serial_resolve_by_id(const char *dev_or_by_path, char *out, size_t out_len);

/* 1 = refuse reconnect. Fail-closed for weak by-id: empty last seat,
 * empty now seat, or last != now. Strong / non-weak always allow. */
int sm_identity_refuse_weak_seat_change(int weak, const char *last_by_path,
                                        const char *now_by_path);

/* 1 = do not auto-bind a named board (weak by-id without a confirmed seat).
 * identity_ok overrides. policy "seat" + matching pinned/now by-path allows. */
int sm_identity_named_board_is_ambiguous(int has_board, int weak,
                                         const char *policy,
                                         const char *pinned_by_path,
                                         const char *now_by_path,
                                         int identity_ok);

/* Poll until path exists. timeout_s=0 is a single check. poll_us < 1000
 * is raised to 1000. Returns 0 if present, -1 on timeout/bad args. */
int sm_wait_path_exists(const char *path, double timeout_s, int poll_us);

/* Discover a smolmux broker socket path via env var or glob.
 * Returns 0 on success, -1 if not found. */
int sm_discover_socket(char *out, size_t out_len);

/* Path buffer size for socket paths (sockaddr_un.sun_path is 108 on Linux). */
#define SM_SOCK_PATH_MAX 108

/* Broker binds via rename of "<socket>.<pid>.tmp"; that temp name must also fit
 * sun_path. Reserve enough for ".%d.tmp" with a large pid (see sock_util.c). */
#define SM_SOCK_BIND_TMP_EXTRA 16

/* Max strlen of a final broker socket path so temp bind always fits. */
#define SM_SOCK_FINAL_MAX (SM_SOCK_PATH_MAX - 1 - SM_SOCK_BIND_TMP_EXTRA)

/* Derive a Unix socket path for a device node or label (e.g. /dev/ttyUSB0 or
 * a serial by-id basename). Uses $XDG_RUNTIME_DIR when set, else /tmp.
 * Long labels (USB by-id strings) are shortened to a stable
 * smolmux-<prefix>-<8hex>.sock form so AF_UNIX bind always succeeds.
 * Returns 0, or -1 if out_len is too small or device_or_label is empty. */
int sm_derive_socket_path(char *out, size_t out_len, const char *device_or_label);

/* Same layout as sm_derive_socket_path, but the tag is "board-role"
 * (board manifests). */
int sm_derive_board_socket_path(char *out, size_t out_len,
                                const char *board, const char *role);

/* Enumerate all active broker sockets: $SMOLMUX_SOCKET (if set),
 * $XDG_RUNTIME_DIR/smolmux-*.sock, and /tmp/smolmux-*.sock, de-duplicated.
 * Fills up to max entries into out[][SM_SOCK_PATH_MAX]; returns the count found
 * (which may exceed max — only the first max are written). */
size_t sm_discover_all_sockets(char (*out)[SM_SOCK_PATH_MAX], size_t max);

/* 1 = first-glob would be a guess (several sockets, no explicit pin).
 * explicit_pin is -s / positional / SMOLMUX_SOCKET. */
int sm_autodiscover_should_refuse(int explicit_pin);

/* 1 if this hello name is an MCP stdio server (claude-mcp, *-mcp). */
int sm_client_name_is_mcp(const char *name);

/* 1 = SIGTERM this client. pid_alive=0 means /proc is gone. ppid<0 unknown. */
int sm_gc_should_kill(const char *name, int pid_alive, int ppid,
                      int kill_all_mcp);

/* 1 if kernel AF_UNIX name is sock_path, or the bind temp
 * sock_path.<pid>.tmp (broker bind-then-rename). */
int sm_unix_sock_name_matches(const char *name, const char *sock_path);

/* Pids of smolmux-mcp / *-mcp processes connected to sock_path.
 * Uses SOCK_DIAG sockfs inodes (not stat of the .sock file). skip_pid
 * is the broker and is omitted. Returns count written, or -1 if the
 * peer table is unavailable. Missing path yields 0. */
int sm_unix_mcp_peer_pids(const char *sock_path, int skip_pid,
                          int *out, int max);

/* Connect to a Unix domain socket. Returns fd on success, -1 on error. */
int sm_connect_unix(const char *path);

/* Connect to a TCP host:port. Returns fd on success, -1 on error. */
int sm_connect_tcp(const char *host, int port);

/* Parse "host:port" (or a bare "host") into host[host_len] and *port. When no
 * ':' is present, *port is left unchanged (keep the caller's default). */
void sm_parse_host_port(const char *spec, char *host, size_t host_len, int *port);

/* Write all bytes to fd, handling partial writes.
 * Returns 0 on success, -1 on error. */
int sm_write_all(int fd, const void *buf, size_t len);

#endif /* SM_SOCK_UTIL_H */
