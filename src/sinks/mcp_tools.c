#include "sinks/mcp.h"
#include "sinks/mcp_internal.h"
#include "sinks/mcp_schemas.h"
#include "sinks/mcp_results.h"
#include "broker.h"
#include "logger.h"
#include "mcp_explain.h"
#include "util/base64.h"
#include "util/json_helpers.h"
#include "util/str.h"
#include "util/sock_util.h"
#include "util/timeutil.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <glob.h>

#define LOG_TAG "mcp-tools"

/* --- Tool implementations --- */

static char *tool_serial_send_command(sm_mcp_sink_t *mcp, cJSON *args,
                                       cJSON *jsonrpc_id)
{
    sm_broker_t *b = mcp->broker;
    const char *command = sm_json_get_string(args, "command");
    if (!command) return strdup("[ERROR] missing 'command' argument");

    const char *expect_pattern = sm_json_get_string(args, "expect_pattern");
    int timeout_ms = sm_json_get_int(args, "timeout_ms", 0);

    /* Apply profile */
    char cmd_buf[4096];
    int nw;
    if (b->profile.command_prefix[0])
        nw = snprintf(cmd_buf, sizeof(cmd_buf), "%s%s",
                      b->profile.command_prefix, command);
    else
        nw = snprintf(cmd_buf, sizeof(cmd_buf), "%s", command);
    if (nw < 0 || (size_t)nw >= sizeof(cmd_buf))
        return strdup("[ERROR] command too long");

    /* Resolve timeout */
    if (timeout_ms <= 0)
        timeout_ms = b->profile.default_timeout_ms;

    /* Resolve pattern */
    int timeout_mode = 0;
    const char *pattern;
    if (expect_pattern && expect_pattern[0]) {
        pattern = expect_pattern;
    } else if (strcmp(b->profile.response_mode, "timeout") == 0) {
        pattern = "^\\b$";  /* Never matches */
        timeout_mode = 1;
    } else {
        pattern = b->profile.prompt_pattern;
    }

    const char *eol = sm_json_get_string(args, "eol");
    const char *ending = "\n";
    size_t ending_len = 1;
    if (eol && strcmp(eol, "cr") == 0) {
        ending = "\r";
        ending_len = 1;
    } else if (eol && strcmp(eol, "crlf") == 0) {
        ending = "\r\n";
        ending_len = 2;
    } else if (eol && eol[0] && strcmp(eol, "lf") != 0) {
        return strdup("[ERROR] eol must be lf, cr, or crlf");
    }

    size_t cmd_len = strlen(cmd_buf);
    while (cmd_len > 0 &&
           (cmd_buf[cmd_len - 1] == '\n' || cmd_buf[cmd_len - 1] == '\r'))
        cmd_buf[--cmd_len] = '\0';
    if (cmd_len + ending_len >= sizeof(cmd_buf))
        return strdup("[ERROR] command too long");
    memcpy(cmd_buf + cmd_len, ending, ending_len);
    cmd_buf[cmd_len + ending_len] = '\0';
    cmd_len += ending_len;

    /* Check link status */
    if (b->suspended) return strdup("[ERROR] serial port is suspended");
    if (b->link_disconnected) return strdup("[ERROR] serial port disconnected");
    if (b->link->read_fd(b->link) < 0) return strdup("[ERROR] serial port not connected");

    /* Generate expect id and register */
    char expect_id[16];
    mcp_gen_expect_id(mcp, expect_id, sizeof(expect_id));

    double timeout_s = (double)timeout_ms / 1000.0;
    {
        int erc = sm_expect_add(&b->expect, expect_id, pattern, timeout_s,
                                SM_MCP_CLIENT_ID);
        if (erc != 0) {
            char err[128];
            snprintf(err, sizeof(err), "[ERROR] %s", sm_expect_add_errstr(erc));
            return strdup(err);
        }
    }

    /* Write command to device */
    int rc = sm_broker_do_write(b, (const uint8_t *)cmd_buf, cmd_len, "mcp");
    if (rc < 0) {
        sm_expect_cancel_id(&b->expect, expect_id);
        return strdup("[ERROR] write failed");
    }

    /* Broadcast input echo */
    double ts = sm_now_realtime();
    cJSON *echo = sm_msg_input_echo((const uint8_t *)cmd_buf, cmd_len, "mcp", ts);
    sm_broker_broadcast_msg(b, echo);
    cJSON_Delete(echo);

    /* Register pending call */
    sm_mcp_pending_t *p = mcp_alloc_pending(mcp, jsonrpc_id, expect_id);
    if (!p) {
        sm_expect_cancel_client(&b->expect, SM_MCP_CLIENT_ID);
        return strdup("[ERROR] too many pending calls");
    }
    p->timeout_mode = timeout_mode;

    return NULL;  /* Response will be sent when expect resolves */
}

