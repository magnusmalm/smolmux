/*
 * smolmux-mcp — standalone MCP server over stdio
 *
 * Connects to a running smolmux broker via Unix socket (or TCP) and exposes
 * MCP tools over stdio JSON-RPC. Translates MCP tool calls into wire protocol
 * messages, waits for correlated responses, and formats MCP results.
 */

#include "constants.h"
#include "protocol.h"
#include "device_profile.h"
#include "util/base64.h"
#include "util/json_helpers.h"
#include "util/sock_util.h"
#include "broker_info.h"
#include "util/str.h"
#include "util/timeutil.h"
#include "util/profile_resolve.h"
#include "sinks/mcp_schemas.h"
#include "mcp_broker_conn.h"
#include "mcp_instructions.h"
#include "mcp_explain.h"
#include "logger.h"
#include "cJSON.h"

#include <errno.h>
#include <getopt.h>
#include <glob.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOG_TAG "mcp-client"

/* --- Global state --- */

static struct {
    /* Broker connection (socket, read buffer, wire-id seq, running flag) */
    sm_broker_conn_t conn;

    /* Output buffer for serial_read */
    uint8_t *output_buf;
    size_t output_len;
    size_t output_cap;

    /* Stdin read buffer */
    char *stdin_buf;
    size_t stdin_len;
    size_t stdin_cap;

    /* Device profile */
    sm_device_profile_t profile;

    /* Client name */
    char name[64];

    /* Soft-fail broker connect (Wave 4): never auto-spawn; retry on tool use. */
    int offline;
    /* Why the last connect attempt failed, when the broker told us. Empty
     * means "no broker reachable" and the generic guidance applies; a
     * rejected hello (bad auth token) puts the broker's own words here so the
     * agent is told what actually went wrong instead of timing out blind. */
    char offline_reason[4096];
    int use_tcp;
    int socket_explicit; /* 1 if the user passed -s / positional socket */
    char socket_path[SM_SOCK_PATH_MAX];
    char tcp_host[256];
    int tcp_port;

    char last_link_event[32];

    int verbose;
    int initialized;
} ctx;

/* --- Broker plumbing: thin wrappers over the shared connection core so the
 * tool handlers keep their concise call sites. --- */

static void gen_wire_id(char *buf, size_t len)
{
    sm_broker_conn_gen_wire_id(&ctx.conn, buf, len);
}

static int broker_send(cJSON *msg)
{
    return sm_broker_conn_send(&ctx.conn, msg);
}

static cJSON *read_broker(const char *wire_id)
{
    return sm_broker_conn_read(&ctx.conn, wire_id);
}

static cJSON *wait_for_response(const char *wire_id, int timeout_ms)
{
    return sm_broker_conn_wait(&ctx.conn, wire_id, timeout_ms);
}

static cJSON *broker_request(cJSON *msg, const char *wire_id, int timeout_ms)
{
    return sm_broker_conn_request(&ctx.conn, msg, wire_id, timeout_ms);
}

/* --- JSON-RPC output helpers --- */

static void mcp_write_json(cJSON *msg)
{
    char *str = cJSON_PrintUnformatted(msg);
    if (!str) return;
    size_t len = strlen(str);
    char *buf = malloc(len + 2);
    if (!buf) { free(str); return; }
    memcpy(buf, str, len);
    buf[len] = '\n';
    buf[len + 1] = '\0';
    size_t total = len + 1;
    size_t written = 0;
    while (written < total) {
        ssize_t n = write(STDOUT_FILENO, buf + written, total - written);
        if (n <= 0) break;
        written += (size_t)n;
    }
    free(buf);
    free(str);
}

static void mcp_send_result(cJSON *id, cJSON *result)
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

static void mcp_send_tool_result(cJSON *id, const char *text)
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

/* --- Output buffer --- */

static void buffer_output(const uint8_t *data, size_t len)
{
    size_t valid = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i] != 0) valid++;
    }
    if (valid == 0) return;

    if (ctx.output_len + valid > ctx.output_cap) {
        size_t need = ctx.output_len + valid - ctx.output_cap;
        size_t evict = need > ctx.output_len ? ctx.output_len : need;
        if (evict < ctx.output_cap / 2)
            evict = ctx.output_cap / 2;
        if (evict > ctx.output_len) evict = ctx.output_len;
        size_t keep = ctx.output_len - evict;
        memmove(ctx.output_buf, ctx.output_buf + evict, keep);
        ctx.output_len = keep;
    }

    for (size_t i = 0; i < len; i++) {
        if (data[i] != 0)
            ctx.output_buf[ctx.output_len++] = data[i];
    }
}

static char *drain_output(void)
{
    if (ctx.output_len == 0) return strdup("(no output)");
    char *text = malloc(ctx.output_len + 1);
    if (!text) return strdup("(allocation failed)");
    memcpy(text, ctx.output_buf, ctx.output_len);
    text[ctx.output_len] = '\0';
    ctx.output_len = 0;
    return text;
}

/* Broker output callback (registered on the shared connection): the decoded
 * SM_MSG_OUTPUT payload is appended to the local ring for serial_read. */
static void mcp_on_broker_output(void *user, const uint8_t *data, size_t len)
{
    (void)user;
    buffer_output(data, len);
}

static void mcp_on_broker_event(void *user, const char *type, cJSON *root)
{
    (void)user;
    (void)root;
    if (!type)
        return;
    snprintf(ctx.last_link_event, sizeof(ctx.last_link_event), "%s", type);
}

/* --- Tool handlers --- */

static char *tool_serial_send_command(cJSON *args)
{
    const char *command = sm_json_get_string(args, "command");
    if (!command) return strdup("[ERROR] missing 'command' argument");

    const char *expect_pattern = sm_json_get_string(args, "expect_pattern");
    int timeout_ms = sm_json_get_int(args, "timeout_ms", 0);

    /* Apply profile */
    char cmd_buf[4096];
    int nw;
    if (ctx.profile.command_prefix[0])
        nw = snprintf(cmd_buf, sizeof(cmd_buf), "%s%s",
                      ctx.profile.command_prefix, command);
    else
        nw = snprintf(cmd_buf, sizeof(cmd_buf), "%s", command);
    if (nw < 0 || (size_t)nw >= sizeof(cmd_buf))
        return strdup("[ERROR] command too long");

    if (timeout_ms <= 0)
        timeout_ms = ctx.profile.default_timeout_ms;

    int timeout_mode = 0;
    const char *pattern;
    if (expect_pattern && expect_pattern[0]) {
        pattern = expect_pattern;
    } else if (strcmp(ctx.profile.response_mode, "timeout") == 0) {
        pattern = "^\\b$";
        timeout_mode = 1;
    } else {
        pattern = ctx.profile.prompt_pattern;
    }

    /* Append line ending: eol=lf (default), cr, or crlf. */
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

    char wire_id[64];
    gen_wire_id(wire_id, sizeof(wire_id));

    cJSON *msg = sm_msg_send_expect(wire_id, (const uint8_t *)cmd_buf, cmd_len,
                                     pattern, timeout_ms);
    /* Wait for expect_result with generous timeout */
    int wait_ms = timeout_ms + 5000;
    cJSON *resp = broker_request(msg, wire_id, wait_ms);

    if (!resp) return strdup("[ERROR] timeout waiting for broker response");

    const char *resp_type = sm_json_get_string(resp, "type");
    if (resp_type && strcmp(resp_type, "error") == 0) {
        const char *emsg = sm_json_get_string(resp, "message");
        char err[512];
        snprintf(err, sizeof(err), "[ERROR] %s", emsg ? emsg : "unknown error");
        cJSON_Delete(resp);
        return strdup(err);
    }

    int matched = sm_json_get_bool(resp, "matched", 0);
    int aborted = sm_json_get_bool(resp, "aborted", 0);
    const char *abort_pattern = sm_json_get_string(resp, "abort_pattern");
    const char *data_b64 = sm_json_get_string(resp, "data");

    if (!data_b64 || !data_b64[0]) {
        if (aborted) {
            char err[256];
            snprintf(err, sizeof(err), "[ABORTED anomaly:%s] (no output)",
                     abort_pattern && abort_pattern[0]
                         ? abort_pattern : "unknown");
            cJSON_Delete(resp);
            return strdup(err);
        }
        cJSON_Delete(resp);
        return strdup("(no output)");
    }

    size_t dec_len;
    uint8_t *data = sm_base64_decode(data_b64, strlen(data_b64), &dec_len);
    cJSON_Delete(resp);

    if (!data) return strdup("(no output)");

    char *text;
    if (aborted) {
        text = malloc(dec_len + 128);
        if (!text) { free(data); return strdup("(allocation failed)"); }
        int prefix_len = snprintf(text, dec_len + 128,
            "[ABORTED anomaly:%s] ",
            abort_pattern && abort_pattern[0] ? abort_pattern : "unknown");
        memcpy(text + prefix_len, data, dec_len);
        text[prefix_len + (int)dec_len] = '\0';
    } else if (!matched && !timeout_mode) {
        text = malloc(dec_len + 64);
        if (!text) { free(data); return strdup("(allocation failed)"); }
        int prefix_len = snprintf(text, dec_len + 64, "[TIMEOUT] ");
        memcpy(text + prefix_len, data, dec_len);
        text[prefix_len + (int)dec_len] = '\0';
    } else {
        text = malloc(dec_len + 1);
        if (!text) { free(data); return strdup("(allocation failed)"); }
        memcpy(text, data, dec_len);
        text[dec_len] = '\0';
    }
    free(data);
    return text;
}

