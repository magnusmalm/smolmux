#include "client.h"
#include "constants.h"
#include "logger.h"
#include "util/base64.h"
#include "util/json_helpers.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>

static int line_is_output(const char *line, size_t len)
{
    return len >= 15 && memcmp(line, "{\"type\":\"output\"", 15) == 0;
}

/* Merge back-to-back output queue entries to reduce queue pressure. */
static int try_coalesce_output(sm_client_t *c, sm_shared_line_t *sl)
{
    if (!sl || c->wq_count == 0 || !line_is_output(sl->data, sl->len))
        return 0;

    size_t prev_slot = (c->wq_head + c->wq_count - 1) % c->wq_cap;
    sm_write_entry_t *prev = &c->write_queue[prev_slot];
    if (prev->offset != 0)
        return 0; /* head partially flushed — do not merge (would reset offset) */
    if (prev->len >= SM_CLIENT_COALESCE_MAX_BYTES)
        return 0; /* merged head already large — stop before the decode +
                     re-encode cost grows with backlog depth (O(K^2)) */
    if (!prev->shared || !line_is_output(prev->data, prev->len))
        return 0;

    sm_msg_t old_msg = sm_msg_decode(prev->data, prev->len);
    sm_msg_t new_msg = sm_msg_decode(sl->data, sl->len);
    if (!old_msg.root || !new_msg.root ||
        old_msg.type != SM_MSG_OUTPUT || new_msg.type != SM_MSG_OUTPUT) {
        sm_msg_free(&old_msg);
        sm_msg_free(&new_msg);
        return 0;
    }

    const char *old_b64 = sm_json_get_string(old_msg.root, "data");
    const char *new_b64 = sm_json_get_string(new_msg.root, "data");
    double ts = sm_json_get_double(new_msg.root, "timestamp", 0.0);
    if (!old_b64 || !new_b64) {
        sm_msg_free(&old_msg);
        sm_msg_free(&new_msg);
        return 0;
    }

    size_t old_len, new_len;
    uint8_t *old_raw = sm_base64_decode(old_b64, strlen(old_b64), &old_len);
    uint8_t *new_raw = sm_base64_decode(new_b64, strlen(new_b64), &new_len);
    sm_msg_free(&old_msg);
    sm_msg_free(&new_msg);
    if (!old_raw || !new_raw) {
        free(old_raw);
        free(new_raw);
        return 0;
    }

    uint8_t *merged = realloc(old_raw, old_len + new_len);
    if (!merged) {
        free(old_raw);
        free(new_raw);
        return 0;
    }
    memcpy(merged + old_len, new_raw, new_len);
    free(new_raw);

    cJSON *out = sm_msg_output(merged, old_len + new_len, ts);
    free(merged);
    if (!out) return 0;

    size_t mlen;
    char *line = sm_msg_encode(out, &mlen);
    cJSON_Delete(out);
    if (!line) return 0;

    sm_shared_line_t *merged_sl = sm_shared_line_new(line, mlen);
    if (!merged_sl)
        return 0; /* sm_shared_line_new owns+frees line on failure */

    sm_shared_line_release(prev->shared);
    prev->shared = merged_sl;
    prev->data = merged_sl->data;
    prev->len = merged_sl->len;
    prev->offset = 0;
    return 1;
}

sm_client_t *sm_client_new(int fd, unsigned int client_num)
{
    sm_client_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;

    c->fd = fd;
    snprintf(c->id, sizeof(c->id), "client-%u", client_num);
    snprintf(c->role, sizeof(c->role), "observer");

    c->read_cap = SM_CLIENT_READ_BUF_SIZE;
    c->read_buf = malloc(c->read_cap);

    c->wq_cap = SM_CLIENT_WRITE_QUEUE_SIZE;
    c->write_queue = calloc(c->wq_cap, sizeof(sm_write_entry_t));

    if (!c->read_buf || !c->write_queue) {
        sm_client_destroy(c);
        return NULL;
    }

    return c;
}

void sm_client_destroy(sm_client_t *c)
{
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    free(c->read_buf);
    for (size_t i = 0; i < c->wq_count; i++) {
        sm_write_entry_t *e = &c->write_queue[(c->wq_head + i) % c->wq_cap];
        if (e->shared)
            sm_shared_line_release(e->shared);
        else
            free(e->data);
    }
    free(c->write_queue);
    free(c);
}

