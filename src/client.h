#ifndef SM_CLIENT_H
#define SM_CLIENT_H

#include "protocol.h"
#include "util/shared_line.h"
#include <stddef.h>
#include <stdint.h>

typedef struct sm_write_entry {
    char *data;
    sm_shared_line_t *shared;  /* if set, release on dequeue instead of free(data) */
    size_t len;
    size_t offset;    /* bytes already written */
} sm_write_entry_t;

typedef struct sm_client {
    int fd;
    char id[32];
    char name[64];
    char role[16];
    int hello_received;
    int disconnected;
    double connected_at;   /* CLOCK_MONOTONIC, set by broker on register */
    int requires_auth;     /* set for network-origin clients (TCP) */

    uint8_t *read_buf;
    size_t read_len;
    size_t read_cap;

    sm_write_entry_t *write_queue;
    size_t wq_head;
    size_t wq_count;
    size_t wq_cap;
    size_t wq_drops;
    int write_pending;
} sm_client_t;

sm_client_t *sm_client_new(int fd, unsigned int client_num);
void sm_client_destroy(sm_client_t *c);

/* Read from fd and extract parsed messages. Returns count of messages. */
size_t sm_client_feed(sm_client_t *c, sm_msg_t *out, size_t max_out);

/* Queue a JSON message for sending. Returns 0 on success. */
int sm_client_send(sm_client_t *c, cJSON *msg);

/* Queue pre-serialized data for sending. Takes ownership of data. Returns 0 on success. */
int sm_client_send_raw(sm_client_t *c, char *data, size_t len);

/* Queue a refcounted shared line (one acquire per queued entry). Returns 0 on success. */
int sm_client_send_shared(sm_client_t *c, sm_shared_line_t *sl);

/* Flush write queue. Returns: 0=done, 1=more pending, -1=error. */
int sm_client_flush(sm_client_t *c);

#endif /* SM_CLIENT_H */