static char *tool_serial_read(sm_mcp_sink_t *mcp)
{
    char *text = mcp_drain_output(mcp);
    size_t n;
    const sm_anomaly_incident_t *incs =
        sm_anomaly_get_incidents(&mcp->broker->anomaly, &n);
    return sm_mcp_append_recent_incidents(text, incs, n, 5);
}

static char *tool_serial_write(sm_mcp_sink_t *mcp, cJSON *args)
{
    sm_broker_t *b = mcp->broker;
    const char *data_str = sm_json_get_string(args, "data");
    if (!data_str) return strdup("[ERROR] missing 'data' argument");

    size_t len = strlen(data_str);
    int rc = sm_broker_do_write(b, (const uint8_t *)data_str, len, "mcp");
    if (rc == -1) return sm_mcp_error_with_hint("[ERROR] serial port is suspended");
    if (rc == -2) return sm_mcp_error_with_hint("[ERROR] serial port disconnected");
    if (rc == -3) return sm_mcp_error_with_hint("[ERROR] write failed");

    double ts = sm_now_realtime();
    cJSON *echo = sm_msg_input_echo((const uint8_t *)data_str, len, "mcp", ts);
    sm_broker_broadcast_msg(b, echo);
    cJSON_Delete(echo);

    return strdup("OK");
}

static char *tool_serial_port_status(sm_mcp_sink_t *mcp, cJSON **sc)
{
    /* Same status_response a wire client gets, rendered by the shared
     * mcp_results code, so both MCP surfaces report identical fields. */
    cJSON *status = sm_broker_status_json(mcp->broker, NULL);
    *sc = sm_mcp_port_status_structured(status, NULL);
    char *out = sm_mcp_port_status_text(status, NULL);
    cJSON_Delete(status);
    return out;
}

static char *tool_serial_add_autoresponder(sm_mcp_sink_t *mcp, cJSON *args)
{
    sm_broker_t *b = mcp->broker;
    const char *name = sm_json_get_string(args, "name");
    const char *pattern = sm_json_get_string(args, "pattern");
    const char *send = sm_json_get_string(args, "send");
    if (!name || !pattern || !send)
        return strdup("[ERROR] missing name, pattern, or send");

    int once = sm_json_get_bool(args, "once", 0);
    int cooldown_ms = sm_json_get_int(args, "cooldown_ms", SM_AR_DEFAULT_COOLDOWN_MS);

    uint8_t resp[SM_AR_RESPONSE_MAX];
    size_t rlen = sm_str_unescape(send, resp, sizeof(resp));

    int rc = sm_autoresponder_add(&b->autoresponder, name, pattern,
                                  resp, rlen, once, cooldown_ms);
    if (rc != 0)
        return strdup("[ERROR] autoresponder rejected (bad regex, full, or "
                      "response too long)");

    char result[384];
    snprintf(result, sizeof(result),
             "Autoresponder '%s' added (pattern=/%s/, %zu bytes%s)",
             name, pattern, rlen, once ? ", once" : "");
    return strdup(result);
}

static char *tool_serial_boot_status(sm_mcp_sink_t *mcp, cJSON **sc)
{
    cJSON *status = sm_broker_status_json(mcp->broker, NULL);
    *sc = sm_mcp_boot_status_structured(status);
    char *out = sm_mcp_boot_status_text(status);
    cJSON_Delete(status);
    return out;
}