static char *tool_serial_read(void)
{
    char *text = drain_output();
    /* Best-effort: pull recent incidents from broker */
    char wire_id[64];
    gen_wire_id(wire_id, sizeof(wire_id));
    cJSON *resp = broker_request(sm_msg_incidents_request(wire_id, 0.0),
                                 wire_id, 2000);
    if (resp) {
        cJSON *arr = cJSON_GetObjectItemCaseSensitive(resp, "incidents");
        if (cJSON_IsArray(arr) && cJSON_GetArraySize(arr) > 0) {
            sm_strbuf_t sb;
            sm_strbuf_init(&sb);
            sm_strbuf_append_str(&sb, text ? text : "(no output)");
            sm_strbuf_append_str(&sb, "\n\n## Recent incidents\n");
            int n = cJSON_GetArraySize(arr);
            int start = n > 5 ? n - 5 : 0;
            for (int i = start; i < n; i++) {
                cJSON *inc = cJSON_GetArrayItem(arr, i);
                const char *name = sm_json_get_string(inc, "pattern_name");
                if (!name) name = sm_json_get_string(inc, "name");
                const char *sev = sm_json_get_string(inc, "severity");
                const char *mt = sm_json_get_string(inc, "match_text");
                sm_strbuf_printf(&sb, "- %s [%s]: %s\n",
                                 name ? name : "?", sev ? sev : "?",
                                 mt ? mt : "");
            }
            free(text);
            text = sm_strbuf_steal(&sb);
        }
        cJSON_Delete(resp);
    }
    return text ? text : strdup("(no output)");
}

static size_t unescape_c_escapes(const char *in, uint8_t *out, size_t out_cap)
{
    size_t n = 0;
    for (const char *p = in; *p && n + 1 < out_cap; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            uint8_t ch;
            switch (*p) {
            case 'r': ch = '\r'; break;
            case 'n': ch = '\n'; break;
            case 't': ch = '\t'; break;
            case '0': ch = '\0'; break;
            case '\\': ch = '\\'; break;
            default:
                out[n++] = '\\';
                if (n + 1 >= out_cap)
                    return n;
                ch = (uint8_t)*p;
                break;
            }
            out[n++] = ch;
        } else {
            out[n++] = (uint8_t)*p;
        }
    }
    return n;
}

static char *tool_serial_write(cJSON *args)
{
    const char *data_str = sm_json_get_string(args, "data");
    if (!data_str) return strdup("[ERROR] missing 'data' argument");

    size_t cap = strlen(data_str) + 1;
    uint8_t *raw = malloc(cap);
    if (!raw) return strdup("[ERROR] out of memory");
    size_t n = unescape_c_escapes(data_str, raw, cap);

    char wire_id[64];
    gen_wire_id(wire_id, sizeof(wire_id));

    cJSON *msg = sm_msg_send(wire_id, raw, n);
    free(raw);
    if (broker_send(msg) < 0) return strdup("[ERROR] failed to send");

    /* Short wait for potential error response */
    cJSON *resp = wait_for_response(wire_id, 500);
    if (resp) {
        const char *resp_type = sm_json_get_string(resp, "type");
        if (resp_type && strcmp(resp_type, "error") == 0) {
            const char *emsg = sm_json_get_string(resp, "message");
            char err[512];
            snprintf(err, sizeof(err), "[ERROR] %s",
                     emsg ? emsg : "unknown error");
            cJSON_Delete(resp);
            return strdup(err);
        }
        cJSON_Delete(resp);
    }
    return strdup("OK");
}

static char *tool_serial_port_status(void)
{
    char wire_id[64];
    gen_wire_id(wire_id, sizeof(wire_id));

    cJSON *resp = broker_request(sm_msg_status(wire_id), wire_id, 5000);
    if (!resp) return strdup("[ERROR] timeout");

    const char *resp_type = sm_json_get_string(resp, "type");
    if (resp_type && strcmp(resp_type, "error") == 0) {
        const char *emsg = sm_json_get_string(resp, "message");
        char err[512];
        snprintf(err, sizeof(err), "[ERROR] %s", emsg ? emsg : "unknown error");
        cJSON_Delete(resp);
        return strdup(err);
    }

    char buf[4096];
    int off = 0;
    off += snprintf(buf + off, sizeof(buf) - (size_t)off,
        "Port: %s\nBaud: %d\nConnected: %s\nSuspended: %s\n"
        "Link up ts: %.3f\nLast RX age ms: %d\nBytes since link up: %.0f\n"
        "Last link event: %s\nIdentity: %s\n",
        sm_json_get_string(resp, "port") ? sm_json_get_string(resp, "port") : "?",
        sm_json_get_int(resp, "baud", 0),
        sm_json_get_bool(resp, "connected", 0) ? "true" : "false",
        sm_json_get_bool(resp, "suspended", 0) ? "true" : "false",
        sm_json_get_double(resp, "link_up_ts", 0.0),
        sm_json_get_int(resp, "last_rx_age_ms", 0),
        sm_json_get_double(resp, "bytes_rx_since_link_up", 0.0),
        ctx.last_link_event[0] ? ctx.last_link_event : "(none)",
        sm_json_get_string(resp, "identity_strength")
            ? sm_json_get_string(resp, "identity_strength")
            : (sm_json_get_bool(resp, "identity_weak", 0) ? "WEAK" : "n/a"));
    if (sm_json_get_string(resp, "identity_by_id"))
        off += snprintf(buf + off, sizeof(buf) - (size_t)off,
            "Identity by-id: %s\n", sm_json_get_string(resp, "identity_by_id"));
    if (sm_json_get_string(resp, "identity_by_path"))
        off += snprintf(buf + off, sizeof(buf) - (size_t)off,
            "Identity by-path: %s\n",
            sm_json_get_string(resp, "identity_by_path"));

    cJSON *pins = cJSON_GetObjectItemCaseSensitive(resp, "pin_states");
    if (pins) {
        char *pin_str = cJSON_PrintUnformatted(pins);
        off += snprintf(buf + off, sizeof(buf) - (size_t)off,
            "Pin states: %s\n", pin_str ? pin_str : "{}");
        free(pin_str);
    }

    const char *takeover = sm_json_get_string(resp, "takeover_client");
    off += snprintf(buf + off, sizeof(buf) - (size_t)off,
        "Takeover: %s\n", takeover ? takeover : "none");

    const char *log_path = sm_json_get_string(resp, "log_path");
    if (log_path)
        off += snprintf(buf + off, sizeof(buf) - (size_t)off,
            "Log: %s\n", log_path);

    cJSON *clients = cJSON_GetObjectItemCaseSensitive(resp, "clients");
    if (cJSON_IsArray(clients)) {
        off += snprintf(buf + off, sizeof(buf) - (size_t)off, "Clients:\n");
        cJSON *ci;
        cJSON_ArrayForEach(ci, clients) {
            const char *cname = sm_json_get_string(ci, "name");
            const char *crole = sm_json_get_string(ci, "role");
            off += snprintf(buf + off, sizeof(buf) - (size_t)off,
                "  - %s (%s)\n", cname ? cname : "?", crole ? crole : "?");
        }
    }
    (void)off;
    cJSON_Delete(resp);
    return strdup(buf);
}

