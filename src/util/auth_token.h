#ifndef SM_AUTH_TOKEN_H
#define SM_AUTH_TOKEN_H

#include <stddef.h>

/* Auto-generated auth tokens for loopback TCP/WS listeners (ACT-029).
 *
 * A broker that serves TCP or WebSocket on loopback without an explicit
 * token generates one and writes it, mode 0600, to
 * $XDG_RUNTIME_DIR/smolmux-<kind>-<port>.token (kind "tcp" or "ws"; /tmp
 * when XDG_RUNTIME_DIR is unset). Clients of the same user connecting to a
 * loopback port read it from there, so other local users and processes
 * without access to the file cannot drive the device. */

#define SM_AUTH_TOKEN_HEX_LEN 48   /* 192 bits; fits broker auth_token[64] */

/* Directory the broker writes token files to: $XDG_RUNTIME_DIR, else /tmp. */
const char *sm_auth_token_dir(void);

/* Token file path for a listener. 0, or -1 if out_len is too small. */
int sm_auth_token_path(char *out, size_t out_len, const char *kind, int port);

/* Directories a token file may be in, most specific first: the broker's
 * write dir (above), then /tmp when that differs. A broker started without
 * XDG_RUNTIME_DIR (cron, a system unit) writes to /tmp while a login
 * session's client has XDG_RUNTIME_DIR set, so readers check both.
 * Fills dirs, returns the count (1 or 2). */
int sm_auth_token_dirs(const char *dirs[2]);

/* Path of the first existing token file for kind/port in those dirs.
 * 0 if found (out set), -1 if none. */
int sm_auth_token_find(char *out, size_t out_len, const char *kind, int port);

/* Random token: SM_AUTH_TOKEN_HEX_LEN hex chars. out_len must be at least
 * SM_AUTH_TOKEN_HEX_LEN + 1. 0 or -1. */
int sm_auth_token_generate(char *out, size_t out_len);

/* Write token to path, mode 0600. Refuses symlinks and a file that exists
 * but is not a regular file owned by this user with one link. 0 or -1
 * (errno set). */
int sm_auth_token_write(const char *path, const char *token);

/* 1 for "localhost", "::1", and 127.0.0.0/8 literals. */
int sm_host_is_loopback(const char *host);

/* Client side: if SMOLMUX_AUTH_TOKEN is unset and host is loopback, load
 * the tcp token file for port into SMOLMUX_AUTH_TOKEN (the hello reads it
 * from there). Call before every connect: a token loaded here is re-read
 * (a restarted broker has a new one); one the user exported is kept.
 * Returns 1 if loaded, 0 if not applicable (user-set env,
 * remote host, or no token file), -1 if a token file exists but was
 * rejected (the reason is printed). */
int sm_auth_token_autoload(const char *host, int port);

#endif /* SM_AUTH_TOKEN_H */
