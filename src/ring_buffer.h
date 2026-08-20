#ifndef SM_RING_BUFFER_H
#define SM_RING_BUFFER_H

#include <stddef.h>
#include <stdint.h>

typedef struct sm_rb_chunk {
    double timestamp;
    uint64_t seq_start;  /* global byte offset of data[0] (monotonic) */
    uint8_t *data;
    size_t len;
    size_t alloc;  /* allocated capacity (pool reuse) */
} sm_rb_chunk_t;

typedef struct sm_ring_buffer {
    sm_rb_chunk_t *chunks;
    size_t head;
    size_t count;
    size_t capacity;
    size_t total_bytes;
    size_t max_bytes;
    uint64_t total_seq;  /* next seq to assign (= bytes ever appended) */
    uint64_t first_seq;  /* seq of oldest retained byte */
} sm_ring_buffer_t;

void sm_rb_init(sm_ring_buffer_t *rb, size_t max_bytes);
void sm_rb_destroy(sm_ring_buffer_t *rb);
void sm_rb_append(sm_ring_buffer_t *rb, const uint8_t *data, size_t len, double ts);

/* Query functions. Returns chunk count and sets *out_chunks to a malloc'd
   array the caller must free() — but NOT the data pointers within, which
   alias the ring buffer's internal storage (I1). Those pointers are only
   valid until the next sm_rb_append(); copy the bytes out before appending
   if you need them to outlive the call. Single-threaded use assumed. */
size_t sm_rb_get_since(sm_ring_buffer_t *rb, double since_ts,
                       sm_rb_chunk_t **out_chunks);
size_t sm_rb_get_last_n_bytes(sm_ring_buffer_t *rb, size_t n,
                              sm_rb_chunk_t **out_chunks);
size_t sm_rb_get_all(sm_ring_buffer_t *rb, sm_rb_chunk_t **out_chunks);

/* Cursor-based read (Wave 3 / P1b). since_seq is the next byte the caller
 * wants; dropped counts bytes lost to eviction before first_seq.
 * *out_first_skip is bytes to skip in chunks[0] to reach since_seq.
 * *out_cursor is the seq just past the last returned byte.
 * *out_has_more is 1 if more complete data remains after this page.
 * max_bytes 0 means no extra cap (caller may still cap). */
size_t sm_rb_get_since_seq(sm_ring_buffer_t *rb, uint64_t since_seq,
                           size_t max_bytes, sm_rb_chunk_t **out_chunks,
                           size_t *out_first_skip, uint64_t *out_cursor,
                           uint64_t *out_dropped, int *out_has_more);

uint64_t sm_rb_total_seq(const sm_ring_buffer_t *rb);
uint64_t sm_rb_first_seq(const sm_ring_buffer_t *rb);

#endif /* SM_RING_BUFFER_H */
