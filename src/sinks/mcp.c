#include "sinks/mcp.h"
#include "sinks/mcp_internal.h"
#include "broker.h"
#include "logger.h"
#include "util/str.h"
#include "sinks/mcp_schemas.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#define LOG_TAG "mcp"

/* --- JSON-RPC helpers --- */

static void mcp_write_json(cJSON *msg)
{
    char *str = cJSON_PrintUnformatted(msg);
    if (!str) return;
    size_t len = strlen(str);
    /* Write message + newline atomically */
    char *buf = malloc(len + 2);
    if (!buf) { free(str); return; }
    memcpy(buf, str, len);
    buf[len] = '\n';
    buf[len + 1] = '\0';
    /* Use write() to avoid stdio buffering issues */
    ssize_t written = 0;
    size_t total = len + 1;
    while ((size_t)written < total) {
        ssize_t n = write(STDOUT_FILENO, buf + written, total - (size_t)written);
        if (n <= 0) break;
        written += n;
    }
    free(buf);
    free(str);
}

void mcp_send_result(cJSON *id, cJSON *result)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id)
        cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
    else
        cJSON_AddNullToObject(resp, "id");
    cJSON_AddItemToObject(resp, "result", result);
    mcp_write_json(resp);
    cJSON_Delete(resp);
}

void mcp_send_tool_result(cJSON *id, const char *text)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *content = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text);
    cJSON_AddItemToArray(content, item);
    cJSON_AddItemToObject(result, "content", content);
    mcp_send_result(id, result);
}

static void mcp_send_error(cJSON *id, int code, const char *message)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id)
        cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
    else
        cJSON_AddNullToObject(resp, "id");
    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    cJSON_AddItemToObject(resp, "error", err);
    mcp_write_json(resp);
    cJSON_Delete(resp);
}

/* --- Pending call management --- */

sm_mcp_pending_t *mcp_alloc_pending(sm_mcp_sink_t *mcp, cJSON *jsonrpc_id,
                                     const char *expect_id)
{
    for (int i = 0; i < SM_MCP_MAX_PENDING_CALLS; i++) {
        if (!mcp->pending[i].jsonrpc_id) {
            mcp->pending[i].jsonrpc_id = cJSON_Duplicate(jsonrpc_id, 1);
            snprintf(mcp->pending[i].expect_id, sizeof(mcp->pending[i].expect_id),
                     "%s", expect_id);
            mcp->pending[i].timeout_mode = 0;
            mcp->pending[i].is_monitor = 0;
            return &mcp->pending[i];
        }
    }
    return NULL;
}

void mcp_gen_expect_id(sm_mcp_sink_t *mcp, char *buf, size_t len)
{
    snprintf(buf, len, "mcp-%08x", mcp->next_expect_seq++);
}

/* --- Output buffer management --- */

void mcp_buffer_output(sm_mcp_sink_t *mcp, const uint8_t *data, size_t len)
{
    size_t valid = 0;
    const uint8_t *p = data;
    while (p < data + len) {
        const void *nul = memchr(p, 0, (size_t)(data + len - p));
        if (!nul) {
            valid += (size_t)(data + len - p);
            break;
        }
        valid += (size_t)((const uint8_t *)nul - p);
        p = (const uint8_t *)nul + 1;
    }
    if (valid == 0) return;

    /* Grow geometrically up to the max before evicting (M10) */
    while (mcp->output_len + valid > mcp->output_cap &&
           mcp->output_cap < SM_MCP_OUTPUT_BUFFER_MAX) {
        size_t new_cap = mcp->output_cap * 2;
        if (new_cap > SM_MCP_OUTPUT_BUFFER_MAX)
            new_cap = SM_MCP_OUTPUT_BUFFER_MAX;
        void *tmp = realloc(mcp->output_buf, new_cap);
        if (!tmp) break;  /* fall back to eviction */
        mcp->output_buf = tmp;
        mcp->output_cap = new_cap;
    }

    /* Evict if needed to make room */
    if (mcp->output_len + valid > mcp->output_cap) {
        size_t need = mcp->output_len + valid - mcp->output_cap;
        size_t evict = need > mcp->output_len ? mcp->output_len : need;
        if (evict < mcp->output_cap / 2)
            evict = mcp->output_cap / 2;
        if (evict > mcp->output_len) evict = mcp->output_len;
        size_t keep = mcp->output_len - evict;
        memmove(mcp->output_buf, mcp->output_buf + evict, keep);
        mcp->output_len = keep;
    }

    p = data;
    while (p < data + len) {
        const void *nul = memchr(p, 0, (size_t)(data + len - p));
        if (!nul) {
            size_t run = (size_t)(data + len - p);
            memcpy(mcp->output_buf + mcp->output_len, p, run);
            mcp->output_len += run;
            break;
        }
        size_t run = (size_t)((const uint8_t *)nul - p);
        if (run > 0) {
            memcpy(mcp->output_buf + mcp->output_len, p, run);
            mcp->output_len += run;
        }
        p = (const uint8_t *)nul + 1;
    }
}