/* Completion context for a deferred break/SysRq tool call. The JSON-RPC
 * response is sent when the broker's break state machine finishes. */
typedef struct mcp_break_ctx {
    cJSON *jsonrpc_id;   /* owned copy */
    char key;            /* SysRq key, 0 for a plain break */
} mcp_break_ctx_t;

static void mcp_break_done(sm_broker_t *b, void *ctx_, int rc)
{
    (void)b;
    mcp_break_ctx_t *ctx = ctx_;
    char result[48];

    if (rc != 0)
        snprintf(result, sizeof(result), "[ERROR] %s failed",
                 ctx->key ? "BREAK+SysRq" : "pin control");
    else if (ctx->key)
        snprintf(result, sizeof(result), "OK (SysRq+%c)", ctx->key);
    else
        snprintf(result, sizeof(result), "OK");

    mcp_send_tool_result(ctx->jsonrpc_id, result);
    cJSON_Delete(ctx->jsonrpc_id);
    free(ctx);
}

/* Same allowlist as handle_pin_control (broker.c): wire docs say dtr|rts|break.
 * Without this, serial_pin_control on the in-process MCP sink re-opens SM-15
 * (agent can set_param("allow_shell","1") on a GDB link). */
static int mcp_pin_is_line_control(const char *pin)
{
    return strcmp(pin, "dtr") == 0 ||
           strcmp(pin, "rts") == 0 ||
           strcmp(pin, "break") == 0;
}

static char *tool_serial_pin_control(sm_mcp_sink_t *mcp, cJSON *args,
                                      cJSON *jsonrpc_id)
{
    sm_broker_t *b = mcp->broker;
    const char *pin = sm_json_get_string(args, "pin");
    const char *action = sm_json_get_string(args, "action");
    int duration_ms = sm_json_get_int(args, "duration_ms", 250);

    if (!pin || !action)
        return strdup("[ERROR] missing pin or action");

    if (!mcp_pin_is_line_control(pin))
        return strdup("[ERROR] unknown pin (expected dtr, rts, or break)");
    if (strcmp(action, "send") == 0 && strcmp(pin, "break") != 0)
        action = "pulse";

    if (strcmp(pin, "break") == 0) {
        mcp_break_ctx_t *ctx = calloc(1, sizeof(*ctx));
        if (!ctx) return strdup("[ERROR] out of memory");
        ctx->jsonrpc_id = cJSON_Duplicate(jsonrpc_id, 1);
        if (sm_broker_schedule_break(b, duration_ms, NULL, 0, 0,
                                     mcp_break_done, ctx) != 0) {
            cJSON_Delete(ctx->jsonrpc_id);
            free(ctx);
            return strdup("[ERROR] break busy or unavailable");
        }
        return NULL;  /* response sent by mcp_break_done */
    }

    int rc = b->link->set_param(b->link, pin, action);
    return strdup(rc == 0 ? "OK" : "[ERROR] pin control failed");
}

static char *tool_serial_sysrq(sm_mcp_sink_t *mcp, cJSON *args,
                                cJSON *jsonrpc_id)
{
    sm_broker_t *b = mcp->broker;
    const char *key = sm_json_get_string(args, "key");
    if (!key || !key[0]) return strdup("[ERROR] missing 'key' argument");

    int break_ms = sm_json_get_int(args, "break_duration_ms", 500);
    int delay_ms = sm_json_get_int(args, "delay_ms", 100);

    mcp_break_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return strdup("[ERROR] out of memory");
    ctx->jsonrpc_id = cJSON_Duplicate(jsonrpc_id, 1);
    ctx->key = key[0];

    uint8_t ch = (uint8_t)key[0];
    if (sm_broker_schedule_break(b, break_ms, &ch, 1, delay_ms,
                                 mcp_break_done, ctx) != 0) {
        cJSON_Delete(ctx->jsonrpc_id);
        free(ctx);
        return strdup("[ERROR] break busy or unavailable");
    }
    return NULL;  /* response sent by mcp_break_done */
}

