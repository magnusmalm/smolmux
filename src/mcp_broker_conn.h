#ifndef SM_MCP_BROKER_CONN_H
#define SM_MCP_BROKER_CONN_H

#include <stddef.h>
#include <stdint.h>
#include <signal.h>
#include "cJSON.h"

/* Shared broker-connection core for the standalone MCP binaries. Owns the
 * socket to a smolmux broker, the line-buffered read side, wire-id generation,
 * and the send / wait-for-response / request plumbing. Extracted from
 * mcp_client.c so smolmux-mcp (serial tools) and smolmux-gdb-mcp (GDB tools)
 * share one correct implementation rather than duplicating it.
 *
 * Device output (SM_MSG_OUTPUT) is decoded from base64 and handed to a
 * caller-supplied callback: smolmux-mcp buffers it into a ring for serial_read;
 * smolmux-gdb-mcp feeds it into an MI line assembler. */

typedef void (*sm_broker_output_fn)(void *user, const uint8_t *data, size_t len);

typedef struct sm_broker_conn {
    int fd;               /* broker socket; set by the caller after connecting */
    volatile sig_atomic_t running;  /* 1 while usable; cleared on disconnect,
                                     * error, or from the owner's signal handler */
    int verbose;          /* log unmatched broker messages when set */

    char  *read_buf;      /* line-reassembly buffer (owned) */
    size_t read_len;
    size_t read_cap;

    unsigned int next_seq;  /* wire-id sequence */

    sm_broker_output_fn on_output;  /* called per decoded SM_MSG_OUTPUT payload */
    void *on_output_user;
} sm_broker_conn_t;

/* Allocate the read buffer and set running=1, fd=-1. Returns 0, or -1 on OOM. */
int  sm_broker_conn_init(sm_broker_conn_t *c, size_t read_cap);

/* Close the fd and free the read buffer. Safe on a zeroed/failed conn. */
void sm_broker_conn_destroy(sm_broker_conn_t *c);

void sm_broker_conn_set_output_cb(sm_broker_conn_t *c,
                                  sm_broker_output_fn fn, void *user);

/* Fill buf with a fresh wire id ("mcp-XXXXXXXX"). */
void sm_broker_conn_gen_wire_id(sm_broker_conn_t *c, char *buf, size_t len);

/* Encode and write a wire message. Takes ownership of msg (always deletes it).
 * Returns 0 on success, -1 on error. */
int  sm_broker_conn_send(sm_broker_conn_t *c, cJSON *msg);

/* Read whatever is available on the socket and process complete lines. Each
 * SM_MSG_OUTPUT is decoded and delivered to on_output. If wire_id is non-NULL,
 * returns the first message whose "id" matches (caller cJSON_Deletes it); if
 * wire_id is NULL, returns the first welcome (matched by type — it carries no
 * id). Returns NULL if no matching message completed this read. Clears running
 * on EOF. */
cJSON *sm_broker_conn_read(sm_broker_conn_t *c, const char *wire_id);

/* Poll the socket up to timeout_ms for a message matching wire_id (or a welcome
 * when wire_id is NULL). Returns the message (caller frees) or NULL on
 * timeout/disconnect. */
cJSON *sm_broker_conn_wait(sm_broker_conn_t *c, const char *wire_id,
                           int timeout_ms);

/* send() then wait() for the correlated response. Takes ownership of msg. */
cJSON *sm_broker_conn_request(sm_broker_conn_t *c, cJSON *msg,
                              const char *wire_id, int timeout_ms);

/* Pump the socket once: poll up to timeout_ms and, on readable, process all
 * available lines (delivering SM_MSG_OUTPUT to on_output). Any correlated
 * welcome/id message that surfaces is discarded. Use this to drive your own
 * correlation from the output stream (e.g. matching GDB/MI result tokens).
 * Returns 1 if data was processed, 0 on timeout, -1 on disconnect/error. */
int sm_broker_conn_pump(sm_broker_conn_t *c, int timeout_ms);

#endif /* SM_MCP_BROKER_CONN_H */