char *mcp_drain_output(sm_mcp_sink_t *mcp)
{
    if (mcp->output_len == 0) return strdup("(no output)");
    char *text = malloc(mcp->output_len + 1);
    if (!text) return strdup("(allocation failed)");
    memcpy(text, mcp->output_buf, mcp->output_len);
    text[mcp->output_len] = '\0';
    mcp->output_len = 0;
    return text;
}

/* --- Message dispatch --- */

static void handle_initialize(sm_mcp_sink_t *mcp, cJSON *id)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "protocolVersion", SM_MCP_PROTOCOL_VERSION);
    cJSON *caps = cJSON_CreateObject();
    cJSON_AddItemToObject(caps, "tools", cJSON_CreateObject());
    cJSON_AddItemToObject(result, "capabilities", caps);
    cJSON *info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "name", SM_NAME);
    cJSON_AddStringToObject(info, "version", SM_VERSION);
    cJSON_AddItemToObject(result, "serverInfo", info);
    mcp_send_result(id, result);
    mcp->initialized = 1;
    SM_LOG_INFO(LOG_TAG, "MCP initialized");
}

static void handle_tools_list(sm_mcp_sink_t *mcp, cJSON *id)
{
    (void)mcp;
    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "tools", sm_mcp_build_tools_list());
    mcp_send_result(id, result);
}

static void handle_tools_call(sm_mcp_sink_t *mcp, cJSON *id, cJSON *params)
{
    const char *name = NULL;
    cJSON *name_item = cJSON_GetObjectItemCaseSensitive(params, "name");
    if (cJSON_IsString(name_item))
        name = name_item->valuestring;

    if (!name) {
        mcp_send_error(id, -32602, "missing tool name");
        return;
    }

    cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
    cJSON *args_tmp = NULL;
    if (!args) {
        args_tmp = cJSON_CreateObject();
        args = args_tmp;
    }

    char *result = mcp_tool_dispatch(mcp, name, args, id);
    if (result) {
        mcp_send_tool_result(id, result);
        free(result);
    }
    /* If NULL returned, response will be sent when pending call resolves */
    cJSON_Delete(args_tmp);
}

static void dispatch_message(sm_mcp_sink_t *mcp, cJSON *msg)
{
    cJSON *method = cJSON_GetObjectItemCaseSensitive(msg, "method");
    cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");

    if (!cJSON_IsString(method)) {
        if (id) mcp_send_error(id, -32600, "invalid request: missing method");
        return;
    }

    const char *m = method->valuestring;

    if (strcmp(m, "initialize") == 0) {
        handle_initialize(mcp, id);
    } else if (strcmp(m, "notifications/initialized") == 0) {
        /* Notification — no response */
    } else if (strcmp(m, "tools/list") == 0) {
        handle_tools_list(mcp, id);
    } else if (strcmp(m, "tools/call") == 0) {
        cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");
        cJSON *params_tmp = NULL;
        if (!params) {
            params_tmp = cJSON_CreateObject();
            params = params_tmp;
        }
        handle_tools_call(mcp, id, params);
        cJSON_Delete(params_tmp);
    } else {
        if (id) mcp_send_error(id, -32601, "method not found");
    }
}