static char *tool_serial_suspend(sm_mcp_sink_t *mcp)
{
    sm_broker_t *b = mcp->broker;

    int rc = sm_broker_do_suspend(b, "mcp");
    if (rc < 0)
        return sm_mcp_error_with_hint("[ERROR] already suspended");

    char result[512];
    snprintf(result, sizeof(result),
             "OK - serial port %s released. Use serial_resume() when done.", b->port);
    return strdup(result);
}

static char *tool_serial_resume(sm_mcp_sink_t *mcp)
{
    sm_broker_t *b = mcp->broker;

    int rc = sm_broker_do_resume(b, "mcp");
    if (rc == -1)
        return sm_mcp_error_with_hint("[ERROR] not suspended");
    if (rc == -2)
        return sm_mcp_error_with_hint("[ERROR] failed to reopen serial port");

    return strdup("OK - serial port re-acquired.");
}

/* Completion of serial_reset without wait_pattern: answer on release. */
typedef struct mcp_reset_ctx {
    cJSON *jsonrpc_id;   /* owned copy; NULL when an expect answers instead */
    char pin[4];
    int hold_ms;
} mcp_reset_ctx_t;

static void mcp_reset_done(sm_broker_t *b, void *ctx_, int rc)
{
    (void)b;
    mcp_reset_ctx_t *ctx = ctx_;
    if (ctx->jsonrpc_id) {
        char result[96];
        if (rc != 0)
            snprintf(result, sizeof(result), "[ERROR] reset via %s failed",
                     ctx->pin);
        else
            snprintf(result, sizeof(result), "OK (reset via %s, held %d ms)",
                     ctx->pin, ctx->hold_ms);
        mcp_send_tool_result(ctx->jsonrpc_id, result);
        cJSON_Delete(ctx->jsonrpc_id);
    } else if (rc != 0) {
        SM_LOG_WARN(LOG_TAG, "serial_reset via %s failed while waiting", ctx->pin);
    }
    free(ctx);
}

/* serial_reset on the in-broker sink: the broker's line-reset machine runs
 * it on the event loop (never blocks). Scheduling asserts the line first,
 * then the optional wait is registered - still before the timer releases
 * the line - and answers the call; otherwise the release answers it. */
static char *tool_serial_reset(sm_mcp_sink_t *mcp, cJSON *args,
                               cJSON *jsonrpc_id)
{
    sm_broker_t *b = mcp->broker;
    const char *pin = sm_json_get_string(args, "pin");
    if (!pin || !pin[0]) pin = "rts";
    if (strcmp(pin, "rts") != 0 && strcmp(pin, "dtr") != 0)
        return strdup("[ERROR] pin must be rts or dtr");
    int hold_ms = sm_json_get_int(args, "hold_ms", SM_DEFAULT_RESET_HOLD_MS);
    if (hold_ms < 1) hold_ms = 1;
    if (hold_ms > SM_MAX_RESET_HOLD_MS) hold_ms = SM_MAX_RESET_HOLD_MS;
    const char *wait_pat = sm_json_get_string(args, "wait_pattern");
    if (wait_pat && !wait_pat[0]) wait_pat = NULL;
    int wait_ms = sm_json_get_int(args, "timeout_ms", 10000);
    if (wait_ms < 100) wait_ms = 100;
    if (wait_ms > SM_MAX_EXPECT_TIMEOUT_MS) wait_ms = SM_MAX_EXPECT_TIMEOUT_MS;

    mcp_reset_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return strdup("[ERROR] out of memory");
    snprintf(ctx->pin, sizeof(ctx->pin), "%s", pin);
    ctx->hold_ms = hold_ms;
    if (!wait_pat)
        ctx->jsonrpc_id = cJSON_Duplicate(jsonrpc_id, 1);
    if (sm_broker_schedule_line_reset(b, pin, hold_ms, mcp_reset_done, ctx) != 0) {
        cJSON_Delete(ctx->jsonrpc_id);
        free(ctx);
        return strdup("[ERROR] reset busy or unavailable (suspended, link "
                      "down, or modem lines not drivable)");
    }
    if (!wait_pat)
        return NULL;  /* answered by mcp_reset_done on release */

    char expect_id[16];
    mcp_gen_expect_id(mcp, expect_id, sizeof(expect_id));
    int erc = sm_expect_add(&b->expect, expect_id, wait_pat,
                            (double)wait_ms / 1000.0, SM_MCP_CLIENT_ID);
    if (erc != 0) {
        char err[160];
        snprintf(err, sizeof(err), "[ERROR] reset started, but wait_pattern "
                 "rejected: %s", sm_expect_add_errstr(erc));
        return strdup(err);
    }
    if (!mcp_alloc_pending(mcp, jsonrpc_id, expect_id)) {
        sm_expect_cancel_id(&b->expect, expect_id);
        return strdup("[ERROR] reset started, but too many pending calls");
    }
    return NULL;  /* answered by the expect result */
}

