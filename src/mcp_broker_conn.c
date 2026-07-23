#include "mcp_broker_conn.h"
#include "constants.h"
#include "protocol.h"
#include "util/base64.h"
#include "util/json_helpers.h"
#include "util/sock_util.h"
#include "logger.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOG_TAG "mcp-conn"

int sm_broker_conn_init(sm_broker_conn_t *c, size_t read_cap)
{
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->running = 1;
    c->read_cap = read_cap;
    c->read_buf = malloc(read_cap);
    if (!c->read_buf) return -1;
    return 0;
}

void sm_broker_conn_destroy(sm_broker_conn_t *c)
{
    if (!c) return;
    if (c->fd >= 0) { close(c->fd); c->fd = -1; }
    free(c->read_buf);
    c->read_buf = NULL;
    c->read_len = 0;
    c->read_cap = 0;
}

void sm_broker_conn_set_output_cb(sm_broker_conn_t *c,
                                  sm_broker_output_fn fn, void *user)
{
    c->on_output = fn;
    c->on_output_user = user;
}

void sm_broker_conn_gen_wire_id(sm_broker_conn_t *c, char *buf, size_t len)
{
    snprintf(buf, len, "mcp-%08x", c->next_seq++);
}

int sm_broker_conn_send(sm_broker_conn_t *c, cJSON *msg)
{
    size_t len;
    char *line = sm_msg_encode(msg, &len);
    if (!line) {
        cJSON_Delete(msg);
        return -1;
    }
    int rc = sm_write_all(c->fd, line, len);
    free(line);
    cJSON_Delete(msg);
    return rc;
}

cJSON *sm_broker_conn_read(sm_broker_conn_t *c, const char *wire_id)
{
    if (c->read_len >= c->read_cap - 1) {
        size_t new_cap = c->read_cap * 2;
        if (new_cap > SM_MCP_READ_BUF_MAX) new_cap = SM_MCP_READ_BUF_MAX;
        if (new_cap <= c->read_cap) {
            c->read_len = 0;   /* oversized line — drop it rather than stall */
            return NULL;
        }
        void *tmp = realloc(c->read_buf, new_cap);
        if (!tmp) return NULL;
        c->read_buf = tmp;
        c->read_cap = new_cap;
    }

    ssize_t n = read(c->fd, c->read_buf + c->read_len,
                     c->read_cap - c->read_len - 1);
    if (n <= 0) {
        if (n == 0) {
            SM_LOG_INFO(LOG_TAG, "broker disconnected");
            c->running = 0;
        }
        return NULL;
    }
    c->read_len += (size_t)n;

    cJSON *result = NULL;
    char *start = c->read_buf;
    char *nl;
    while ((nl = memchr(start, '\n',
                        c->read_len - (size_t)(start - c->read_buf))) != NULL) {
        size_t line_len = (size_t)(nl - start);
        sm_msg_t msg = sm_msg_decode(start, line_len);
        start = nl + 1;

        if (!msg.root) continue;

        if (msg.type == SM_MSG_OUTPUT) {
            const char *b64 = sm_json_get_string(msg.root, "data");
            if (b64 && c->on_output) {
                size_t dec_len;
                uint8_t *data = sm_base64_decode(b64, strlen(b64), &dec_len);
                if (data) {
                    c->on_output(c->on_output_user, data, dec_len);
                    free(data);
                }
            }
            sm_msg_free(&msg);
            continue;
        }

        if (wire_id) {
            const char *id = sm_json_get_string(msg.root, "id");
            if (id && strcmp(id, wire_id) == 0) {
                result = msg.root;
                msg.root = NULL;
                sm_msg_free(&msg);
                break;
            }
        } else if (msg.type == SM_MSG_WELCOME) {
            /* wire_id NULL is the startup welcome wait: welcome carries no id,
             * so match it by type (else the wait always times out). */
            result = msg.root;
            msg.root = NULL;
            sm_msg_free(&msg);
            break;
        }

        if (c->verbose && msg.type != SM_MSG_WELCOME)
            SM_LOG_DEBUG(LOG_TAG, "unmatched broker message type=%s",
                         sm_msg_type_name(msg.type));
        sm_msg_free(&msg);
    }

    size_t remaining = c->read_len - (size_t)(start - c->read_buf);
    if (remaining > 0 && start != c->read_buf)
        memmove(c->read_buf, start, remaining);
    c->read_len = remaining;

    return result;
}

cJSON *sm_broker_conn_wait(sm_broker_conn_t *c, const char *wire_id,
                           int timeout_ms)
{
    struct pollfd pfd = { .fd = c->fd, .events = POLLIN };
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    while (c->running) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t remain_ms = (deadline.tv_sec - now.tv_sec) * 1000 +
                            (deadline.tv_nsec - now.tv_nsec) / 1000000;
        if (remain_ms <= 0) return NULL;

        int ret = poll(&pfd, 1,
                       (int)(remain_ms > INT32_MAX ? INT32_MAX : remain_ms));
        if (ret < 0) {
            if (errno == EINTR) continue;
            return NULL;
        }
        if (ret == 0) return NULL;

        if (pfd.revents & (POLLHUP | POLLERR)) {
            c->running = 0;
            return NULL;
        }
        if (pfd.revents & POLLIN) {
            cJSON *resp = sm_broker_conn_read(c, wire_id);
            if (resp) return resp;
        }
    }
    return NULL;
}

cJSON *sm_broker_conn_request(sm_broker_conn_t *c, cJSON *msg,
                              const char *wire_id, int timeout_ms)
{
    if (sm_broker_conn_send(c, msg) < 0) return NULL;
    return sm_broker_conn_wait(c, wire_id, timeout_ms);
}

int sm_broker_conn_pump(sm_broker_conn_t *c, int timeout_ms)
{
    struct pollfd pfd = { .fd = c->fd, .events = POLLIN };
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret < 0)
        return errno == EINTR ? 0 : -1;
    if (ret == 0)
        return 0;
    if (pfd.revents & (POLLHUP | POLLERR)) {
        c->running = 0;
        return -1;
    }
    if (pfd.revents & POLLIN) {
        /* read() delivers OUTPUT to on_output and may return a stray
         * welcome/id-matched message; we drive correlation ourselves, so
         * drop anything it hands back. */
        cJSON *stray = sm_broker_conn_read(c, NULL);
        if (stray) cJSON_Delete(stray);
        return c->running ? 1 : -1;
    }
    return 0;
}
