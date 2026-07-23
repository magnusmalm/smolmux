#include "sinks/mcp.h"
#include "sinks/mcp_internal.h"
#include "broker.h"
#include "logger.h"
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
    if (b->profile.command_prefix[0]) {
        snprintf(cmd_buf, sizeof(cmd_buf), "%s%s", b->profile.command_prefix, command);
    } else {
        snprintf(cmd_buf, sizeof(cmd_buf), "%s", command);
    }

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

    /* Append newline if missing */
    size_t cmd_len = strlen(cmd_buf);
    if (cmd_len == 0 || cmd_buf[cmd_len - 1] != '\n') {
        if (cmd_len + 1 < sizeof(cmd_buf)) {
            cmd_buf[cmd_len] = '\n';
            cmd_buf[cmd_len + 1] = '\0';
            cmd_len++;
        }
    }

    /* Check link status */
    if (b->suspended) return strdup("[ERROR] serial port is suspended");
    if (b->link_disconnected) return strdup("[ERROR] serial port disconnected");
    if (b->link->read_fd(b->link) < 0) return strdup("[ERROR] serial port not connected");

    /* Generate expect id and register */
    char expect_id[16];
    mcp_gen_expect_id(mcp, expect_id, sizeof(expect_id));

    double timeout_s = (double)timeout_ms / 1000.0;
    if (sm_expect_add(&b->expect, expect_id, pattern, timeout_s,
                       SM_MCP_CLIENT_ID) != 0) {
        return strdup("[ERROR] invalid regex pattern");
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
    return mcp_drain_output(mcp);
}

static char *tool_serial_write(sm_mcp_sink_t *mcp, cJSON *args)
{
    sm_broker_t *b = mcp->broker;
    const char *data_str = sm_json_get_string(args, "data");
    if (!data_str) return strdup("[ERROR] missing 'data' argument");

    size_t len = strlen(data_str);
    int rc = sm_broker_do_write(b, (const uint8_t *)data_str, len, "mcp");
    if (rc == -1) return strdup("[ERROR] serial port is suspended");
    if (rc == -2) return strdup("[ERROR] serial port disconnected");
    if (rc == -3) return strdup("[ERROR] write failed");

    double ts = sm_now_realtime();
    cJSON *echo = sm_msg_input_echo((const uint8_t *)data_str, len, "mcp", ts);
    sm_broker_broadcast_msg(b, echo);
    cJSON_Delete(echo);

    return strdup("OK");
}

