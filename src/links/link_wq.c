#include "links/link_wq.h"
#include "constants.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

void sm_link_wq_init(sm_link_wq_t *wq)
{
    memset(wq, 0, sizeof(*wq));
}

void sm_link_wq_clear(sm_link_wq_t *wq)
{
    for (size_t i = 0; i < wq->count; i++) {
        size_t idx = (wq->head + i) % wq->cap;
        free(wq->entries[idx].data);
        wq->entries[idx].data = NULL;
    }
    free(wq->entries);
    memset(wq, 0, sizeof(*wq));
}

int sm_link_wq_has_pending(const sm_link_wq_t *wq)
{
    return wq->count > 0;
}

int sm_link_wq_enqueue(sm_link_wq_t *wq, const uint8_t *data, size_t len)
{
    if (len == 0) return 0;
    if (wq->total_bytes + len > SM_LINK_WRITE_QUEUE_MAX_BYTES)
        return -1;

    if (wq->count >= wq->cap) {
        size_t new_cap = wq->cap ? wq->cap * 2 : 16;
        sm_link_wq_entry_t *ne = calloc(new_cap, sizeof(*ne));
        if (!ne) return -1;
        for (size_t i = 0; i < wq->count; i++)
            ne[i] = wq->entries[(wq->head + i) % wq->cap];
        free(wq->entries);
        wq->entries = ne;
        wq->cap = new_cap;
        wq->head = 0;
    }

    uint8_t *copy = malloc(len);
    if (!copy) return -1;
    memcpy(copy, data, len);

    size_t slot = (wq->head + wq->count) % wq->cap;
    wq->entries[slot].data = copy;
    wq->entries[slot].len = len;
    wq->entries[slot].offset = 0;
    wq->count++;
    wq->total_bytes += len;
    return 0;
}

int sm_link_wq_flush(int fd, sm_link_wq_t *wq)
{
    while (wq->count > 0) {
        sm_link_wq_entry_t *e = &wq->entries[wq->head];
        size_t remain = e->len - e->offset;
        ssize_t n = write(fd, e->data + e->offset, remain);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 1;
            return -1;
        }
        if (n == 0)
            return -1;

        e->offset += (size_t)n;
        wq->total_bytes -= (size_t)n;
        if (e->offset >= e->len) {
            free(e->data);
            e->data = NULL;
            wq->head = (wq->head + 1) % wq->cap;
            wq->count--;
        }
    }
    return 0;
}