size_t sm_client_feed(sm_client_t *c, sm_msg_t *out, size_t max_out)
{
    /* Read available data */
    size_t avail = c->read_cap - c->read_len - 1;
    if (avail == 0) {
        /* A single line filled the whole read buffer with no newline: a client
         * message this large is a protocol violation. Disconnect rather than
         * silently discarding it and re-spinning on level-triggered EPOLLIN
         * (which would busy-loop and lose data without any signal). */
        SM_LOG_WARN("client",
                    "%s: line exceeds %zu-byte buffer without newline, disconnecting",
                    c->id, c->read_cap);
        c->disconnected = 1;
        return 0;
    }
    ssize_t n = read(c->fd, c->read_buf + c->read_len, avail);
    if (n <= 0) {
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
            c->disconnected = 1;
        return 0;
    }
    c->read_len += (size_t)n;

    /* Extract complete lines */
    size_t count = 0;
    size_t start = 0;

    for (size_t i = 0; i < c->read_len && count < max_out; i++) {
        if (c->read_buf[i] == '\n') {
            size_t line_len = i - start;
            if (line_len > 0) {
                out[count] = sm_msg_decode((const char *)c->read_buf + start, line_len);
                if (out[count].root)
                    count++;
            }
            start = i + 1;
        }
    }

    /* Compact read buffer */
    if (start > 0) {
        c->read_len -= start;
        if (c->read_len > 0)
            memmove(c->read_buf, c->read_buf + start, c->read_len);
    }

    return count;
}

int sm_client_send(sm_client_t *c, cJSON *msg)
{
    size_t len;
    char *line = sm_msg_encode(msg, &len);
    if (!line) return -1;

    if (c->wq_count >= c->wq_cap) {
        /* Drop message — queue full */
        c->wq_drops++;
        if (c->wq_drops == 1 || (c->wq_drops % 100) == 0)
            SM_LOG_WARN("client", "%s: write queue full, dropped %zu messages",
                        c->id, c->wq_drops);
        free(line);
        return -1;
    }

    size_t slot = (c->wq_head + c->wq_count) % c->wq_cap;
    sm_write_entry_t *e = &c->write_queue[slot];
    e->data = line;
    e->len = len;
    e->offset = 0;
    c->wq_count++;
    return 0;
}

int sm_client_send_raw(sm_client_t *c, char *data, size_t len)
{
    if (c->wq_count >= c->wq_cap) {
        c->wq_drops++;
        if (c->wq_drops == 1 || (c->wq_drops % 100) == 0)
            SM_LOG_WARN("client", "%s: write queue full, dropped %zu messages",
                        c->id, c->wq_drops);
        free(data);
        return -1;
    }

    size_t slot = (c->wq_head + c->wq_count) % c->wq_cap;
    sm_write_entry_t *e = &c->write_queue[slot];
    e->data = data;
    e->shared = NULL;
    e->len = len;
    e->offset = 0;
    c->wq_count++;
    return 0;
}

int sm_client_send_shared(sm_client_t *c, sm_shared_line_t *sl)
{
    if (!sl) return -1;

    if (try_coalesce_output(c, sl))
        return 0;

    if (c->wq_count >= c->wq_cap) {
        c->wq_drops++;
        if (c->wq_drops == 1 || (c->wq_drops % 100) == 0)
            SM_LOG_WARN("client", "%s: write queue full, dropped %zu messages",
                        c->id, c->wq_drops);
        return -1;
    }

    sm_shared_line_acquire(sl);
    size_t slot = (c->wq_head + c->wq_count) % c->wq_cap;
    sm_write_entry_t *e = &c->write_queue[slot];
    e->shared = sl;
    e->data = sl->data;
    e->len = sl->len;
    e->offset = 0;
    c->wq_count++;
    return 0;
}

int sm_client_flush(sm_client_t *c)
{
    while (c->wq_count > 0) {
        sm_write_entry_t *e = &c->write_queue[c->wq_head];
        ssize_t n = write(c->fd, e->data + e->offset, e->len - e->offset);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 1; /* more pending */
            return -1;    /* error */
        }
        e->offset += (size_t)n;
        if (e->offset >= e->len) {
            if (e->shared)
                sm_shared_line_release(e->shared);
            else
                free(e->data);
            e->shared = NULL;
            e->data = NULL;
            c->wq_head = (c->wq_head + 1) % c->wq_cap;
            c->wq_count--;
        }
    }
    return 0; /* done */
}
