#include "expect.h"
#include "constants.h"
#include "util/timeutil.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

void sm_expect_init(sm_expect_engine_t *eng)
{
    memset(eng, 0, sizeof(*eng));
}

void sm_expect_destroy(sm_expect_engine_t *eng)
{
    for (size_t i = 0; i < eng->count; i++) {
        sm_regex_free(eng->requests[i].compiled);
        free(eng->requests[i].buffer);
    }
    free(eng->requests);
    memset(eng, 0, sizeof(*eng));
}

int sm_expect_add(sm_expect_engine_t *eng, const char *id,
                  const char *pattern, double timeout_s, const char *client_id)
{
    if (eng->count >= SM_MAX_EXPECT_PENDING) return -1;

    /* Enforce per-client limit to prevent resource exhaustion */
    size_t client_count = 0;
    for (size_t i = 0; i < eng->count; i++) {
        if (strcmp(eng->requests[i].client_id, client_id) == 0)
            client_count++;
    }
    if (client_count >= SM_MAX_EXPECT_PER_CLIENT) return -1;

    if (eng->count >= eng->capacity) {
        size_t new_cap = eng->capacity ? eng->capacity * 2 : 16;
        if (new_cap > SM_MAX_EXPECT_PENDING) return -1;
        void *tmp = realloc(eng->requests, new_cap * sizeof(sm_expect_request_t));
        if (!tmp) return -1;
        eng->requests = tmp;
        eng->capacity = new_cap;
    }

    sm_expect_request_t *req = &eng->requests[eng->count];
    memset(req, 0, sizeof(*req));

    snprintf(req->id, sizeof(req->id), "%s", id);
    snprintf(req->pattern_str, sizeof(req->pattern_str), "%s", pattern);
    snprintf(req->client_id, sizeof(req->client_id), "%s", client_id);

    req->compiled = sm_regex_compile(pattern, NULL, 0);
    if (!req->compiled) return -1;

    req->deadline = sm_now_monotonic() + timeout_s;
    req->buf_cap = 4096;
    req->buffer = malloc(req->buf_cap);
    if (!req->buffer) {
        sm_regex_free(req->compiled);
        return -1;
    }
    req->buf_len = 0;
    req->matched = 0;
    req->search_offset = 0;

    eng->count++;
    return 0;
}

void sm_expect_feed(sm_expect_engine_t *eng, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < eng->count; i++) {
        sm_expect_request_t *req = &eng->requests[i];
        if (req->matched) continue;

        /* Skip if buffer would exceed cap */
        if (req->buf_len + len > SM_MAX_EXPECT_BUF_SIZE) continue;

        /* Grow buffer if needed — the >= keeps one spare byte for the NUL */
        int oom = 0;
        while (req->buf_len + len >= req->buf_cap) {
            size_t new_cap = req->buf_cap * 2;
            void *tmp = realloc(req->buffer, new_cap);
            if (!tmp) { oom = 1; break; }
            req->buffer = tmp;
            req->buf_cap = new_cap;
        }
        if (oom) continue;
        memcpy(req->buffer + req->buf_len, data, len);
        req->buf_len += len;
        req->buffer[req->buf_len] = '\0';

        /* Try match — scan from near where new data starts to avoid O(N*M).
         * Trade-off: a match whose start lies more than the lookback before
         * the new data is missed. */
        size_t search_start = 0;
        if (req->search_offset > SM_EXPECT_SCAN_LOOKBACK)
            search_start = req->search_offset - SM_EXPECT_SCAN_LOOKBACK;
        size_t search_len = req->buf_len - search_start;
        if (sm_regex_exec(req->compiled,
                          (const char *)req->buffer + search_start,
                          search_len, NULL) == 0) {
            req->matched = 1;
        }
        req->search_offset = req->buf_len;
    }
}

size_t sm_expect_collect(sm_expect_engine_t *eng, double now,
                         sm_expect_result_t *out, size_t max_out)
{
    size_t collected = 0;

    for (size_t i = 0; i < eng->count && collected < max_out; ) {
        sm_expect_request_t *req = &eng->requests[i];
        int done = req->matched || (now >= req->deadline);

        if (!done) {
            i++;
            continue;
        }

        /* Fill result */
        sm_expect_result_t *r = &out[collected++];
        snprintf(r->id, sizeof(r->id), "%s", req->id);
        r->matched = req->matched;
        r->data = req->buffer;
        r->data_len = req->buf_len;
        snprintf(r->pattern, sizeof(r->pattern), "%s", req->pattern_str);
        snprintf(r->client_id, sizeof(r->client_id), "%s", req->client_id);

        /* Don't free buffer — transferred to result */
        sm_regex_free(req->compiled);

        /* Remove from list */
        eng->count--;
        if (i < eng->count)
            eng->requests[i] = eng->requests[eng->count];
        /* Don't increment i — swapped element needs checking */
    }

    return collected;
}

void sm_expect_cancel_id(sm_expect_engine_t *eng, const char *id)
{
    for (size_t i = 0; i < eng->count; ) {
        if (strcmp(eng->requests[i].id, id) == 0) {
            sm_regex_free(eng->requests[i].compiled);
            free(eng->requests[i].buffer);
            eng->count--;
            if (i < eng->count)
                eng->requests[i] = eng->requests[eng->count];
        } else {
            i++;
        }
    }
}

void sm_expect_cancel_client(sm_expect_engine_t *eng, const char *client_id)
{
    for (size_t i = 0; i < eng->count; ) {
        if (strcmp(eng->requests[i].client_id, client_id) == 0) {
            sm_regex_free(eng->requests[i].compiled);
            free(eng->requests[i].buffer);
            eng->count--;
            if (i < eng->count)
                eng->requests[i] = eng->requests[eng->count];
        } else {
            i++;
        }
    }
}

void sm_expect_cancel_all(sm_expect_engine_t *eng)
{
    for (size_t i = 0; i < eng->count; i++) {
        sm_regex_free(eng->requests[i].compiled);
        free(eng->requests[i].buffer);
    }
    eng->count = 0;
}
