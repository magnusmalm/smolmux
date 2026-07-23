#ifndef SM_RING_BUFFER_H
#define SM_RING_BUFFER_H

#include <stddef.h>
#include <stdint.h>

typedef struct sm_rb_chunk {
    double timestamp;
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

#endif /* SM_RING_BUFFER_H */