/* --- Sink callbacks --- */

static int mcp_start(sm_sink_t *self, void *broker)
{
    sm_mcp_sink_t *mcp = self->data;
    mcp->broker = broker;
    SM_LOG_INFO(LOG_TAG, "MCP sink started on stdin/stdout");
    return 0;
}

static void mcp_stop(sm_sink_t *self)
{
    (void)self;
    SM_LOG_INFO(LOG_TAG, "MCP sink stopped");
}

static void mcp_on_output(sm_sink_t *self, const uint8_t *data, size_t len,
                            double ts)
{
    (void)ts;
    sm_mcp_sink_t *mcp = self->data;
    mcp_buffer_output(mcp, data, len);
}

static void mcp_on_event(sm_sink_t *self, const char *event,
                           const char *payload)
{
    (void)self;
    (void)event;
    (void)payload;
    /* Anomaly events are stored in broker's detector, queried via tools */
}

static void mcp_on_readable(sm_sink_t *self)
{
    sm_mcp_sink_t *mcp = self->data;

    /* Read from stdin */
    if (mcp->read_len >= mcp->read_cap) {
        if (mcp->read_cap >= SM_MCP_READ_BUF_MAX) {
            /* Buffer at max with no newline — discard to prevent OOM */
            SM_LOG_WARN(LOG_TAG, "read buffer overflow (%zu bytes, no newline), discarding",
                        mcp->read_len);
            mcp->read_len = 0;
            return;
        }
        size_t new_cap = mcp->read_cap * 2;
        if (new_cap > SM_MCP_READ_BUF_MAX) new_cap = SM_MCP_READ_BUF_MAX;
        void *tmp = realloc(mcp->read_buf, new_cap);
        if (!tmp) return;
        mcp->read_buf = tmp;
        mcp->read_cap = new_cap;
    }

    ssize_t n = read(mcp->stdin_fd, mcp->read_buf + mcp->read_len,
                     mcp->read_cap - mcp->read_len);
    if (n <= 0) {
        if (n == 0) {
            SM_LOG_INFO(LOG_TAG, "stdin closed, stopping broker");
            sm_broker_stop(mcp->broker);
        }
        return;
    }
    mcp->read_len += (size_t)n;

    /* Process complete lines */
    while (1) {
        char *nl = memchr(mcp->read_buf, '\n', mcp->read_len);
        if (!nl) break;

        size_t line_len = (size_t)(nl - mcp->read_buf);
        mcp->read_buf[line_len] = '\0';

        /* Parse JSON */
        cJSON *msg = cJSON_Parse(mcp->read_buf);
        if (msg) {
            dispatch_message(mcp, msg);
            cJSON_Delete(msg);
        } else {
            SM_LOG_WARN(LOG_TAG, "malformed JSON on stdin");
            /* Try to extract id for error response */
            mcp_send_error(NULL, -32700, "parse error");
        }

        /* Shift buffer */
        size_t remaining = mcp->read_len - line_len - 1;
        memmove(mcp->read_buf, nl + 1, remaining);
        mcp->read_len = remaining;
    }
}