static char *tool_serial_boot_status(void)
{
    char wire_id[64];
    gen_wire_id(wire_id, sizeof(wire_id));

    cJSON *resp = broker_request(sm_msg_status(wire_id), wire_id, 5000);
    if (!resp) return strdup("[ERROR] timeout");

    const char *resp_type = sm_json_get_string(resp, "type");
    if (resp_type && strcmp(resp_type, "error") == 0) {
        char err[512];
        snprintf(err, sizeof(err), "[ERROR] %s",
                 sm_json_get_string(resp, "message") ?
                 sm_json_get_string(resp, "message") : "unknown error");
        cJSON_Delete(resp);
        return strdup(err);
    }

    cJSON *boot = cJSON_GetObjectItemCaseSensitive(resp, "boot");
    if (!boot) {
        cJSON_Delete(resp);
        return strdup("No boot_stages defined in the device profile — "
                      "boot progress tracking is not configured.");
    }

    int furthest = sm_json_get_int(boot, "furthest", -1);
    int total = sm_json_get_int(boot, "total", 0);
    int stalled = sm_json_get_bool(boot, "stalled", 0);
    int done = sm_json_get_bool(boot, "terminal_reached", 0);
    cJSON *stages = cJSON_GetObjectItemCaseSensitive(boot, "stages");

    int reached = 0, idx = 0;
    const char *fname = NULL;
    if (cJSON_IsArray(stages)) {
        cJSON *s;
        cJSON_ArrayForEach(s, stages) {
            if (sm_json_get_bool(s, "reached", 0)) reached++;
            if (idx == furthest) fname = sm_json_get_string(s, "name");
            idx++;
        }
    }
    const char *state = done ? "complete" : (stalled ? "STALLED"
                      : (furthest < 0 ? "not started" : "in progress"));

    sm_strbuf_t sb;
    sm_strbuf_init(&sb);
    sm_strbuf_printf(&sb, "Boot: %d/%d stages reached", reached, total);
    if (fname) sm_strbuf_printf(&sb, " (furthest: %s)", fname);
    sm_strbuf_printf(&sb, ", state: %s\n", state);
    if (cJSON_IsArray(stages)) {
        idx = 0;
        cJSON *s;
        cJSON_ArrayForEach(s, stages) {
            int r = sm_json_get_bool(s, "reached", 0);
            const char *nm = sm_json_get_string(s, "name");
            sm_strbuf_printf(&sb, "  [%c] %s%s\n", r ? 'x' : ' ',
                             nm ? nm : "?", idx == furthest ? "  <- furthest" : "");
            idx++;
        }
    }

    cJSON_Delete(resp);
    char *out = sm_strbuf_steal(&sb);
    return out ? out : strdup("(allocation failed)");
}

static char *tool_serial_add_autoresponder(cJSON *args)
{
    const char *name = sm_json_get_string(args, "name");
    const char *pattern = sm_json_get_string(args, "pattern");
    const char *send = sm_json_get_string(args, "send");
    if (!name || !pattern || !send)
        return strdup("[ERROR] missing name, pattern, or send");

    int once = sm_json_get_bool(args, "once", 0);
    int cooldown_ms = sm_json_get_int(args, "cooldown_ms", SM_AR_DEFAULT_COOLDOWN_MS);

    uint8_t resp[SM_AR_RESPONSE_MAX];
    size_t rlen = sm_str_unescape(send, resp, sizeof(resp));

    char wire_id[64];
    gen_wire_id(wire_id, sizeof(wire_id));

    cJSON *r = broker_request(
        sm_msg_configure_autoresponder(wire_id, name, pattern, resp, rlen,
                                       once, cooldown_ms, 0),
        wire_id, 5000);
    if (!r) return strdup("[ERROR] timeout");

    const char *rt = sm_json_get_string(r, "type");
    if (rt && strcmp(rt, "error") == 0) {
        const char *emsg = sm_json_get_string(r, "message");
        char err[512];
        snprintf(err, sizeof(err), "[ERROR] %s", emsg ? emsg : "rejected");
        cJSON_Delete(r);
        return strdup(err);
    }
    cJSON_Delete(r);

    char result[384];
    snprintf(result, sizeof(result),
             "Autoresponder '%s' added (pattern=/%s/, %zu bytes%s)",
             name, pattern, rlen, once ? ", once" : "");
    return strdup(result);
}

static char *tool_serial_pin_control(cJSON *args)
{
    const char *pin = sm_json_get_string(args, "pin");
    const char *action = sm_json_get_string(args, "action");
    int duration_ms = sm_json_get_int(args, "duration_ms", 250);
    if (!pin || !action) return strdup("[ERROR] missing pin or action");

    char wire_id[64];
    gen_wire_id(wire_id, sizeof(wire_id));

    cJSON *resp = broker_request(
        sm_msg_pin_control(wire_id, pin, action, duration_ms),
        wire_id, 5000);
    if (!resp) return strdup("[ERROR] timeout");

    const char *resp_type = sm_json_get_string(resp, "type");
    if (resp_type && strcmp(resp_type, "error") == 0) {
        const char *emsg = sm_json_get_string(resp, "message");
        char err[512];
        snprintf(err, sizeof(err), "[ERROR] %s", emsg ? emsg : "pin control failed");
        cJSON_Delete(resp);
        return strdup(err);
    }
    cJSON_Delete(resp);
    return strdup("OK");
}

