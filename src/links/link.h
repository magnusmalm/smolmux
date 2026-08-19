#ifndef SM_LINK_H
#define SM_LINK_H

#include "cJSON.h"
#include <stddef.h>
#include <stdint.h>

typedef struct sm_link {
    const char *name;
    int  (*open)(struct sm_link *self);
    void (*close)(struct sm_link *self);
    int  (*read_fd)(struct sm_link *self);
    int  (*write_fd)(struct sm_link *self);
    int  (*write_data)(struct sm_link *self, const uint8_t *data, size_t len);
    int  (*has_write_pending)(struct sm_link *self);
    int  (*flush_write_queue)(struct sm_link *self);
    int  (*send_break)(struct sm_link *self, int duration_ms);
    int  (*set_param)(struct sm_link *self, const char *key, const char *value);
    int  (*get_status)(struct sm_link *self, cJSON *out);
    void (*destroy)(struct sm_link *self);
    /* Optional non-blocking connect for the reconnect path, so a slow or
     * unreachable server never stalls the broker's event loop (open() stays
     * blocking and is used at startup/resume, where blocking is harmless).
     * connect_begin initiates the connect and leaves read_fd() valid: returns
     * 0 = connected, 1 = in progress (watch the fd for writable, then call
     * connect_poll), -1 = failed. connect_poll returns 1 = connected,
     * 0 = still pending, -1 = failed. Links without these fall back to open()
     * on reconnect. */
    int  (*connect_begin)(struct sm_link *self);
    int  (*connect_poll)(struct sm_link *self);
    /* Optional: transform bytes just read from the link before the broker
     * processes them (history, expect, anomaly, broadcast) — e.g. strip telnet
     * IAC negotiation from a serial-over-TCP stream. Writes at most in_len
     * bytes to `out` (filtering only removes bytes) and returns the count. The
     * link may write control replies back to its own fd from within. NULL (the
     * default for calloc'd links) means the broker uses the raw bytes. */
    size_t (*filter_rx)(struct sm_link *self, const uint8_t *in, size_t in_len,
                        uint8_t *out);
    /* Nonzero when long silence is normal for this link (e.g. GDB MI: a
     * running or halted target produces no output). The broker skips the
     * idle-based "link_health degraded" broadcasts for such links. */
    int silence_normal;
    void *data;
} sm_link_t;

#endif /* SM_LINK_H */
