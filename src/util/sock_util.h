#ifndef SM_SOCK_UTIL_H
#define SM_SOCK_UTIL_H

#include <stddef.h>
#include <glob.h>

/* Glob the serial-port device nodes (/dev/ttyUSB*, /dev/ttyACM*) into a single
 * *g (caller must globfree it). Returns the number of matches (g->gl_pathc). */
size_t sm_glob_serial_ports(glob_t *g);

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
