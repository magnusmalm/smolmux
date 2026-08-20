#ifndef SM_EXPECT_H
#define SM_EXPECT_H

#include <stddef.h>
#include <stdint.h>
#include "regex_engine.h"

typedef struct sm_expect_result {
    char id[64];
    int matched;
    int aborted;           /* 1 = ended early (e.g. critical anomaly) */
    uint8_t *data;         /* accumulated data (owned — caller must free) */
    size_t data_len;
    char pattern[256];
    char client_id[32];
    char abort_reason[32]; /* e.g. "anomaly"; empty if not aborted */
    char abort_pattern[64];/* anomaly pattern name when aborted */
} sm_expect_result_t;

typedef struct sm_expect_request {
    char id[64];
    sm_regex_t *compiled;
    char pattern_str[256];
    double deadline;
    uint8_t *buffer;
    size_t buf_len;
    size_t buf_cap;
    size_t search_offset;  /* scan optimization: skip already-scanned data */
    int matched;
    int aborted;
    char client_id[32];
    char abort_reason[32];
    char abort_pattern[64];
} sm_expect_request_t;

typedef struct sm_expect_engine {
    sm_expect_request_t *requests;
    size_t count;
    size_t capacity;
} sm_expect_engine_t;

void sm_expect_init(sm_expect_engine_t *eng);
void sm_expect_destroy(sm_expect_engine_t *eng);

int sm_expect_add(sm_expect_engine_t *eng, const char *id,
                  const char *pattern, double timeout_s, const char *client_id);

void sm_expect_feed(sm_expect_engine_t *eng, const uint8_t *data, size_t len);

size_t sm_expect_collect(sm_expect_engine_t *eng, double now,
                         sm_expect_result_t *out, size_t max_out);

void sm_expect_cancel_client(sm_expect_engine_t *eng, const char *client_id);
void sm_expect_cancel_id(sm_expect_engine_t *eng, const char *id);
void sm_expect_cancel_all(sm_expect_engine_t *eng);

/* Mark every pending expect aborted (matched stays 0). Used when a critical
 * anomaly means the board is not going to produce the expected output.
 * reason is typically "anomaly"; pattern_name is the incident pattern. */
void sm_expect_abort_all(sm_expect_engine_t *eng, const char *reason,
                         const char *pattern_name);

#endif /* SM_EXPECT_H */