static char *tool_serial_port_status(sm_mcp_sink_t *mcp)
{
    sm_broker_t *b = mcp->broker;

    cJSON *status_json = cJSON_CreateObject();
    b->link->get_status(b->link, status_json);

    /* sm_strbuf — the old fixed char[4096] with off += snprintf(buf+off,
     * sizeof-off, ...) shared the M15 underflow idiom: unusually long client
     * names / pin JSON could push off past 4096 and write out of bounds. */
    sm_strbuf_t sb;
    sm_strbuf_init(&sb);
    sm_strbuf_printf(&sb,
        "Port: %s\nBaud: %d\nConnected: %s\nSuspended: %s\n",
        b->port, b->baudrate,
        b->link->read_fd(b->link) >= 0 ? "true" : "false",
        b->suspended ? "true" : "false");

    /* Pin states */
    cJSON *pins = cJSON_GetObjectItemCaseSensitive(status_json, "pin_states");
    if (pins) {
        char *pin_str = cJSON_PrintUnformatted(pins);
        sm_strbuf_printf(&sb, "Pin states: %s\n", pin_str ? pin_str : "{}");
        free(pin_str);
    }

    /* Takeover */
    if (b->takeover_client)
        sm_strbuf_printf(&sb, "Takeover: %s\n", b->takeover_client->name);
    else
        sm_strbuf_printf(&sb, "Takeover: none\n");

    /* Clients */
    sm_strbuf_printf(&sb, "Clients:\n");
    for (size_t i = 0; i < b->client_count; i++) {
        sm_strbuf_printf(&sb, "  - %s (%s)\n",
                         b->clients[i]->name, b->clients[i]->role);
    }

    cJSON_Delete(status_json);
    char *out = sm_strbuf_steal(&sb);
    return out ? out : strdup("(allocation failed)");
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

static char *tool_serial_boot_status(sm_mcp_sink_t *mcp)
{
    sm_broker_t *b = mcp->broker;

    if (b->boot.stage_count == 0)
        return strdup("No boot_stages defined in the device profile — "
                      "boot progress tracking is not configured.");

    int furthest = b->boot.furthest;
    int total = (int)b->boot.stage_count;
    int stalled = sm_boot_stalled(&b->boot, sm_now_realtime());
    int done = sm_boot_terminal_reached(&b->boot);

    int reached = 0;
    for (size_t i = 0; i < b->boot.stage_count; i++)
        if (b->boot.stages[i].reached) reached++;
    const char *fname = (furthest >= 0 && (size_t)furthest < b->boot.stage_count)
                        ? b->boot.stages[furthest].name : NULL;
    const char *state = done ? "complete" : (stalled ? "STALLED"
                      : (furthest < 0 ? "not started" : "in progress"));

    sm_strbuf_t sb;
    sm_strbuf_init(&sb);
    sm_strbuf_printf(&sb, "Boot: %d/%d stages reached", reached, total);
    if (fname) sm_strbuf_printf(&sb, " (furthest: %s)", fname);
    sm_strbuf_printf(&sb, ", state: %s\n", state);
    for (size_t i = 0; i < b->boot.stage_count; i++) {
        sm_boot_stage_t *st = &b->boot.stages[i];
        sm_strbuf_printf(&sb, "  [%c] %s%s\n", st->reached ? 'x' : ' ',
                         st->name, (int)i == furthest ? "  <- furthest" : "");
    }

    char *out = sm_strbuf_steal(&sb);
    return out ? out : strdup("(allocation failed)");
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

static char *tool_serial_pin_control(sm_mcp_sink_t *mcp, cJSON *args,
                                      cJSON *jsonrpc_id)
{
    sm_broker_t *b = mcp->broker;
    const char *pin = sm_json_get_string(args, "pin");
    const char *action = sm_json_get_string(args, "action");
    int duration_ms = sm_json_get_int(args, "duration_ms", 250);

    if (!pin || !action)
        return strdup("[ERROR] missing pin or action");

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
    if (rc < 0) return strdup("[ERROR] already suspended");

    char result[512];
    snprintf(result, sizeof(result),
             "OK - serial port %s released. Use serial_resume() when done.", b->port);
    return strdup(result);
}

static char *tool_serial_resume(sm_mcp_sink_t *mcp)
{
    sm_broker_t *b = mcp->broker;

    int rc = sm_broker_do_resume(b, "mcp");
    if (rc == -1) return strdup("[ERROR] not suspended");
    if (rc == -2) return strdup("[ERROR] failed to reopen serial port");

    return strdup("OK - serial port re-acquired.");
}

static char *tool_serial_output_history(sm_mcp_sink_t *mcp, cJSON *args)
{
    sm_broker_t *b = mcp->broker;
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
        return strdup("(no output in history)");
    }

    /* Concatenate all chunks */
    size_t total_len = 0;
    for (size_t i = 0; i < count; i++)
        total_len += chunks[i].len;

    char *text = malloc(total_len + 1);
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

    if (off == 0) {
        free(text);
        return strdup("(no output in history)");
    }
    return text;
}

static char *tool_serial_get_incidents(sm_mcp_sink_t *mcp, cJSON *args)
{
    sm_broker_t *b = mcp->broker;
    double seconds = sm_json_get_double(args, "seconds", 0.0);
    double since_ts = seconds > 0 ? sm_now_realtime() - seconds : 0.0;

    size_t count;
    const sm_anomaly_incident_t *incidents =
        sm_anomaly_get_incidents(&b->anomaly, &count);

    /* Count matching incidents */
    size_t matching = 0;
    for (size_t i = 0; i < count; i++) {
        if (since_ts > 0 && incidents[i].timestamp < since_ts) continue;
        matching++;
    }

    if (matching == 0) return strdup("No anomalies detected.");

    /* Build report with sm_strbuf — a fixed matching*512 budget underflowed
     * once one incident's match_text (256B) + pre_context (1024B) pushed the
     * accumulated snprintf return values past the cap, corrupting the heap. */
    sm_strbuf_t sb;
    sm_strbuf_init(&sb);

    int num = 0;
    for (size_t i = 0; i < count; i++) {
        if (since_ts > 0 && incidents[i].timestamp < since_ts) continue;
        num++;
        sm_strbuf_printf(&sb,
            "### Incident %d: %s [%s]\nMatch: %s\n",
            num, incidents[i].pattern_name, incidents[i].severity,
            incidents[i].match_text);
        if (incidents[i].pre_context[0]) {
            sm_strbuf_printf(&sb,
                "Pre-context:\n```\n%s\n```\n", incidents[i].pre_context);
        }
        sm_strbuf_printf(&sb, "\n");
    }

    char *out = sm_strbuf_steal(&sb);
    return out ? out : strdup("(allocation failed)");
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
    if (sm_expect_add(&b->expect, expect_id, "^\\b$", timeout_s,
                       SM_MCP_CLIENT_ID) != 0) {
        return strdup("[ERROR] failed to register monitor");
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

static char *tool_serial_list_ports(void)
{
    glob_t g;
    size_t total = sm_glob_serial_ports(&g);
    if (total == 0) {
        globfree(&g);
        return strdup("No serial ports found.");
    }

    sm_strbuf_t sb;
    sm_strbuf_init(&sb);
    for (size_t i = 0; i < total; i++) {
        if (i) sm_strbuf_append_str(&sb, "\n");
        sm_strbuf_append_str(&sb, g.gl_pathv[i]);
    }
    globfree(&g);

    char *out = sm_strbuf_steal(&sb);
    return out ? out : strdup("(allocation failed)");
}

/* --- Tool dispatcher --- */

char *mcp_tool_dispatch(sm_mcp_sink_t *mcp, const char *name, cJSON *args,
                         cJSON *jsonrpc_id)
{
    if (strcmp(name, "serial_send_command") == 0)
        return tool_serial_send_command(mcp, args, jsonrpc_id);
    if (strcmp(name, "serial_read") == 0)
        return tool_serial_read(mcp);
    if (strcmp(name, "serial_write") == 0)
        return tool_serial_write(mcp, args);
    if (strcmp(name, "serial_port_status") == 0)
        return tool_serial_port_status(mcp);
    if (strcmp(name, "serial_boot_status") == 0)
        return tool_serial_boot_status(mcp);
    if (strcmp(name, "serial_add_autoresponder") == 0)
        return tool_serial_add_autoresponder(mcp, args);
    if (strcmp(name, "serial_pin_control") == 0)
        return tool_serial_pin_control(mcp, args, jsonrpc_id);
    if (strcmp(name, "serial_sysrq") == 0)
        return tool_serial_sysrq(mcp, args, jsonrpc_id);
    if (strcmp(name, "serial_suspend") == 0)
        return tool_serial_suspend(mcp);
    if (strcmp(name, "serial_resume") == 0)
        return tool_serial_resume(mcp);
    if (strcmp(name, "serial_output_history") == 0)
        return tool_serial_output_history(mcp, args);
    if (strcmp(name, "serial_get_incidents") == 0)
        return tool_serial_get_incidents(mcp, args);
    if (strcmp(name, "serial_add_watchdog") == 0)
        return tool_serial_add_watchdog(mcp, args);
    if (strcmp(name, "serial_monitor") == 0)
        return tool_serial_monitor(mcp, args, jsonrpc_id);
    if (strcmp(name, "serial_generate_report") == 0)
        return tool_serial_generate_report(mcp);
    if (strcmp(name, "serial_list_ports") == 0)
        return tool_serial_list_ports();

    char err[256];
    snprintf(err, sizeof(err), "[ERROR] unknown tool: %s", name);
    return strdup(err);
}
