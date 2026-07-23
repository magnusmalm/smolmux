#include "ring_buffer.h"
#include "constants.h"

#include <stdlib.h>
#include <string.h>

#define RB_IDX(rb, i) (((rb)->head + (i)) % (rb)->capacity)

typedef struct sm_rb_pool_slot {
    uint8_t *data;
    size_t cap;
} sm_rb_pool_slot_t;

static sm_rb_pool_slot_t rb_pool[SM_RB_POOL_SLOTS];
static size_t rb_pool_count;

static void rb_pool_release(uint8_t *data, size_t cap)
{
    if (!data) return;
    if (rb_pool_count < SM_RB_POOL_SLOTS) {
        rb_pool[rb_pool_count].data = data;
        rb_pool[rb_pool_count].cap = cap;
        rb_pool_count++;
        return;
    }
    free(data);
}

static uint8_t *rb_pool_acquire(size_t need)
{
    size_t want = need > SM_RB_CHUNK_TARGET ? need : SM_RB_CHUNK_TARGET;
    for (size_t i = 0; i < rb_pool_count; i++) {
        if (rb_pool[i].cap >= want) {
            uint8_t *buf = rb_pool[i].data;
            size_t cap = rb_pool[i].cap;
            rb_pool[i] = rb_pool[--rb_pool_count];
            if (cap < want) {
                uint8_t *grown = realloc(buf, want);
                if (!grown) {
                    rb_pool_release(buf, cap);
                    return NULL;
                }
                buf = grown;
                cap = want;
            }
            return buf;
        }
    }
    return malloc(want);
}

void sm_rb_init(sm_ring_buffer_t *rb, size_t max_bytes)
{
    memset(rb, 0, sizeof(*rb));
    rb->max_bytes = max_bytes;
}

void sm_rb_destroy(sm_ring_buffer_t *rb)
{
    for (size_t i = 0; i < rb->count; i++) {
        sm_rb_chunk_t *c = &rb->chunks[RB_IDX(rb, i)];
        rb_pool_release(c->data, c->alloc);
        c->data = NULL;
        c->alloc = 0;
    }
    free(rb->chunks);
    memset(rb, 0, sizeof(*rb));
}

static void evict(sm_ring_buffer_t *rb)
{
    while (rb->count > 0 && rb->total_bytes > rb->max_bytes) {
        sm_rb_chunk_t *front = &rb->chunks[rb->head];
        size_t excess = rb->total_bytes - rb->max_bytes;
        if (front->len <= excess) {
            rb->total_bytes -= front->len;
            rb_pool_release(front->data, front->alloc);
            front->data = NULL;
            front->alloc = 0;
            front->len = 0;
            rb->head = (rb->head + 1) % rb->capacity;
            rb->count--;
        } else {
            memmove(front->data, front->data + excess, front->len - excess);
            front->len -= excess;
            rb->total_bytes -= excess;
        }
    }
}

void sm_rb_append(sm_ring_buffer_t *rb, const uint8_t *data, size_t len, double ts)
{
    if (len == 0) return;

    /* Coalesce into the last chunk when it shares the timestamp and has room. */
    if (rb->count > 0) {
        sm_rb_chunk_t *last = &rb->chunks[(rb->head + rb->count - 1) % rb->capacity];
        if (last->timestamp == ts && last->len < SM_RB_CHUNK_TARGET &&
            last->len + len <= last->alloc) {
            memcpy(last->data + last->len, data, len);
            last->len += len;
            rb->total_bytes += len;
            evict(rb);
            return;
        }
        if (last->timestamp == ts && last->len < SM_RB_CHUNK_TARGET &&
            last->len + len <= SM_RB_CHUNK_TARGET) {
            size_t new_cap = last->len + len;
            uint8_t *grown = realloc(last->data, new_cap);
            if (grown) {
                last->data = grown;
                last->alloc = new_cap;
                memcpy(last->data + last->len, data, len);
                last->len += len;
                rb->total_bytes += len;
                evict(rb);
                return;
            }
        }
    }

    if (rb->count >= rb->capacity) {
        size_t new_cap = rb->capacity ? rb->capacity * 2 : 64;
        sm_rb_chunk_t *new_chunks = calloc(new_cap, sizeof(sm_rb_chunk_t));
        if (!new_chunks) return;
        for (size_t i = 0; i < rb->count; i++)
            new_chunks[i] = rb->chunks[RB_IDX(rb, i)];
        free(rb->chunks);
        rb->chunks = new_chunks;
        rb->capacity = new_cap;
        rb->head = 0;
    }

    size_t slot = (rb->head + rb->count) % rb->capacity;
    sm_rb_chunk_t *c = &rb->chunks[slot];
    size_t alloc = len > SM_RB_CHUNK_TARGET ? len : SM_RB_CHUNK_TARGET;
    c->data = rb_pool_acquire(alloc);
    if (!c->data) return;
    c->alloc = alloc;
    c->timestamp = ts;
    memcpy(c->data, data, len);
    c->len = len;
    rb->count++;
    rb->total_bytes += len;

    evict(rb);
}

/* Copy a range of logical indices [start, start+n) into a new array */
static size_t copy_range(sm_ring_buffer_t *rb, size_t start, size_t n,
                          sm_rb_chunk_t **out_chunks)
{
    if (n == 0) {
        *out_chunks = NULL;
        return 0;
    }
    sm_rb_chunk_t *result = malloc(n * sizeof(sm_rb_chunk_t));
    if (!result) {
        *out_chunks = NULL;
        return 0;
    }
    for (size_t i = 0; i < n; i++)
        result[i] = rb->chunks[RB_IDX(rb, start + i)];
    *out_chunks = result;
    return n;
}

size_t sm_rb_get_since(sm_ring_buffer_t *rb, double since_ts,
                       sm_rb_chunk_t **out_chunks)
{
    for (size_t i = 0; i < rb->count; i++) {
        if (rb->chunks[RB_IDX(rb, i)].timestamp >= since_ts)
            return copy_range(rb, i, rb->count - i, out_chunks);
    }
    *out_chunks = NULL;
    return 0;
}

size_t sm_rb_get_last_n_bytes(sm_ring_buffer_t *rb, size_t n,
                              sm_rb_chunk_t **out_chunks)
{
    if (rb->count == 0 || n == 0) {
        *out_chunks = NULL;
        return 0;
    }

    size_t acc = 0;
    size_t start = rb->count;
    while (start > 0 && acc < n) {
        start--;
        acc += rb->chunks[RB_IDX(rb, start)].len;
    }

    return copy_range(rb, start, rb->count - start, out_chunks);
}

size_t sm_rb_get_all(sm_ring_buffer_t *rb, sm_rb_chunk_t **out_chunks)
{
    return copy_range(rb, 0, rb->count, out_chunks);
}