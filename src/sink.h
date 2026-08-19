#ifndef SM_SINK_H
#define SM_SINK_H

#include <stddef.h>
#include <stdint.h>

typedef struct sm_sink {
    const char *name;
    int  (*start)(struct sm_sink *self, void *broker);
    void (*stop)(struct sm_sink *self);
    void (*on_output)(struct sm_sink *self, const uint8_t *data, size_t len, double ts);
    void (*on_event)(struct sm_sink *self, const char *event, const char *payload);
    void (*on_readable)(struct sm_sink *self);
    void (*on_expect_result)(struct sm_sink *self, const char *id,
                             int matched, const uint8_t *data, size_t data_len,
                             const char *pattern);
    void (*destroy)(struct sm_sink *self);
    void *data;
    int fd; /* fd to watch in epoll, -1 = none */
} sm_sink_t;

#endif /* SM_SINK_H */