static char *tool_serial_wait_for(sm_mcp_sink_t *mcp, cJSON *args,
                                    cJSON *jsonrpc_id)
{
    sm_broker_t *b = mcp->broker;
    const char *pattern = sm_json_get_string(args, "pattern");
    if (!pattern || !pattern[0])
        return strdup("[ERROR] missing 'pattern' argument");

    int timeout_ms = sm_json_get_int(args, "timeout_ms", 30000);
    if (timeout_ms < 100) timeout_ms = 100;
    if (timeout_ms > SM_MAX_EXPECT_TIMEOUT_MS)
        timeout_ms = SM_MAX_EXPECT_TIMEOUT_MS;

    if (b->suspended) return strdup("[ERROR] serial port is suspended");
    if (b->link->read_fd(b->link) < 0)
        return strdup("[ERROR] serial port not connected");

    char expect_id[16];
    mcp_gen_expect_id(mcp, expect_id, sizeof(expect_id));

    double timeout_s = (double)timeout_ms / 1000.0;
    {
        int erc = sm_expect_add(&b->expect, expect_id, pattern, timeout_s,
                                SM_MCP_CLIENT_ID);
        if (erc != 0) {
            char err[128];
            snprintf(err, sizeof(err), "[ERROR] %s", sm_expect_add_errstr(erc));
            return strdup(err);
        }
    }

    sm_mcp_pending_t *p = mcp_alloc_pending(mcp, jsonrpc_id, expect_id);
    if (!p) {
        sm_expect_cancel_id(&b->expect, expect_id);
        return strdup("[ERROR] too many pending calls");
    }
    /* No TX — listen only. */
    return NULL;
}

static cJSON *history_json_page(sm_broker_t *b, uint64_t since_seq, int max_bytes)
{
    if (max_bytes <= 0)
        max_bytes = (int)SM_MAX_HISTORY_RESPONSE_BYTES;
    if (max_bytes > (int)SM_MAX_HISTORY_RESPONSE_BYTES)
        max_bytes = (int)SM_MAX_HISTORY_RESPONSE_BYTES;

    sm_rb_chunk_t *chunks = NULL;
    size_t first_skip = 0;
    uint64_t cursor = 0, dropped = 0;
    int has_more = 0;
    size_t count = sm_rb_get_since_seq(&b->history, since_seq, (size_t)max_bytes,
                                       &chunks, &first_skip, &cursor, &dropped,
                                       &has_more);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "cursor", (double)cursor);
    cJSON_AddNumberToObject(root, "dropped", (double)dropped);
    cJSON_AddBoolToObject(root, "has_more", has_more ? 1 : 0);
    cJSON *arr = cJSON_AddArrayToObject(root, "chunks");

    size_t encoded = 0;
    for (size_t i = 0; i < count; i++) {
        size_t off = (i == 0) ? first_skip : 0;
        size_t len = chunks[i].len - off;
        if (encoded + len > (size_t)max_bytes)
            len = (size_t)max_bytes - encoded;
        if (len == 0)
            break;
        cJSON *ch = cJSON_CreateObject();
        /* Prefer UTF-8 text; base64 only if needed — keep simple: raw as string
         * with NULs stripped for MCP JSON. */
        char *txt = malloc(len + 1);
        size_t t = 0;
        if (!txt) {
            cJSON_Delete(ch);
            cJSON_Delete(root);
            free(chunks);
            return NULL;
        }
        for (size_t j = 0; j < len; j++) {
            uint8_t c = chunks[i].data[off + j];
            if (c != 0) txt[t++] = (char)c;
        }
        txt[t] = '\0';
        cJSON_AddStringToObject(ch, "text", txt);
        free(txt);
        cJSON_AddNumberToObject(ch, "timestamp", chunks[i].timestamp);
        cJSON_AddNumberToObject(ch, "seq_start",
                                (double)(chunks[i].seq_start + (uint64_t)off));
        cJSON_AddItemToArray(arr, ch);
        encoded += len;
        if (encoded >= (size_t)max_bytes)
            break;
    }
    free(chunks);
    return root;
}

