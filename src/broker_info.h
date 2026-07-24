#ifndef SM_BROKER_INFO_H
#define SM_BROKER_INFO_H

#include <stddef.h>
#include "cJSON.h"
#include "util/sock_util.h"

/* A one-shot summary of what a broker holds, obtained by connecting and asking
 * for status. Reusable by smolmux-monitor (discovery listing), smolmux-cli, and
 * agent tooling. */
typedef struct sm_broker_info {
    char socket[SM_SOCK_PATH_MAX];
    int  reachable;          /* 1 if connect + status_response succeeded */
    char board[64];          /* board grouping label (--board), "" if unset */
    char role[32];           /* this wire's role on the board (--role) */
    char link_type[16];      /* "uart", "gdb", or "?" */
    char endpoint[160];      /* UART: device path; GDB: target spec */
    int  baud;               /* UART baud, 0 if not applicable */
    int  connected;          /* link is connected to the device/target */
    int  suspended;
    int  client_count;
    int  pid;                /* broker pid (via SO_PEERCRED), -1 if unknown */
} sm_broker_info_t;

/* Probe a broker at socket_path: connect as an observer, request status, and
 * fill *out. Returns 0 if reachable (out fully populated), -1 if not
 * (out->reachable = 0, out->socket still set). timeout_ms bounds the probe. */
int sm_broker_probe(const char *socket_path, sm_broker_info_t *out,
                    int timeout_ms);

/* Stop the broker behind socket_path: probe for its pid (SO_PEERCRED), send
 * SIGTERM, and wait until the broker's clean teardown unlinks the socket.
 * Same-user only by construction (kill(2) permission model) — deliberately
 * NOT a wire-protocol message, so it is never reachable via the TCP sink.
 * Returns 0 when the socket is gone (broker exited). On failure returns -1
 * with a human-readable reason in errbuf: unreachable/stale socket, missing
 * socket, unknown pid, kill error, or timeout. pid_out (optional) receives
 * the broker pid once known. timeout_ms <= 0 means a 5 s default. */
int sm_broker_stop_by_socket(const char *socket_path, int timeout_ms,
                             int *pid_out, char *errbuf, size_t errlen);

/* Find a running broker whose endpoint is the given device path (UART links).
 * Both sides are realpath-normalized so /dev/serial/by-id/... and the
 * /dev/ttyACM* it points at compare equal. Returns 0 with *out filled when a
 * reachable broker holds the port, -1 when none does. timeout_ms bounds each
 * per-broker probe. */
int sm_find_broker_for_endpoint(const char *port, sm_broker_info_t *out,
                                int timeout_ms);

/* Format a one-line human summary (no trailing newline) into buf. */
void sm_broker_info_format(const sm_broker_info_t *info, char *buf, size_t len);

/* Serialize one broker summary to a cJSON object (caller owns it). Unreachable
 * brokers carry only "socket" and "reachable":false. */
cJSON *sm_broker_info_to_json(const sm_broker_info_t *info);

/* Discover and probe every active broker into out[] (up to max entries).
 * Returns the total number of sockets found — which may exceed max, in which
 * case only the first max are probed and written. timeout_ms bounds each probe.
 * The reusable primitive behind `smolmux-cli brokers` and `smolmux-monitor -L`. */
size_t sm_broker_discover(sm_broker_info_t *out, size_t max, int timeout_ms);

#endif /* SM_BROKER_INFO_H */