static char *tool_serial_sysrq(cJSON *args)
{
    const char *key = sm_json_get_string(args, "key");
    if (!key || !key[0]) return strdup("[ERROR] missing 'key' argument");

    int break_ms = sm_json_get_int(args, "break_duration_ms", 500);
    int delay_ms = sm_json_get_int(args, "delay_ms", 100);
    if (break_ms > SM_MAX_BREAK_DURATION_MS) break_ms = SM_MAX_BREAK_DURATION_MS;
    if (delay_ms > SM_MAX_SYSRQ_DELAY_MS) delay_ms = SM_MAX_SYSRQ_DELAY_MS;

    /* Send BREAK via pin_control */
    char wire_id[64];
    gen_wire_id(wire_id, sizeof(wire_id));

    cJSON *resp = broker_request(
        sm_msg_pin_control(wire_id, "break", "send", break_ms),
        wire_id, break_ms + 5000);
    if (!resp) return strdup("[ERROR] timeout sending BREAK");

    const char *resp_type = sm_json_get_string(resp, "type");
    if (resp_type && strcmp(resp_type, "error") == 0) {
        const char *emsg = sm_json_get_string(resp, "message");
        char err[512];
        snprintf(err, sizeof(err), "[ERROR sending BREAK] %s",
                 emsg ? emsg : "unknown error");
        cJSON_Delete(resp);
        return strdup(err);
    }
    cJSON_Delete(resp);

    /* Delay between BREAK and key */
    struct timespec ts = {.tv_sec = delay_ms / 1000,
                          .tv_nsec = (delay_ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);

    /* Send key character */
    gen_wire_id(wire_id, sizeof(wire_id));
    uint8_t ch = (uint8_t)key[0];
    broker_send(sm_msg_send(wire_id, &ch, 1));

    char result[32];
    snprintf(result, sizeof(result), "OK (SysRq+%c)", key[0]);
    return strdup(result);
}

static char *tool_serial_suspend(void)
{
    char wire_id[64];
    gen_wire_id(wire_id, sizeof(wire_id));
    broker_send(sm_msg_suspend(wire_id));

    /* Short wait for potential error */
    cJSON *resp = wait_for_response(wire_id, 500);
    if (resp) {
        const char *resp_type = sm_json_get_string(resp, "type");
        if (resp_type && strcmp(resp_type, "error") == 0) {
            const char *emsg = sm_json_get_string(resp, "message");
            char err[512];
            snprintf(err, sizeof(err), "[ERROR] %s",
                     emsg ? emsg : "suspend failed");
            cJSON_Delete(resp);
            return strdup(err);
        }
        cJSON_Delete(resp);
    }
    return strdup("OK - serial port released. Use serial_resume() when done.");
}

static char *tool_serial_resume(void)
{
    char wire_id[64];
    gen_wire_id(wire_id, sizeof(wire_id));
    broker_send(sm_msg_resume(wire_id));

    /* Short wait for potential error */
    cJSON *resp = wait_for_response(wire_id, 500);
    if (resp) {
        const char *resp_type = sm_json_get_string(resp, "type");
        if (resp_type && strcmp(resp_type, "error") == 0) {
            const char *emsg = sm_json_get_string(resp, "message");
            char err[512];
            snprintf(err, sizeof(err), "[ERROR] %s",
                     emsg ? emsg : "resume failed");
            cJSON_Delete(resp);
            return strdup(err);
        }
        cJSON_Delete(resp);
    }
    return strdup("OK - serial port re-acquired.");
}

static char *tool_serial_wait_for(cJSON *args)
{
    const char *pattern = sm_json_get_string(args, "pattern");
    if (!pattern || !pattern[0])
        return strdup("[ERROR] missing 'pattern' argument");

    int timeout_ms = sm_json_get_int(args, "timeout_ms", 30000);
    if (timeout_ms < 100) timeout_ms = 100;
    if (timeout_ms > SM_MAX_EXPECT_TIMEOUT_MS)
        timeout_ms = SM_MAX_EXPECT_TIMEOUT_MS;

    char wire_id[64];
    gen_wire_id(wire_id, sizeof(wire_id));

    cJSON *msg = sm_msg_listen_expect(wire_id, pattern, timeout_ms);
    int wait_ms = timeout_ms + 5000;
    cJSON *resp = broker_request(msg, wire_id, wait_ms);
    if (!resp) return strdup("[ERROR] timeout waiting for broker response");

    const char *resp_type = sm_json_get_string(resp, "type");
    if (resp_type && strcmp(resp_type, "error") == 0) {
        const char *emsg = sm_json_get_string(resp, "message");
        char err[512];
        snprintf(err, sizeof(err), "[ERROR] %s", emsg ? emsg : "unknown error");
        cJSON_Delete(resp);
        return strdup(err);
    }

    int matched = sm_json_get_bool(resp, "matched", 0);
    int aborted = sm_json_get_bool(resp, "aborted", 0);
    const char *abort_pattern = sm_json_get_string(resp, "abort_pattern");
    const char *data_b64 = sm_json_get_string(resp, "data");

    if (!data_b64 || !data_b64[0]) {
        if (aborted) {
            char err[256];
            snprintf(err, sizeof(err), "[ABORTED anomaly:%s] (no output)",
                     abort_pattern && abort_pattern[0]
                         ? abort_pattern : "unknown");
            cJSON_Delete(resp);
            return strdup(err);
        }
        cJSON_Delete(resp);
        return matched ? strdup("(matched, no capture)")
                       : strdup("[TIMEOUT] (no output)");
    }

    size_t dec_len;
    uint8_t *data = sm_base64_decode(data_b64, strlen(data_b64), &dec_len);
    cJSON_Delete(resp);
    if (!data) return strdup("(no output)");

    char *text;
    if (aborted) {
        text = malloc(dec_len + 128);
        if (!text) { free(data); return strdup("(allocation failed)"); }
        int pl = snprintf(text, dec_len + 128, "[ABORTED anomaly:%s] ",
                          abort_pattern && abort_pattern[0]
                              ? abort_pattern : "unknown");
        memcpy(text + pl, data, dec_len);
        text[pl + (int)dec_len] = '\0';
    } else if (!matched) {
        text = malloc(dec_len + 64);
        if (!text) { free(data); return strdup("(allocation failed)"); }
        int pl = snprintf(text, dec_len + 64, "[TIMEOUT] ");
        memcpy(text + pl, data, dec_len);
        text[pl + (int)dec_len] = '\0';
    } else {
        text = malloc(dec_len + 1);
        if (!text) { free(data); return strdup("(allocation failed)"); }
        memcpy(text, data, dec_len);
        text[dec_len] = '\0';
    }
    free(data);
    return text;
}

static char *history_cursor_json_from_resp(cJSON *resp)
{
    /* Rebuild agent-facing JSON: cursor/dropped/has_more + text chunks. */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "cursor",
                            sm_json_get_double(resp, "cursor", 0.0));
    cJSON_AddNumberToObject(root, "dropped",
                            sm_json_get_double(resp, "dropped", 0.0));
    cJSON_AddBoolToObject(root, "has_more",
                          sm_json_get_bool(resp, "has_more", 0));
    cJSON *out_chunks = cJSON_AddArrayToObject(root, "chunks");
    cJSON *chunks = cJSON_GetObjectItemCaseSensitive(resp, "chunks");
    if (cJSON_IsArray(chunks)) {
        cJSON *chunk;
        cJSON_ArrayForEach(chunk, chunks) {
            const char *b64 = sm_json_get_string(chunk, "data");
            if (!b64) continue;
            size_t dec_len;
            uint8_t *data = sm_base64_decode(b64, strlen(b64), &dec_len);
            if (!data) continue;
            char *txt = malloc(dec_len + 1);
            size_t t = 0;
            if (txt) {
                for (size_t i = 0; i < dec_len; i++)
                    if (data[i] != 0) txt[t++] = (char)data[i];
                txt[t] = '\0';
            }
            free(data);
            cJSON *ch = cJSON_CreateObject();
            cJSON_AddStringToObject(ch, "text", txt ? txt : "");
            free(txt);
            cJSON_AddNumberToObject(ch, "timestamp",
                                    sm_json_get_double(chunk, "timestamp", 0.0));
            cJSON *ss = cJSON_GetObjectItemCaseSensitive(chunk, "seq_start");
            if (cJSON_IsNumber(ss))
                cJSON_AddNumberToObject(ch, "seq_start", ss->valuedouble);
            cJSON_AddItemToArray(out_chunks, ch);
        }
    }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out ? out : strdup("(allocation failed)");
}

static char *tool_serial_output_history(cJSON *args)
{
    cJSON *since_item = cJSON_GetObjectItemCaseSensitive(args, "since_seq");
    if (cJSON_IsNumber(since_item)) {
        uint64_t since_seq = (uint64_t)since_item->valuedouble;
        int max_bytes = sm_json_get_int(args, "max_bytes", 0);
        char wire_id[64];
        gen_wire_id(wire_id, sizeof(wire_id));
        cJSON *resp = broker_request(
            sm_msg_history_request_seq(wire_id, since_seq, max_bytes),
            wire_id, 10000);
        if (!resp) return strdup("[ERROR] timeout");
        const char *resp_type = sm_json_get_string(resp, "type");
        if (resp_type && strcmp(resp_type, "error") == 0) {
            const char *emsg = sm_json_get_string(resp, "message");
            char err[512];
            snprintf(err, sizeof(err), "[ERROR] %s",
                     emsg ? emsg : "unknown error");
            cJSON_Delete(resp);
            return strdup(err);
        }
        char *json = history_cursor_json_from_resp(resp);
        cJSON_Delete(resp);
        return json;
    }

    double seconds = sm_json_get_double(args, "seconds", 0.0);
    int last_bytes = sm_json_get_int(args, "last_bytes", 0);

    double since_ts = seconds > 0 ? sm_now_realtime() - seconds : 0.0;

    char wire_id[64];
    gen_wire_id(wire_id, sizeof(wire_id));

    cJSON *resp = broker_request(
        sm_msg_history_request(wire_id, since_ts, last_bytes),
        wire_id, 10000);
    if (!resp) return strdup("[ERROR] timeout");

    const char *resp_type = sm_json_get_string(resp, "type");
    if (resp_type && strcmp(resp_type, "error") == 0) {
        const char *emsg = sm_json_get_string(resp, "message");
        char err[512];
        snprintf(err, sizeof(err), "[ERROR] %s", emsg ? emsg : "unknown error");
        cJSON_Delete(resp);
        return strdup(err);
    }

    cJSON *chunks = cJSON_GetObjectItemCaseSensitive(resp, "chunks");
    if (!cJSON_IsArray(chunks) || cJSON_GetArraySize(chunks) == 0) {
        cJSON_Delete(resp);
        return strdup("(no output in history)");
    }

    /* Format with timestamps: [HH:MM:SS.mmm] text */
    size_t buf_cap = 65536;
    char *buf = malloc(buf_cap);
    if (!buf) { cJSON_Delete(resp); return strdup("(allocation failed)"); }
    size_t off = 0;

    cJSON *chunk;
    cJSON_ArrayForEach(chunk, chunks) {
        double ts = sm_json_get_double(chunk, "timestamp", 0.0);
        const char *b64 = sm_json_get_string(chunk, "data");
        if (!b64) continue;

        size_t dec_len;
        uint8_t *data = sm_base64_decode(b64, strlen(b64), &dec_len);
        if (!data) continue;

        /* Strip NUL bytes */
        size_t valid = 0;
        for (size_t i = 0; i < dec_len; i++) {
            if (data[i] != 0) valid++;
        }
        if (valid == 0) { free(data); continue; }

        /* Format timestamp */
        time_t sec = (time_t)ts;
        int ms = (int)((ts - (double)sec) * 1000.0);
        struct tm tm;
        gmtime_r(&sec, &tm);

        /* Grow buffer if needed */
        if (off + valid + 32 > buf_cap) {
            buf_cap = off + valid + 32768;
            void *tmp = realloc(buf, buf_cap);
            if (!tmp) { free(data); break; }
            buf = tmp;
        }

        off += (size_t)snprintf(buf + off, buf_cap - off,
            "[%02d:%02d:%02d.%03d] ",
            tm.tm_hour, tm.tm_min, tm.tm_sec, ms);

        for (size_t i = 0; i < dec_len; i++) {
            if (data[i] != 0 && off < buf_cap - 1)
                buf[off++] = (char)data[i];
        }
        free(data);
    }
    buf[off] = '\0';
    cJSON_Delete(resp);

    if (off == 0) {
        free(buf);
        return strdup("(no output in history)");
    }
    return buf;
}