static void mcp_on_expect_result(sm_sink_t *self, const char *id,
                                   int matched, const uint8_t *data,
                                   size_t data_len, const char *pattern)
{
    sm_mcp_sink_t *mcp = self->data;

    /* Find the pending call */
    for (int i = 0; i < SM_MCP_MAX_PENDING_CALLS; i++) {
        sm_mcp_pending_t *p = &mcp->pending[i];
        if (!p->jsonrpc_id) continue;
        if (strcmp(p->expect_id, id) != 0) continue;

        /* Build response text */
        char *text;
        if (data && data_len > 0) {
            text = malloc(data_len + 64);
            if (!text) { text = strdup("(allocation failed)"); goto send_result; }
            if (!matched && !p->timeout_mode && !p->is_monitor) {
                /* Expect timed out with no match: prefix the captured output. */
                snprintf(text, data_len + 64, "[TIMEOUT] ");
                size_t prefix_len = strlen(text);
                memcpy(text + prefix_len, data, data_len);
                text[prefix_len + data_len] = '\0';
            } else if (p->is_monitor) {
                /* Build report with sm_strbuf — the previous data_len+1024
                 * budget underflowed once accumulated incident lines pushed
                 * the snprintf offset past the cap, writing past the heap
                 * allocation (same class as the mcp_tools.c fix). */
                size_t inc_count;
                const sm_anomaly_incident_t *incidents =
                    sm_anomaly_get_incidents(&mcp->broker->anomaly, &inc_count);

                sm_strbuf_t sb;
                sm_strbuf_init(&sb);
                sm_strbuf_append_str(&sb,
                    "## Serial Monitor Report\n\nOutput:\n```\n");
                sm_strbuf_append(&sb, (const char *)data, data_len);
                sm_strbuf_append_str(&sb, "\n```\n");

                int inc_found = 0;
                for (size_t j = 0; j < inc_count; j++) {
                    if (incidents[j].timestamp >= p->monitor_start) {
                        if (!inc_found) {
                            sm_strbuf_append_str(&sb, "\n## Anomalies Detected\n");
                            inc_found = 1;
                        }
                        sm_strbuf_printf(&sb, "- **%s** [%s]: %s\n",
                            incidents[j].pattern_name,
                            incidents[j].severity,
                            incidents[j].match_text);
                    }
                }
                if (!inc_found)
                    sm_strbuf_append_str(&sb,
                        "\nNo anomalies detected during monitoring.\n");

                char *rpt = sm_strbuf_steal(&sb);
                if (rpt) {
                    free(text);
                    text = rpt;
                } else {
                    snprintf(text, data_len + 64, "(allocation failed)");
                }
            } else {
                memcpy(text, data, data_len);
                text[data_len] = '\0';
            }
        } else {
            text = strdup("(no output)");
        }

send_result:
        mcp_send_tool_result(p->jsonrpc_id, text ? text : "(allocation failed)");
        free(text);

        /* Free pending entry */
        cJSON_Delete(p->jsonrpc_id);
        p->jsonrpc_id = NULL;
        break;
    }
}

static void mcp_destroy(sm_sink_t *self)
{
    sm_mcp_sink_t *mcp = self->data;

    /* Free pending calls */
    for (int i = 0; i < SM_MCP_MAX_PENDING_CALLS; i++) {
        if (mcp->pending[i].jsonrpc_id)
            cJSON_Delete(mcp->pending[i].jsonrpc_id);
    }

    free(mcp->output_buf);
    free(mcp->read_buf);
    free(mcp);
    free(self);
}

/* --- Factory --- */

sm_sink_t *sm_mcp_sink_new(sm_broker_t *broker)
{
    sm_sink_t *sink = calloc(1, sizeof(sm_sink_t));
    sm_mcp_sink_t *mcp = calloc(1, sizeof(sm_mcp_sink_t));

    if (!sink || !mcp) {
        free(sink);
        free(mcp);
        return NULL;
    }

    mcp->broker = broker;
    /* Start small; mcp_buffer_output grows on demand up to
     * SM_MCP_OUTPUT_BUFFER_MAX (M10) */
    mcp->output_cap = 8192;
    mcp->output_buf = malloc(mcp->output_cap);
    mcp->output_len = 0;
    mcp->read_cap = 8192;
    mcp->read_buf = malloc(mcp->read_cap);
    mcp->read_len = 0;
    mcp->stdin_fd = STDIN_FILENO;

    if (!mcp->output_buf || !mcp->read_buf) {
        free(mcp->output_buf);
        free(mcp->read_buf);
        free(mcp);
        free(sink);
        return NULL;
    }

    sink->name = "mcp";
    sink->start = mcp_start;
    sink->stop = mcp_stop;
    sink->on_output = mcp_on_output;
    sink->on_event = mcp_on_event;
    sink->on_readable = mcp_on_readable;
    sink->on_expect_result = mcp_on_expect_result;
    sink->destroy = mcp_destroy;
    sink->data = mcp;
    sink->fd = STDIN_FILENO;

    return sink;
}