static char *tool_serial_output_history(sm_mcp_sink_t *mcp, cJSON *args,
                                       cJSON **sc)
{
    sm_broker_t *b = mcp->broker;

    cJSON *since_item = cJSON_GetObjectItemCaseSensitive(args, "since_seq");
    if (cJSON_IsNumber(since_item)) {
        uint64_t since_seq = (uint64_t)since_item->valuedouble;
        int max_bytes = sm_json_get_int(args, "max_bytes", 0);
        cJSON *page = history_json_page(b, since_seq, max_bytes);
        if (!page)
            return strdup("(allocation failed)");
        *sc = sm_mcp_history_page_structured(page);
        char *out = sm_mcp_history_page_text(page);
        cJSON_Delete(page);
        return out;
    }

    double seconds = sm_json_get_double(args, "seconds", 0.0);
    int last_bytes = sm_json_get_int(args, "last_bytes", 0);

    sm_rb_chunk_t *chunks = NULL;
    size_t count;

    if (last_bytes > 0)
        count = sm_rb_get_last_n_bytes(&b->history, (size_t)last_bytes, &chunks);
    else if (seconds > 0) {
        double since_ts = sm_now_realtime() - seconds;
        count = sm_rb_get_since(&b->history, since_ts, &chunks);
    } else
        count = sm_rb_get_all(&b->history, &chunks);

    if (count == 0) {
        free(chunks);
        *sc = sm_mcp_history_text_structured("");
        return strdup("(no output in history)");
    }

    /* Concatenate all chunks */
    size_t total_len = 0;
    for (size_t i = 0; i < count; i++)
        total_len += chunks[i].len;

    char *text = malloc(total_len + 1);
    if (!text) {
        free(chunks);
        return strdup("(allocation failed)");
    }
    size_t off = 0;
    for (size_t i = 0; i < count; i++) {
        /* Strip NUL bytes */
        for (size_t j = 0; j < chunks[i].len; j++) {
            if (chunks[i].data[j] != 0)
                text[off++] = (char)chunks[i].data[j];
        }
    }
    text[off] = '\0';
    free(chunks);

    *sc = sm_mcp_history_text_structured(text);
    if (off == 0) {
        free(text);
        return strdup("(no output in history)");
    }
    return text;
}

static char *tool_serial_get_incidents(sm_mcp_sink_t *mcp, cJSON *args,
                                      cJSON **sc)
{
    double seconds = sm_json_get_double(args, "seconds", 0.0);
    double since_ts = seconds > 0 ? sm_now_realtime() - seconds : 0.0;
    cJSON *incidents = sm_broker_incidents_json(mcp->broker, since_ts);
    *sc = sm_mcp_incidents_structured(incidents);
    char *out = sm_mcp_incidents_text(incidents);
    cJSON_Delete(incidents);
    return out;
}

static char *tool_serial_add_watchdog(sm_mcp_sink_t *mcp, cJSON *args)
{
    sm_broker_t *b = mcp->broker;
    const char *name = sm_json_get_string(args, "name");
    const char *pattern = sm_json_get_string(args, "pattern");
    const char *severity = sm_json_get_string(args, "severity");
    if (!severity) severity = "warning";

    if (!name || !pattern) return strdup("[ERROR] missing name or pattern");

    int rc = sm_anomaly_add_pattern(&b->anomaly, name, pattern, severity);
    if (rc != 0) return strdup("[ERROR] invalid regex pattern");

    char result[256];
    snprintf(result, sizeof(result),
             "Watchdog '%s' added (severity=%s, pattern=%s)", name, severity, pattern);
    return strdup(result);
}