static char *tool_serial_get_incidents(cJSON *args)
{
    double seconds = sm_json_get_double(args, "seconds", 0.0);
    double since_ts = seconds > 0 ? sm_now_realtime() - seconds : 0.0;

    char wire_id[64];
    gen_wire_id(wire_id, sizeof(wire_id));

    cJSON *resp = broker_request(
        sm_msg_incidents_request(wire_id, since_ts),
        wire_id, 10000);
    if (!resp) return strdup("[ERROR] timeout");

    cJSON *incidents = cJSON_GetObjectItemCaseSensitive(resp, "incidents");
    if (!cJSON_IsArray(incidents) || cJSON_GetArraySize(incidents) == 0) {
        cJSON_Delete(resp);
        return strdup("No anomalies detected.");
    }

    /* Build report with sm_strbuf — a fixed count*512 budget underflowed
     * once one incident's match_text + pre_context pushed the accumulated
     * snprintf return values past the cap, corrupting the heap. */
    sm_strbuf_t sb;
    sm_strbuf_init(&sb);

    int num = 0;
    cJSON *inc;
    cJSON_ArrayForEach(inc, incidents) {
        num++;
        const char *pname = sm_json_get_string(inc, "pattern_name");
        const char *sev = sm_json_get_string(inc, "severity");
        const char *match = sm_json_get_string(inc, "match_text");
        const char *pre = sm_json_get_string(inc, "pre_context");

        sm_strbuf_printf(&sb,
            "### Incident %d: %s [%s]\nMatch: %s\n",
            num, pname ? pname : "?", sev ? sev : "?",
            match ? match : "");
        if (pre && pre[0]) {
            sm_strbuf_printf(&sb,
                "Pre-context:\n```\n%s\n```\n", pre);
        }
        sm_strbuf_printf(&sb, "\n");
    }
    cJSON_Delete(resp);

    char *out = sm_strbuf_steal(&sb);
    return out ? out : strdup("(allocation failed)");
}

static char *tool_serial_add_watchdog(cJSON *args)
{
    const char *name = sm_json_get_string(args, "name");
    const char *pattern = sm_json_get_string(args, "pattern");
    const char *severity = sm_json_get_string(args, "severity");
    if (!severity) severity = "warning";
    if (!name || !pattern) return strdup("[ERROR] missing name or pattern");

    char wire_id[64];
    gen_wire_id(wire_id, sizeof(wire_id));

    cJSON *patterns = cJSON_CreateArray();
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "name", name);
    cJSON_AddStringToObject(p, "pattern", pattern);
    cJSON_AddStringToObject(p, "severity", severity);
    cJSON_AddItemToArray(patterns, p);

    cJSON *resp = broker_request(
        sm_msg_configure_anomaly(wire_id, patterns),
        wire_id, 5000);
    if (!resp) return strdup("[ERROR] timeout");

    const char *resp_type = sm_json_get_string(resp, "type");
    if (resp_type && strcmp(resp_type, "error") == 0) {
        const char *emsg = sm_json_get_string(resp, "message");
        char err[512];
        snprintf(err, sizeof(err), "[ERROR] %s",
                 emsg ? emsg : "failed to add pattern");
        cJSON_Delete(resp);
        return strdup(err);
    }
    cJSON_Delete(resp);

    char result[256];
    snprintf(result, sizeof(result),
             "Watchdog '%s' added (severity=%s, pattern=%s)",
             name, severity, pattern);
    return strdup(result);
}

static char *tool_serial_monitor(cJSON *args)
{
    int duration = sm_json_get_int(args, "duration_seconds", 30);
    if (duration > 300) duration = 300;
    if (duration < 1) duration = 1;

    double monitor_start = sm_now_realtime();

    /* Send send_expect with never-matching pattern */
    char wire_id[64];
    gen_wire_id(wire_id, sizeof(wire_id));

    int timeout_ms = duration * 1000;
    cJSON *msg = sm_msg_send_expect(wire_id, NULL, 0, "^\\b$", timeout_ms);
    cJSON *resp = broker_request(msg, wire_id, timeout_ms + 5000);

    char *output_text = NULL;
    if (resp) {
        const char *resp_type = sm_json_get_string(resp, "type");
        if (resp_type && strcmp(resp_type, "error") == 0) {
            const char *emsg = sm_json_get_string(resp, "message");
            char err[512];
            snprintf(err, sizeof(err), "[ERROR] %s",
                     emsg ? emsg : "monitor failed");
            cJSON_Delete(resp);
            return strdup(err);
        }

        const char *data_b64 = sm_json_get_string(resp, "data");
        if (data_b64 && data_b64[0]) {
            size_t dec_len;
            uint8_t *data = sm_base64_decode(data_b64, strlen(data_b64), &dec_len);
            if (data) {
                output_text = malloc(dec_len + 1);
                if (output_text) {
                    memcpy(output_text, data, dec_len);
                    output_text[dec_len] = '\0';
                }
                free(data);
            }
        }
        cJSON_Delete(resp);
    }
    if (!output_text) output_text = strdup("(no output)");

    /* Get incidents during monitoring window */
    char wire_id2[64];
    gen_wire_id(wire_id2, sizeof(wire_id2));
    cJSON *inc_resp = broker_request(
        sm_msg_incidents_request(wire_id2, monitor_start),
        wire_id2, 5000);

    /* Build report with sm_strbuf — the previous strlen+4096 budget
     * underflowed once accumulated incident lines pushed the snprintf offset
     * past the cap, writing past the heap allocation. */
    sm_strbuf_t sb;
    sm_strbuf_init(&sb);
    sm_strbuf_printf(&sb,
        "## Serial Monitor Report\n\nOutput:\n```\n%s\n```\n",
        output_text);
    free(output_text);

    if (inc_resp) {
        cJSON *incidents = cJSON_GetObjectItemCaseSensitive(inc_resp, "incidents");
        int inc_found = 0;
        if (cJSON_IsArray(incidents)) {
            cJSON *inc;
            cJSON_ArrayForEach(inc, incidents) {
                if (!inc_found) {
                    sm_strbuf_append_str(&sb, "\n## Anomalies Detected\n");
                    inc_found = 1;
                }
                const char *pname = sm_json_get_string(inc, "pattern_name");
                const char *sev = sm_json_get_string(inc, "severity");
                const char *match = sm_json_get_string(inc, "match_text");
                sm_strbuf_printf(&sb,
                    "- **%s** [%s]: %s\n",
                    pname ? pname : "?", sev ? sev : "?",
                    match ? match : "");
            }
        }
        if (!inc_found) {
            sm_strbuf_append_str(&sb,
                "\nNo anomalies detected during monitoring.\n");
        }
        cJSON_Delete(inc_resp);
    }

    char *out = sm_strbuf_steal(&sb);
    return out ? out : strdup("(allocation failed)");
}

