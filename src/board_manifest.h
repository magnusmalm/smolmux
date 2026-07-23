#ifndef SM_BOARD_MANIFEST_H
#define SM_BOARD_MANIFEST_H

#include <stddef.h>
#include "util/sock_util.h"

/* A board manifest (*.board.json) declares a board's wires — one broker each.
 * It doubles as the machine-readable version of the bring-up template's wiring
 * table, and drives `smolmux-cli board up`. Grouping stays decentralized: the
 * manifest only describes how to *start* the wires; the live board view is
 * still derived from discovery + the brokers' own --board labels. */

#define SM_BOARD_MAX_WIRES 16

typedef struct sm_board_wire {
    char role[32];      /* wire role on the board: console, swd, aux, ... */
    char link[8];       /* "uart" or "gdb" */
    char device[128];   /* UART: device path (/dev/ttyUSB0) */
    int  baud;          /* UART: baud (default SM_DEFAULT_BAUD) */
    char gdb_path[128]; /* GDB: gdb binary (default "gdb") */
    char target[128];   /* GDB: target spec (host:port) */
    char profile[256];  /* UART: broker device profile; GDB: gdb-mcp profile (informational) */
    char socket[SM_SOCK_PATH_MAX]; /* explicit socket override; "" => derived */
} sm_board_wire_t;

typedef struct sm_board_manifest {
    char board[64];
    sm_board_wire_t wires[SM_BOARD_MAX_WIRES];
    size_t wire_count;
} sm_board_manifest_t;

/* Parse a manifest from a JSON string / file. Returns 0 on success, -1 on
 * parse error or a missing required field (board name; per-wire role, link,
 * and UART device). */
int sm_board_manifest_from_json(const char *json, sm_board_manifest_t *out);
int sm_board_manifest_load(const char *path, sm_board_manifest_t *out);

/* Resolve a wire's socket path: the explicit override if set, else
 * <dir>/smolmux-<board>-<role>.sock with dir = $XDG_RUNTIME_DIR or /tmp.
 * Returns 0, or -1 if the derived path would exceed the socket length limit. */
int sm_board_wire_socket(const sm_board_manifest_t *m, const sm_board_wire_t *w,
                         char *out, size_t len);

#endif /* SM_BOARD_MANIFEST_H */
