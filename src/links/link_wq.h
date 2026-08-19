#ifndef SM_LINK_WQ_H
#define SM_LINK_WQ_H

#include <stddef.h>
#include <stdint.h>

typedef struct sm_link_wq_entry {
    uint8_t *data;
    size_t len;
    size_t offset;
} sm_link_wq_entry_t;

typedef struct sm_link_wq {
    sm_link_wq_entry_t *entries;
    size_t head;
    size_t count;
    size_t cap;
    size_t total_bytes;
} sm_link_wq_t;

void sm_link_wq_init(sm_link_wq_t *wq);
void sm_link_wq_clear(sm_link_wq_t *wq);

/* Append bytes to the deferred write queue. Returns 0 on success, -1 on OOM
 * or queue full. */
int sm_link_wq_enqueue(sm_link_wq_t *wq, const uint8_t *data, size_t len);

int sm_link_wq_has_pending(const sm_link_wq_t *wq);

/* Try to write queued bytes to fd (non-blocking). Returns 0 when the queue
 * is drained, 1 when more data remains (EAGAIN), -1 on hard error. */
int sm_link_wq_flush(int fd, sm_link_wq_t *wq);

#endif /* SM_LINK_WQ_H */