static char *tool_serial_generate_report(void)
{
    /* 1. Get status */
    char wid1[64];
    gen_wire_id(wid1, sizeof(wid1));
    cJSON *status = broker_request(sm_msg_status(wid1), wid1, 5000);

    /* 2. Get incidents */
    char wid2[64];
    gen_wire_id(wid2, sizeof(wid2));
    cJSON *inc_resp = broker_request(
        sm_msg_incidents_request(wid2, 0.0), wid2, 5000);

    /* 3. Get history (last 32KB) */
    char wid3[64];
    gen_wire_id(wid3, sizeof(wid3));
    cJSON *hist_resp = broker_request(
        sm_msg_history_request(wid3, 0.0, 32768), wid3, 10000);

    /* Growable report — fixed 8192 + off += snprintf overflowed on large
     * incident lists (M15 idiom; sink path already uses sm_strbuf). */
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
        ctx.profile.name, ctx.profile.device_type,
        ctx.profile.description, ctx.profile.prompt_pattern,
        ctx.profile.response_mode);
    if (ctx.profile.command_prefix[0])
        sm_strbuf_printf(&sb, "- Command prefix: `%s`\n",
                         ctx.profile.command_prefix);

    if (status) {
        sm_strbuf_printf(&sb,
            "\n## Port Status\n"
            "- Port: %s\n"
            "- Baud: %d\n"
            "- Connected: %s\n",
            sm_json_get_string(status, "port") ?
                sm_json_get_string(status, "port") : "?",
            sm_json_get_int(status, "baud", 0),
            sm_json_get_bool(status, "connected", 0) ? "true" : "false");

        cJSON *clients = cJSON_GetObjectItemCaseSensitive(status, "clients");
        if (cJSON_IsArray(clients))
            sm_strbuf_printf(&sb, "- Clients: %d\n",
                             cJSON_GetArraySize(clients));
        cJSON_Delete(status);
    }

    if (inc_resp) {
        cJSON *incidents = cJSON_GetObjectItemCaseSensitive(inc_resp, "incidents");
        if (cJSON_IsArray(incidents) && cJSON_GetArraySize(incidents) > 0) {
            sm_strbuf_printf(&sb, "\n## Incidents (%d total)\n",
                             cJSON_GetArraySize(incidents));
            cJSON *inc;
            cJSON_ArrayForEach(inc, incidents) {
                const char *pname = sm_json_get_string(inc, "pattern_name");
                const char *sev = sm_json_get_string(inc, "severity");
                const char *match = sm_json_get_string(inc, "match_text");
                sm_strbuf_printf(&sb, "- **%s** [%s]: %s\n",
                                 pname ? pname : "?", sev ? sev : "?",
                                 match ? match : "");
            }
        } else {
            sm_strbuf_printf(&sb, "\n## Incidents\nNone detected.\n");
        }
        cJSON_Delete(inc_resp);
    }

    sm_strbuf_printf(&sb, "\n## Recent Output (last 32KB)\n");
    if (hist_resp) {
        cJSON *chunks = cJSON_GetObjectItemCaseSensitive(hist_resp, "chunks");
        if (cJSON_IsArray(chunks) && cJSON_GetArraySize(chunks) > 0) {
            sm_strbuf_printf(&sb, "```\n");
            cJSON *chunk;
            cJSON_ArrayForEach(chunk, chunks) {
                const char *b64 = sm_json_get_string(chunk, "data");
                if (!b64) continue;
                size_t dec_len;
                uint8_t *data = sm_base64_decode(b64, strlen(b64), &dec_len);
                if (!data) continue;
                size_t start = 0;
                for (size_t j = 0; j <= dec_len; j++) {
                    if (j == dec_len || data[j] == 0) {
                        if (j > start)
                            sm_strbuf_append(&sb, (const char *)data + start,
                                             j - start);
                        start = j + 1;
                    }
                }
                free(data);
            }
            sm_strbuf_printf(&sb, "\n```\n");
        } else {
            sm_strbuf_printf(&sb, "(no output)\n");
        }
        cJSON_Delete(hist_resp);
    }
    return sm_strbuf_steal(&sb);
}

static char *tool_serial_list_ports(void)
{
    return sm_format_serial_ports_text();
}

/* --- Tool dispatcher --- */

static int try_connect_broker(void); /* forward */

static char *dispatch_tool(const char *name, cJSON *args, cJSON *jsonrpc_id)
{
    (void)jsonrpc_id;

    /* list_ports works offline (local sysfs only). */
    if (strcmp(name, "serial_list_ports") != 0) {
        if (ctx.conn.fd < 0 || !ctx.conn.running) {
            if (try_connect_broker() != 0) {
                /* Prefer the broker's own words ("authentication failed")
                 * over the generic "no broker" guidance — the two call for
                 * completely different fixes. */
                if (ctx.offline_reason[0])
                    return sm_mcp_error_with_hint(ctx.offline_reason);
                return sm_mcp_offline_broker_message();
            }
        }
    }

    if (sm_mcp_tool_is_mutate(name) && !sm_mcp_mutate_enabled())
        return strdup("[ERROR] mutate tools disabled (set SMOLMUX_MCP_MUTATE=1)");

    char *result = NULL;
    if (strcmp(name, "serial_send_command") == 0) result = tool_serial_send_command(args);
    else if (strcmp(name, "serial_read") == 0)         result = tool_serial_read();
    else if (strcmp(name, "serial_write") == 0)         result = tool_serial_write(args);
    else if (strcmp(name, "serial_port_status") == 0)   result = tool_serial_port_status();
    else if (strcmp(name, "serial_boot_status") == 0)    result = tool_serial_boot_status();
    else if (strcmp(name, "serial_add_autoresponder") == 0)
        result = tool_serial_add_autoresponder(args);
    else if (strcmp(name, "serial_pin_control") == 0)   result = tool_serial_pin_control(args);
    else if (strcmp(name, "serial_sysrq") == 0)         result = tool_serial_sysrq(args);
    else if (strcmp(name, "serial_suspend") == 0)        result = tool_serial_suspend();
    else if (strcmp(name, "serial_resume") == 0)         result = tool_serial_resume();
    else if (strcmp(name, "serial_wait_for") == 0)       result = tool_serial_wait_for(args);
    else if (strcmp(name, "serial_output_history") == 0) result = tool_serial_output_history(args);
    else if (strcmp(name, "serial_get_incidents") == 0)  result = tool_serial_get_incidents(args);
    else if (strcmp(name, "serial_add_watchdog") == 0)   result = tool_serial_add_watchdog(args);
    else if (strcmp(name, "serial_monitor") == 0)        result = tool_serial_monitor(args);
    else if (strcmp(name, "serial_generate_report") == 0) result = tool_serial_generate_report();
    else if (strcmp(name, "serial_list_ports") == 0)     result = tool_serial_list_ports();
    else {
        char err[256];
        snprintf(err, sizeof(err), "[ERROR] unknown tool: %s", name);
        result = sm_mcp_error_with_hint(err);
    }
    if (result)
        result = sm_mcp_maybe_explain_result(result);
    return result;
}

/* --- MCP message dispatch --- */

static void handle_initialize(cJSON *id)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "protocolVersion", SM_MCP_PROTOCOL_VERSION);
    cJSON *caps = cJSON_CreateObject();
    cJSON_AddItemToObject(caps, "tools", cJSON_CreateObject());
    cJSON_AddItemToObject(caps, "prompts", cJSON_CreateObject());
    cJSON_AddItemToObject(result, "capabilities", caps);
    cJSON *info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "name", SM_NAME "-mcp");
    cJSON_AddStringToObject(info, "version", SM_VERSION);
    cJSON_AddItemToObject(result, "serverInfo", info);
    cJSON_AddStringToObject(result, "instructions",
                            sm_mcp_serial_instructions());
    mcp_send_result(id, result);
    ctx.initialized = 1;
    SM_LOG_INFO(LOG_TAG, "MCP initialized");
}

static void handle_prompts_list(cJSON *id)
{
    mcp_send_result(id, sm_mcp_serial_prompts_list_result());
}

static void handle_prompts_get(cJSON *id, cJSON *params)
{
    cJSON *name = params
        ? cJSON_GetObjectItemCaseSensitive(params, "name") : NULL;
    if (!cJSON_IsString(name) || !name->valuestring[0]) {
        mcp_send_error(id, -32602, "missing prompt name");
        return;
    }

    cJSON *args = params
        ? cJSON_GetObjectItemCaseSensitive(params, "arguments") : NULL;
    char *text = NULL;
    const char *desc = NULL;
    if (sm_mcp_serial_prompt_get(name->valuestring, args, &text, &desc) != 0) {
        mcp_send_error(id, -32602, "unknown prompt");
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "description", desc ? desc : "");
    cJSON *msgs = cJSON_AddArrayToObject(result, "messages");
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    cJSON *content = cJSON_CreateObject();
    cJSON_AddStringToObject(content, "type", "text");
    cJSON_AddStringToObject(content, "text", text ? text : "");
    cJSON_AddItemToObject(m, "content", content);
    cJSON_AddItemToArray(msgs, m);
    free(text);
    mcp_send_result(id, result);
}