static char *tool_serial_monitor(sm_mcp_sink_t *mcp, cJSON *args,
                                   cJSON *jsonrpc_id)
{
    sm_broker_t *b = mcp->broker;
    int duration = sm_json_get_int(args, "duration_seconds", 30);
    if (duration > 300) duration = 300;
    if (duration < 1) duration = 1;

    if (b->suspended) return strdup("[ERROR] serial port is suspended");
    if (b->link->read_fd(b->link) < 0)
        return strdup("[ERROR] serial port not connected");

    /* Register expect with never-matching pattern */
    char expect_id[16];
    mcp_gen_expect_id(mcp, expect_id, sizeof(expect_id));

    double timeout_s = (double)duration;
    {
        int erc = sm_expect_add(&b->expect, expect_id, "^\\b$", timeout_s,
                                SM_MCP_CLIENT_ID);
        if (erc != 0) {
            char err[128];
            snprintf(err, sizeof(err), "[ERROR] %s", sm_expect_add_errstr(erc));
            return strdup(err);
        }
    }

    /* Register pending call */
    sm_mcp_pending_t *p = mcp_alloc_pending(mcp, jsonrpc_id, expect_id);
    if (!p) {
        sm_expect_cancel_client(&b->expect, SM_MCP_CLIENT_ID);
        return strdup("[ERROR] too many pending calls");
    }
    p->is_monitor = 1;
    p->monitor_start = sm_now_realtime();

    return NULL;  /* Response sent when expect times out */
}

/* Built with sm_strbuf — the previous fixed 8KB buffer overflowed once
 * accumulated snprintf return values exceeded the cap (M15). */
static char *tool_serial_generate_report(sm_mcp_sink_t *mcp)
{
    sm_broker_t *b = mcp->broker;
    sm_strbuf_t sb;
    sm_strbuf_init(&sb);

    sm_strbuf_printf(&sb, "# smolmux Device Report\n\n");

    sm_strbuf_printf(&sb,
        "## Device Profile\n"
        "- Name: %s\n"
        "- Type: %s\n"
        "- Description: %s\n"
        "- Prompt: `%s`\n"
        "- Response mode: %s\n",
        b->profile.name, b->profile.device_type,
        b->profile.description, b->profile.prompt_pattern,
        b->profile.response_mode);
    if (b->profile.command_prefix[0])
        sm_strbuf_printf(&sb, "- Command prefix: `%s`\n",
                         b->profile.command_prefix);

    sm_strbuf_printf(&sb,
        "\n## Port Status\n"
        "- Port: %s\n"
        "- Baud: %d\n"
        "- Connected: %s\n"
        "- Clients: %zu\n",
        b->port, b->baudrate,
        b->link->read_fd(b->link) >= 0 ? "true" : "false",
        b->client_count);

    /* Incidents */
    size_t inc_count;
    const sm_anomaly_incident_t *incidents =
        sm_anomaly_get_incidents(&b->anomaly, &inc_count);
    if (inc_count > 0) {
        sm_strbuf_printf(&sb, "\n## Incidents (%zu total)\n", inc_count);
        for (size_t i = 0; i < inc_count; i++) {
            sm_strbuf_printf(&sb, "- **%s** [%s]: %s\n",
                             incidents[i].pattern_name, incidents[i].severity,
                             incidents[i].match_text);
        }
    } else {
        sm_strbuf_printf(&sb, "\n## Incidents\nNone detected.\n");
    }

    /* Recent output */
    sm_rb_chunk_t *chunks = NULL;
    size_t chunk_count = sm_rb_get_last_n_bytes(&b->history, 32768, &chunks);
    sm_strbuf_printf(&sb, "\n## Recent Output (last 32KB)\n");
    if (chunk_count > 0) {
        sm_strbuf_printf(&sb, "```\n");
        for (size_t i = 0; i < chunk_count; i++) {
            /* Append runs between NUL bytes */
            size_t start = 0;
            for (size_t j = 0; j <= chunks[i].len; j++) {
                if (j == chunks[i].len || chunks[i].data[j] == 0) {
                    if (j > start)
                        sm_strbuf_append(&sb,
                                         (const char *)chunks[i].data + start,
                                         j - start);
                    start = j + 1;
                }
            }
        }
        sm_strbuf_printf(&sb, "\n```\n");
    } else {
        sm_strbuf_printf(&sb, "(no output)\n");
    }
    free(chunks);

    char *out = sm_strbuf_steal(&sb);
    return out ? out : strdup("(allocation failed)");
}