static void handle_tools_list(cJSON *id)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "tools", sm_mcp_build_tools_list());
    mcp_send_result(id, result);
}

static void handle_tools_call(cJSON *id, cJSON *params)
{
    cJSON *name_item = cJSON_GetObjectItemCaseSensitive(params, "name");
    if (!cJSON_IsString(name_item)) {
        mcp_send_error(id, -32602, "missing tool name");
        return;
    }

    cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
    cJSON *args_tmp = NULL;
    if (!args) {
        args_tmp = cJSON_CreateObject();
        args = args_tmp;
    }

    char *result = dispatch_tool(name_item->valuestring, args, id);
    if (result) {
        mcp_send_tool_result(id, result);
        free(result);
    }
    cJSON_Delete(args_tmp);
}

static void dispatch_mcp_message(cJSON *msg)
{
    cJSON *method = cJSON_GetObjectItemCaseSensitive(msg, "method");
    cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");

    if (!cJSON_IsString(method)) {
        if (id) mcp_send_error(id, -32600, "invalid request: missing method");
        return;
    }

    const char *m = method->valuestring;

    if (strcmp(m, "initialize") == 0)
        handle_initialize(id);
    else if (strcmp(m, "notifications/initialized") == 0)
        { /* no response */ }
    else if (strcmp(m, "tools/list") == 0)
        handle_tools_list(id);
    else if (strcmp(m, "tools/call") == 0) {
        cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");
        cJSON *params_tmp = NULL;
        if (!params) { params_tmp = cJSON_CreateObject(); params = params_tmp; }
        handle_tools_call(id, params);
        cJSON_Delete(params_tmp);
    } else if (strcmp(m, "prompts/list") == 0) {
        handle_prompts_list(id);
    } else if (strcmp(m, "prompts/get") == 0) {
        cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");
        handle_prompts_get(id, params);
    } else {
        if (id) mcp_send_error(id, -32601, "method not found");
    }
}

/* --- I/O handlers --- */

static void handle_stdin_data(void)
{
    if (ctx.stdin_len >= ctx.stdin_cap - 1) {
        if (ctx.stdin_cap >= SM_MCP_READ_BUF_MAX) {
            SM_LOG_WARN(LOG_TAG, "stdin buffer overflow, discarding");
            ctx.stdin_len = 0;
            return;
        }
        size_t new_cap = ctx.stdin_cap * 2;
        if (new_cap > SM_MCP_READ_BUF_MAX) new_cap = SM_MCP_READ_BUF_MAX;
        void *tmp = realloc(ctx.stdin_buf, new_cap);
        if (!tmp) return;
        ctx.stdin_buf = tmp;
        ctx.stdin_cap = new_cap;
    }

    ssize_t n = read(STDIN_FILENO, ctx.stdin_buf + ctx.stdin_len,
                     ctx.stdin_cap - ctx.stdin_len - 1);
    if (n <= 0) {
        if (n == 0) {
            SM_LOG_INFO(LOG_TAG, "stdin closed");
            ctx.conn.running = 0;
        }
        return;
    }
    ctx.stdin_len += (size_t)n;

    /* Process complete lines */
    while (1) {
        char *nl = memchr(ctx.stdin_buf, '\n', ctx.stdin_len);
        if (!nl) break;

        size_t line_len = (size_t)(nl - ctx.stdin_buf);
        ctx.stdin_buf[line_len] = '\0';

        cJSON *msg = cJSON_Parse(ctx.stdin_buf);
        if (msg) {
            dispatch_mcp_message(msg);
            cJSON_Delete(msg);
        } else {
            SM_LOG_WARN(LOG_TAG, "malformed JSON on stdin");
            mcp_send_error(NULL, -32700, "parse error");
        }

        size_t remaining = ctx.stdin_len - line_len - 1;
        memmove(ctx.stdin_buf, nl + 1, remaining);
        ctx.stdin_len = remaining;
    }
}

/* --- Signal handling --- */

static void signal_handler(int sig)
{
    (void)sig;
    ctx.conn.running = 0;
}

/* --- Profile discovery --- */

/* Returns 0 on success, -1 if explicit/env profile cannot be resolved. */
static int discover_profile(const char *profile_path)
{
    char resolved[512];
    sm_profile_init_default(&ctx.profile);

    if (profile_path && profile_path[0]) {
        if (sm_profile_resolve_path(profile_path, SM_PROFILE_FILE_SUFFIX,
                                    resolved, sizeof(resolved)) != 0) {
            SM_LOG_ERROR(LOG_TAG, "failed to resolve profile '%s'", profile_path);
            return -1;
        }
        if (sm_profile_load(&ctx.profile, resolved) != 0) {
            SM_LOG_ERROR(LOG_TAG, "failed to load profile: %s", resolved);
            return -1;
        }
        SM_LOG_INFO(LOG_TAG, "loaded profile: %s", resolved);
        return 0;
    }

    const char *env = getenv(SM_PROFILE_ENV);
    if (env && env[0]) {
        if (sm_profile_resolve_path(env, SM_PROFILE_FILE_SUFFIX,
                                    resolved, sizeof(resolved)) != 0) {
            SM_LOG_ERROR(LOG_TAG, "failed to resolve $%s=%s", SM_PROFILE_ENV, env);
            return -1;
        }
        if (sm_profile_load(&ctx.profile, resolved) != 0) {
            SM_LOG_ERROR(LOG_TAG, "failed to load profile from env: %s", resolved);
            return -1;
        }
        SM_LOG_INFO(LOG_TAG, "loaded profile from env: %s", resolved);
        return 0;
    }

    SM_LOG_INFO(LOG_TAG, "using default profile");
    return 0;
}

/* Send profile anomaly patterns to broker */
static void configure_broker_anomalies(void)
{
    if (ctx.profile.anomaly_count == 0) return;

    cJSON *patterns = cJSON_CreateArray();
    for (size_t i = 0; i < ctx.profile.anomaly_count; i++) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "name", ctx.profile.anomaly_patterns[i].name);
        cJSON_AddStringToObject(p, "pattern", ctx.profile.anomaly_patterns[i].pattern);
        cJSON_AddStringToObject(p, "severity", ctx.profile.anomaly_patterns[i].severity);
        cJSON_AddItemToArray(patterns, p);
    }

    char wire_id[64];
    gen_wire_id(wire_id, sizeof(wire_id));
    cJSON *resp = broker_request(
        sm_msg_configure_anomaly(wire_id, patterns),
        wire_id, 5000);
    if (resp) {
        int added = sm_json_get_int(resp, "patterns_added", 0);
        SM_LOG_INFO(LOG_TAG, "configured %d anomaly patterns on broker", added);
        cJSON_Delete(resp);
    }
}

/* --- Main --- */

/* Record why we are offline, so the next tool call can tell the agent.
 *
 * fd < 0 is what marks "not connected" — dispatch_tool() retries on it. Note
 * conn.running does double duty: sm_broker_conn_read() clears it at socket
 * EOF, but the main loop also uses it as its own keep-running condition, so
 * leaving it clear here would exit the whole MCP server instead of soft-
 * failing the way an unreachable broker does. Restore it deliberately. */
static void set_offline(const char *reason)
{
    ctx.offline = 1;
    snprintf(ctx.offline_reason, sizeof(ctx.offline_reason), "%s",
             reason ? reason : "");
    if (ctx.conn.fd >= 0) {
        close(ctx.conn.fd);
        ctx.conn.fd = -1;
    }
    ctx.conn.running = 1;
}

/* Connect (or reconnect) to broker. Never auto-spawns. Returns 0 on success. */
static int try_connect_broker(void)
{
    if (ctx.conn.fd >= 0) {
        close(ctx.conn.fd);
        ctx.conn.fd = -1;
    }
    ctx.conn.running = 1;
    ctx.offline_reason[0] = '\0';

    if (ctx.use_tcp) {
        ctx.conn.fd = sm_connect_tcp(ctx.tcp_host, ctx.tcp_port);
        if (ctx.conn.fd < 0) {
            ctx.offline = 1;
            return -1;
        }
        SM_LOG_INFO(LOG_TAG, "connected to broker via TCP %s:%d",
                    ctx.tcp_host, ctx.tcp_port);
    } else {
        if (!ctx.socket_path[0]) {
            /* Several sockets: first-glob is a magnet (desk: 20 controllers
             * on the first by-id board, 0 on the other). Pin -s. */
            int env_pin = getenv(SM_SOCKET_ENV) && getenv(SM_SOCKET_ENV)[0];
            if (sm_autodiscover_should_refuse(ctx.socket_explicit || env_pin)) {
                cJSON *err = sm_identity_ambiguous_json("", "",
                    "multiple brokers; pass -s to pin a socket");
                char *s = cJSON_PrintUnformatted(err);
                snprintf(ctx.offline_reason, sizeof(ctx.offline_reason),
                         "%s", s ? s : "{\"error\":\"identity_ambiguous\"}");
                free(s);
                cJSON_Delete(err);
                ctx.offline = 1;
                return -1;
            }
            char discovered[SM_SOCK_PATH_MAX];
            if (sm_discover_socket(discovered, sizeof(discovered)) == 0)
                snprintf(ctx.socket_path, sizeof(ctx.socket_path), "%s",
                         discovered);
            else {
                ctx.offline = 1;
                return -1;
            }
        }
        ctx.conn.fd = sm_connect_unix(ctx.socket_path);
        if (ctx.conn.fd < 0) {
            ctx.offline = 1;
            return -1;
        }
        SM_LOG_INFO(LOG_TAG, "connected to broker via %s", ctx.socket_path);
    }

    /* The handshake reply decides whether we are actually usable. Treating
     * "no welcome" as success left the server marked online on top of a
     * socket the broker had already dropped, so every later tool call died
     * with no JSON-RPC answer at all and the operator got no hint that a
     * token was wrong. */
    broker_send(sm_msg_hello(ctx.name, "controller"));
    cJSON *reply = wait_for_response(NULL, 5000);

    if (!reply) {
        SM_LOG_WARN(LOG_TAG, "no reply to hello from broker");
        set_offline("[ERROR] broker did not answer the hello handshake");
        return -1;
    }

    const char *reply_type = sm_json_get_string(reply, "type");
    if (!reply_type || strcmp(reply_type, "welcome") != 0) {
        const char *why = sm_json_get_string(reply, "message");
        char reason[192];
        snprintf(reason, sizeof(reason), "[ERROR] broker rejected this client: %s",
                 why && why[0] ? why : "handshake refused");
        SM_LOG_ERROR(LOG_TAG, "%s", reason);
        cJSON_Delete(reply);
        set_offline(reason);
        return -1;
    }

    const char *port = sm_json_get_string(reply, "port");
    int baud = sm_json_get_int(reply, "baud", 0);
    SM_LOG_INFO(LOG_TAG, "broker: port=%s baud=%d", port ? port : "?", baud);
    cJSON_Delete(reply);

    configure_broker_anomalies();
    ctx.offline = 0;
    ctx.offline_reason[0] = '\0';
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options] [socket_path]\n"
        "\n"
        "Standalone MCP server — connects to a running smolmux broker\n"
        "and exposes MCP tools over stdio JSON-RPC.\n"
        "If the broker is down, MCP still starts; tools return offline\n"
        "guidance (never auto-spawns a broker).\n"
        "\n"
        "Options:\n"
        "  -s, --socket <path>    Broker socket path (auto-discovered)\n"
        "  --tcp <host:port>      Connect via TCP instead\n"
        "  -p, --profile <path>   Device profile JSON\n"
        "  -n, --name <name>      Client name (default: claude-mcp)\n"
        "  -v, --verbose          Debug logging\n"
        "  -V, --version          Show version\n"
        "  -h, --help             Show help\n",
        prog);
}

int main(int argc, char *argv[])
{
    const char *socket_path = NULL;
    const char *tcp_target = NULL;
    const char *profile_path = NULL;

    memset(&ctx, 0, sizeof(ctx));
    snprintf(ctx.name, sizeof(ctx.name), "claude-mcp");

    static const struct option long_opts[] = {
        {"socket",  required_argument, NULL, 's'},
        {"tcp",     required_argument, NULL, 'T'},
        {"profile", required_argument, NULL, 'p'},
        {"name",    required_argument, NULL, 'n'},
        {"verbose", no_argument,       NULL, 'v'},
        {"version", no_argument,       NULL, 'V'},
        {"help",    no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "s:p:n:vVh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 's':
            socket_path = optarg;
            break;
        case 'T':
            tcp_target = optarg;
            break;
        case 'p':
            profile_path = optarg;
            break;
        case 'n':
            snprintf(ctx.name, sizeof(ctx.name), "%s", optarg);
            break;
        case 'v':
            ctx.verbose = 1;
            break;
        case 'V':
            fprintf(stderr, "%s-mcp %s\n", SM_NAME, SM_VERSION);
            return 0;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    /* Set log level */
    if (ctx.verbose)
        sm_logger_set_level(SM_LOG_DEBUG);
    else
        sm_logger_set_level(SM_LOG_WARN);

    /* Allocate buffers + init the broker connection core */
    ctx.output_cap = SM_MCP_OUTPUT_BUFFER_MAX;
    ctx.output_buf = malloc(ctx.output_cap);
    ctx.stdin_cap = 8192;
    ctx.stdin_buf = malloc(ctx.stdin_cap);

    if (!ctx.output_buf || !ctx.stdin_buf ||
        sm_broker_conn_init(&ctx.conn, 8192) != 0) {
        fprintf(stderr, "Error: failed to allocate buffers\n");
        return 1;
    }
    ctx.conn.verbose = ctx.verbose;
    sm_broker_conn_set_output_cb(&ctx.conn, mcp_on_broker_output, NULL);
    sm_broker_conn_set_event_cb(&ctx.conn, mcp_on_broker_event, NULL);

    /* Load device profile */
    if (discover_profile(profile_path) != 0)
        return 1;

    /* Remember how to reach the broker (for soft-fail reconnect). */
    if (tcp_target) {
        ctx.use_tcp = 1;
        ctx.tcp_port = SM_TCP_DEFAULT_PORT;
        sm_parse_host_port(tcp_target, ctx.tcp_host, sizeof(ctx.tcp_host),
                           &ctx.tcp_port);
    } else {
        if (!socket_path && optind < argc)
            socket_path = argv[optind];
        if (socket_path) {
            snprintf(ctx.socket_path, sizeof(ctx.socket_path), "%s",
                     socket_path);
            ctx.socket_explicit = 1;
        }
    }

    /* Soft-fail: enter MCP even if broker is down (never auto-spawn). */
    if (try_connect_broker() != 0) {
        fprintf(stderr,
                "Warning: broker not available — MCP online; tools return "
                "offline guidance until a broker starts\n");
        ctx.conn.running = 1; /* still serve stdio MCP */
        ctx.offline = 1;
    }

    /* Signal handling */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* Main event loop — poll stdin always; broker fd only when connected. */
    while (ctx.conn.running) {
        struct pollfd fds[2];
        int nfds = 1;
        fds[0].fd = STDIN_FILENO;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        if (ctx.conn.fd >= 0) {
            fds[1].fd = ctx.conn.fd;
            fds[1].events = POLLIN;
            fds[1].revents = 0;
            nfds = 2;
        }

        int ret = poll(fds, nfds, ctx.conn.fd >= 0 ? -1 : 500);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (nfds == 2 && (fds[1].revents & POLLIN)) {
            cJSON *stray = read_broker(NULL);
            if (stray) cJSON_Delete(stray);
        }
        if (nfds == 2 && (fds[1].revents & (POLLHUP | POLLERR))) {
            SM_LOG_INFO(LOG_TAG, "broker disconnected (will soft-fail tools)");
            close(ctx.conn.fd);
            ctx.conn.fd = -1;
            ctx.offline = 1;
        }

        if (fds[0].revents & POLLIN)
            handle_stdin_data();
        else if (fds[0].revents & (POLLHUP | POLLERR)) {
            SM_LOG_INFO(LOG_TAG, "stdin closed");
            ctx.conn.running = 0;
        }
    }

    /* Cleanup */
    sm_broker_conn_destroy(&ctx.conn);
    free(ctx.output_buf);
    free(ctx.stdin_buf);
    sm_profile_destroy(&ctx.profile);

    return 0;
}