static char *tool_serial_list_ports(cJSON **sc)
{
    return sm_mcp_list_ports(sc);
}

/* --- Tool dispatcher --- */

char *mcp_tool_dispatch(sm_mcp_sink_t *mcp, const char *name, cJSON *args,
                         cJSON *jsonrpc_id, cJSON **structured)
{
    char *result = NULL;
    *structured = NULL;
    if (sm_mcp_tool_is_mutate(name) && !sm_mcp_mutate_enabled())
        return strdup("[ERROR] mutate tools disabled (set SMOLMUX_MCP_MUTATE=1)");
    /* In-process --mcp bypasses the Unix-client takeover check. Keep the
     * --mcp flag (do not delete); refuse TX while another client holds it. */
    if (sm_mcp_tool_is_mutate(name) && mcp->broker->takeover_client)
        return strdup("[ERROR] another client holds takeover");
    if (strcmp(name, "serial_send_command") == 0)
        result = tool_serial_send_command(mcp, args, jsonrpc_id);
    else if (strcmp(name, "serial_read") == 0)
        result = tool_serial_read(mcp);
    else if (strcmp(name, "serial_write") == 0)
        result = tool_serial_write(mcp, args);
    else if (strcmp(name, "serial_port_status") == 0)
        result = tool_serial_port_status(mcp, structured);
    else if (strcmp(name, "serial_boot_status") == 0)
        result = tool_serial_boot_status(mcp, structured);
    else if (strcmp(name, "serial_add_autoresponder") == 0)
        result = tool_serial_add_autoresponder(mcp, args);
    else if (strcmp(name, "serial_pin_control") == 0)
        result = tool_serial_pin_control(mcp, args, jsonrpc_id);
    else if (strcmp(name, "serial_reset") == 0)
        result = tool_serial_reset(mcp, args, jsonrpc_id);
    else if (strcmp(name, "serial_sysrq") == 0)
        result = tool_serial_sysrq(mcp, args, jsonrpc_id);
    else if (strcmp(name, "serial_suspend") == 0)
        result = tool_serial_suspend(mcp);
    else if (strcmp(name, "serial_resume") == 0)
        result = tool_serial_resume(mcp);
    else if (strcmp(name, "serial_wait_for") == 0)
        result = tool_serial_wait_for(mcp, args, jsonrpc_id);
    else if (strcmp(name, "serial_output_history") == 0)
        result = tool_serial_output_history(mcp, args, structured);
    else if (strcmp(name, "serial_get_incidents") == 0)
        result = tool_serial_get_incidents(mcp, args, structured);
    else if (strcmp(name, "serial_add_watchdog") == 0)
        result = tool_serial_add_watchdog(mcp, args);
    else if (strcmp(name, "serial_monitor") == 0)
        result = tool_serial_monitor(mcp, args, jsonrpc_id);
    else if (strcmp(name, "serial_generate_report") == 0)
        result = tool_serial_generate_report(mcp);
    else if (strcmp(name, "serial_list_ports") == 0)
        result = tool_serial_list_ports(structured);
    else {
        char err[256];
        snprintf(err, sizeof(err), "[ERROR] unknown tool: %s", name);
        result = sm_mcp_error_with_hint(err);
    }
    if (result)
        result = sm_mcp_maybe_explain_result(result);
    return result;
}
