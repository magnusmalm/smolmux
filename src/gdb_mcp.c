/*
 * smolmux-gdb-mcp — standalone MCP server for GDB debugging over stdio
 *
 * Connects to a running smolmux broker that holds a GDB link (broker started
 * with --gdb) and exposes high-level GDB debugging tools over MCP JSON-RPC.
 * Exposes a high-level GDB tool surface (breakpoints, registers, memory,
 * Cortex-M fault analysis, target profiles, board probing).
 *
 * Architecture: the broker and its GDB link stay dumb byte pipes. This client
 * holds all the MI intelligence — it prefixes each GDB/MI command with a
 * numeric token, sends the bytes through the broker to gdb's stdin, and scans
 * the raw output stream (delivered as SM_MSG_OUTPUT) for the matching
 * `<token>^...` result record. Async records (*stopped, ~console) are captured
 * as they stream by. See src/util/mi_parse.c and src/gdb_profile.c.
 */

#include "constants.h"
#include "protocol.h"
#include "gdb_profile.h"
#include "mcp_broker_conn.h"
#include "util/mi_parse.h"
#include "util/json_helpers.h"
#include "util/sock_util.h"
#include "util/str.h"
#include "util/profile_resolve.h"
#include "logger.h"
#include "cJSON.h"

#include <errno.h>
#include <getopt.h>
#include <glob.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOG_TAG "gdb-mcp"

#define GDB_CONSOLE_MAX (256 * 1024)  /* cap buffered console text */

/* --- Global state --- */

static struct {
    sm_broker_conn_t conn;

    /* MI line assembler: raw output bytes accumulate here until '\n'. */
    sm_strbuf_t mi_buf;

    /* Pending MI command result correlation (by token). */
    unsigned next_token;
    unsigned pending_token;   /* 0 = none in flight */
    int      have_result;
    cJSON   *result;          /* owned: the results object of the matched record */
    char     result_class[32];

    /* Async execution state captured from *stopped / *running. */
    int  target_running;
    int  have_stopped;        /* set on *stopped, consumed by gdb_wait_stop */
    char stop_reason[64];
    char stop_func[128];
    char stop_file[256];
    char stop_line[16];
    char stop_signal[32];
    char stop_signal_meaning[160];

    /* Buffered console/stream/target text (drained by gdb_console_output). */
    sm_strbuf_t console;

    /* Stdin (MCP JSON-RPC) reassembly. */
    char  *stdin_buf;
    size_t stdin_len;
    size_t stdin_cap;

    sm_gdb_profile_t profile;
    char name[64];
    int  verbose;
    int  initialized;
} g;

/* Fetch a string field with a fallback (sm_json_get_string tolerates a NULL
 * object, so nested lookups need no separate null guard). Avoids the GNU ?:
 * elvis operator, which -Wpedantic rejects. */
static const char *jstr(const cJSON *obj, const char *key, const char *def)
{
    const char *s = sm_json_get_string(obj, key);
    return s ? s : def;
}

/* --- MCP stdio JSON-RPC output --- */

static void mcp_write_json(cJSON *msg)
{
    char *str = cJSON_PrintUnformatted(msg);
    if (!str) return;
    size_t len = strlen(str);
    char *buf = malloc(len + 2);
    if (!buf) { free(str); return; }
    memcpy(buf, str, len);
    buf[len] = '\n';
    size_t total = len + 1, written = 0;
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
    if (id) cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
    else    cJSON_AddNullToObject(resp, "id");
    cJSON_AddItemToObject(resp, "result", result);
    mcp_write_json(resp);
    cJSON_Delete(resp);
}

static void mcp_send_error(cJSON *id, int code, const char *message)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id) cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
    else    cJSON_AddNullToObject(resp, "id");
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

/* --- MI stream handling (runs inside the broker read via on_output) --- */

static void console_append(const char *s)
{
    if (!s) return;
    if (g.console.len > GDB_CONSOLE_MAX) {
        sm_strbuf_destroy(&g.console);
        sm_strbuf_init(&g.console);
    }
    sm_strbuf_append_str(&g.console, s);
}

static void handle_mi_record(sm_mi_record_t *rec)
{
    switch (rec->type) {
    case SM_MI_RESULT:
        if (g.pending_token != 0 && rec->token == (long)g.pending_token) {
            if (g.result) cJSON_Delete(g.result);
            g.result = rec->results;   /* take ownership */
            rec->results = NULL;
            snprintf(g.result_class, sizeof(g.result_class), "%s", rec->class_);
            g.have_result = 1;
            g.pending_token = 0;
        }
        break;
    case SM_MI_EXEC_ASYNC:
        if (strcmp(rec->class_, "stopped") == 0) {
            g.target_running = 0;
            g.have_stopped = 1;
            snprintf(g.stop_reason, sizeof(g.stop_reason), "%s",
                     jstr(rec->results, "reason", ""));
            cJSON *frame = cJSON_GetObjectItem(rec->results, "frame");
            snprintf(g.stop_func, sizeof(g.stop_func), "%s", jstr(frame, "func", ""));
            snprintf(g.stop_file, sizeof(g.stop_file), "%s", jstr(frame, "file", ""));
            snprintf(g.stop_line, sizeof(g.stop_line), "%s", jstr(frame, "line", ""));
            snprintf(g.stop_signal, sizeof(g.stop_signal), "%s",
                     jstr(rec->results, "signal-name", ""));
            snprintf(g.stop_signal_meaning, sizeof(g.stop_signal_meaning), "%s",
                     jstr(rec->results, "signal-meaning", ""));
        } else if (strcmp(rec->class_, "running") == 0) {
            g.target_running = 1;
        }
        break;
    case SM_MI_CONSOLE:
    case SM_MI_TARGET:
    case SM_MI_LOG:
        console_append(rec->stream_data);
        break;
    default:
        break;
    }
}

/* Broker output callback: assemble complete lines and dispatch MI records. A
 * line that is not a recognizable MI record is treated as raw target output
 * and appended to the console buffer. */
static void gdb_on_broker_output(void *user, const uint8_t *data, size_t len)
{
    (void)user;
    sm_strbuf_append(&g.mi_buf, (const char *)data, len);

    size_t start = 0;
    for (size_t i = 0; i < g.mi_buf.len; i++) {
        if (g.mi_buf.data[i] != '\n') continue;
        const char *line = g.mi_buf.data + start;
        size_t line_len = i - start;
        start = i + 1;

        sm_mi_record_t rec;
        if (sm_mi_parse_line(line, line_len, &rec) == 0) {
            if (rec.type != SM_MI_PROMPT)
                handle_mi_record(&rec);
            sm_mi_record_free(&rec);
        } else if (line_len > 0) {
            char *tmp = malloc(line_len + 1);
            if (tmp) {
                memcpy(tmp, line, line_len);
                tmp[line_len] = '\0';
                console_append(tmp);
                console_append("\n");
                free(tmp);
            }
        }
    }

    /* Retain any trailing partial line. */
    if (start > 0) {
        size_t remain = g.mi_buf.len - start;
        memmove(g.mi_buf.data, g.mi_buf.data + start, remain);
        g.mi_buf.len = remain;
    }
}

/* --- MI command / response --- */

static void deadline_from_now(struct timespec *dl, int timeout_ms)
{
    clock_gettime(CLOCK_MONOTONIC, dl);
    dl->tv_sec += timeout_ms / 1000;
    dl->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (dl->tv_nsec >= 1000000000L) { dl->tv_sec++; dl->tv_nsec -= 1000000000L; }
}

static int remaining_ms(const struct timespec *dl)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t ms = (dl->tv_sec - now.tv_sec) * 1000 +
                 (dl->tv_nsec - now.tv_nsec) / 1000000;
    if (ms < 0) return 0;
    return ms > INT32_MAX ? INT32_MAX : (int)ms;
}

/* Send an MI command (token-prefixed) and pump the output stream until its
 * result record arrives. Returns the results object (caller cJSON_Deletes),
 * with the result class copied to class_out; NULL on timeout/disconnect. */
static cJSON *gdb_mi_command(const char *mi_cmd, int timeout_ms,
                             char *class_out, size_t class_len)
{
    if (g.result) { cJSON_Delete(g.result); g.result = NULL; }
    g.have_result = 0;
    unsigned token = g.next_token++;
    if (g.next_token == 0) g.next_token = 1;
    g.pending_token = token;

    char wire_id[64];
    sm_broker_conn_gen_wire_id(&g.conn, wire_id, sizeof(wire_id));

    sm_strbuf_t sb;
    sm_strbuf_init(&sb);
    sm_strbuf_printf(&sb, "%u%s\n", token, mi_cmd);
    cJSON *msg = sm_msg_send(wire_id, (const uint8_t *)sb.data, sb.len);
    int rc = sm_broker_conn_send(&g.conn, msg);
    sm_strbuf_destroy(&sb);
    if (rc < 0) { g.pending_token = 0; return NULL; }

    struct timespec dl;
    deadline_from_now(&dl, timeout_ms);
    while (g.conn.running && !g.have_result) {
        int remain = remaining_ms(&dl);
        if (remain <= 0) break;
        if (sm_broker_conn_pump(&g.conn, remain) < 0) break;
    }
    g.pending_token = 0;
    if (!g.have_result) return NULL;

    if (class_out) snprintf(class_out, class_len, "%s", g.result_class);
    cJSON *res = g.result;
    g.result = NULL;
    g.have_result = 0;
    return res;
}

/* strdup-style formatted string (for tool return values). */
static char *fmtdup(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static char *fmtdup(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return strdup(buf);
}

/* Common failure text for a NULL/error MI result. Returns a strdup'd string, or
 * NULL if the result was fine (caller proceeds). Consumes err_class check. */
static char *mi_error_text(cJSON *result, const char *class_)
{
    if (!result)
        return strdup("[ERROR] no response from GDB (timeout or link down)");
    if (strcmp(class_, "error") == 0) {
        const char *msg = sm_json_get_string(result, "msg");
        return fmtdup("[ERROR] %s", msg ? msg : "GDB error");
    }
    return NULL;
}

/* --- Tools --- */

static char *tool_launch(cJSON *args)
{
    const char *exe = sm_json_get_string(args, "executable");
    const char *target = sm_json_get_string(args, "target");
    if (!exe) return strdup("[ERROR] missing 'executable'");

    char cls[32];
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "-file-exec-and-symbols \"%s\"", exe);
    cJSON *r = gdb_mi_command(cmd, 30000, cls, sizeof(cls));
    char *err = mi_error_text(r, cls);
    if (err) { if (r) cJSON_Delete(r); return err; }
    cJSON_Delete(r);

    sm_strbuf_t out;
    sm_strbuf_init(&out);
    sm_strbuf_printf(&out, "Loaded symbols: %s", exe);

    /* The broker owns the gdb process and its target connection; if a target
     * is given, (re)select it best-effort and report the outcome. */
    if (target && target[0]) {
        snprintf(cmd, sizeof(cmd), "-target-select %s", target);
        cJSON *tr = gdb_mi_command(cmd, 15000, cls, sizeof(cls));
        if (tr && strcmp(cls, "error") != 0)
            sm_strbuf_printf(&out, "\nConnected target: %s", target);
        else
            sm_strbuf_printf(&out, "\nTarget %s not (re)selected — the broker "
                             "already owns the connection", target);
        if (tr) cJSON_Delete(tr);
    }
    return sm_strbuf_steal(&out);
}

static char *tool_breakpoint(cJSON *args)
{
    const char *loc = sm_json_get_string(args, "location");
    const char *cond = sm_json_get_string(args, "condition");
    int temporary = sm_json_get_bool(args, "temporary", 0);
    if (!loc) return strdup("[ERROR] missing 'location'");

    char cmd[640];
    int n = snprintf(cmd, sizeof(cmd), "-break-insert");
    if (temporary) n += snprintf(cmd + n, sizeof(cmd) - n, " -t");
    if (cond && cond[0]) n += snprintf(cmd + n, sizeof(cmd) - n, " -c \"%s\"", cond);
    snprintf(cmd + n, sizeof(cmd) - n, " %s", loc);

    char cls[32];
    cJSON *r = gdb_mi_command(cmd, 5000, cls, sizeof(cls));
    char *err = mi_error_text(r, cls);
    if (err) { if (r) cJSON_Delete(r); return err; }

    cJSON *bkpt = cJSON_GetObjectItem(r, "bkpt");
    char *out = fmtdup("Breakpoint %s at %s",
                       jstr(bkpt, "number", "?"), jstr(bkpt, "addr", loc));
    cJSON_Delete(r);
    return out;
}

static char *tool_delete_breakpoint(cJSON *args)
{
    int number = sm_json_get_int(args, "number", -1);
    if (number < 0) return strdup("[ERROR] missing/invalid 'number'");
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "-break-delete %d", number);
    char cls[32];
    cJSON *r = gdb_mi_command(cmd, 5000, cls, sizeof(cls));
    char *err = mi_error_text(r, cls);
    if (err) { if (r) cJSON_Delete(r); return err; }
    cJSON_Delete(r);
    return fmtdup("Deleted breakpoint %d", number);
}

static char *tool_continue(void)
{
    char cls[32];
    cJSON *r = gdb_mi_command("-exec-continue", 5000, cls, sizeof(cls));
    char *err = mi_error_text(r, cls);
    if (err) { if (r) cJSON_Delete(r); return err; }
    cJSON_Delete(r);
    return strdup("Running — use gdb_wait_stop to wait for the next stop");
}

/* Halt a running target. Added after HW dogfooding: without it a long-running
 * gdb_continue/gdb_step (e.g. `next` over a slow loop) leaves the session stuck
 * with every command failing "target is running" and no clean way to recover. */
static char *tool_interrupt(void)
{
    char cls[32];
    cJSON *r = gdb_mi_command("-exec-interrupt", 5000, cls, sizeof(cls));
    char *err = mi_error_text(r, cls);
    if (err) { if (r) cJSON_Delete(r); return err; }
    cJSON_Delete(r);
    return strdup("Interrupt requested — call gdb_wait_stop to confirm the halt");
}

static char *tool_step(cJSON *args)
{
    const char *mode = jstr(args, "mode", "step");
    int count = sm_json_get_int(args, "count", 1);
    const char *mi;
    if      (strcmp(mode, "next") == 0)   mi = "-exec-next";
    else if (strcmp(mode, "finish") == 0) mi = "-exec-finish";
    else if (strcmp(mode, "stepi") == 0)  mi = "-exec-step-instruction";
    else if (strcmp(mode, "nexti") == 0)  mi = "-exec-next-instruction";
    else                                  mi = "-exec-step";

    char cmd[64];
    if (count > 1) snprintf(cmd, sizeof(cmd), "%s %d", mi, count);
    else           snprintf(cmd, sizeof(cmd), "%s", mi);

    char cls[32];
    cJSON *r = gdb_mi_command(cmd, 10000, cls, sizeof(cls));
    char *err = mi_error_text(r, cls);
    if (err) { if (r) cJSON_Delete(r); return err; }
    cJSON_Delete(r);
    return strdup("Stepped — use gdb_wait_stop for the resulting stop location");
}

static char *tool_backtrace(void)
{
    char cls[32];
    cJSON *r = gdb_mi_command("-stack-list-frames", 5000, cls, sizeof(cls));
    char *err = mi_error_text(r, cls);
    if (err) { if (r) cJSON_Delete(r); return err; }

    cJSON *stack = cJSON_GetObjectItem(r, "stack");
    sm_strbuf_t out;
    sm_strbuf_init(&out);
    if (cJSON_IsArray(stack) && cJSON_GetArraySize(stack) > 0) {
        cJSON *entry;
        cJSON_ArrayForEach(entry, stack) {
            cJSON *f = cJSON_GetObjectItem(entry, "frame");
            if (!f) f = entry;  /* tolerate either shape */
            sm_strbuf_printf(&out, "#%s %s in %s at %s:%s\n",
                jstr(f, "level", "?"), jstr(f, "addr", ""),
                jstr(f, "func", "?"), jstr(f, "file", "?"), jstr(f, "line", "?"));
        }
    } else {
        sm_strbuf_append_str(&out, "No frames");
    }
    cJSON_Delete(r);
    return sm_strbuf_steal(&out);
}

/* Map a register index (as reported in register-values "number") back to its
 * name via the register-names array, so output reads "pc" not "15". */
/* Match GDB register names with optional leading '$' (pc vs $pc). */
static int reg_name_match(const char *a, const char *b)
{
    if (!a || !b) return 0;
    if (a[0] == '$') a++;
    if (b[0] == '$') b++;
    return strcasecmp(a, b) == 0;
}

static const char *reg_name_at(cJSON *names, const char *number)
{
    if (!names || !number) return NULL;
    char *end;
    long i = strtol(number, &end, 10);
    if (*end != '\0' || i < 0) return NULL;
    cJSON *nm = cJSON_GetArrayItem(names, (int)i);
    return (nm && cJSON_IsString(nm) && nm->valuestring[0]) ? nm->valuestring : NULL;
}

static char *tool_read_registers(cJSON *args)
{
    /* Fetch register names once: used both to select the requested subset and
     * to label the values (dogfooding showed raw indices are unreadable). */
    char cls[32];
    cJSON *nr = gdb_mi_command("-data-list-register-names", 5000, cls, sizeof(cls));
    cJSON *names = (nr && strcmp(cls, "error") != 0)
                       ? cJSON_GetObjectItem(nr, "register-names") : NULL;
    if (names && !cJSON_IsArray(names)) names = NULL;

    /* Determine the wanted register set (explicit names arg, else the target
     * profile's important registers; empty => all). Explicit names hard-fail
     * if unknown; profile defaults skip names GDB does not advertise.
     *
     * Accept JSON array (correct schema) OR a string: single name, CSV, or a
     * JSON array encoded as a string (common when the old schema typed
     * "names" as string and agents followed it). */
    const char *wanted[SM_GDB_PROFILE_MAX_REGISTERS];
    char *wanted_owned[SM_GDB_PROFILE_MAX_REGISTERS];
    size_t wanted_owned_n = 0;
    size_t wanted_n = 0;
    int explicit_names = 0;
    cJSON *names_arg = cJSON_GetObjectItem(args, "names");
    cJSON *parsed_names = NULL;

    if (cJSON_IsArray(names_arg) && cJSON_GetArraySize(names_arg) > 0) {
        explicit_names = 1;
        cJSON *it;
        cJSON_ArrayForEach(it, names_arg)
            if (cJSON_IsString(it) && it->valuestring[0] &&
                wanted_n < SM_GDB_PROFILE_MAX_REGISTERS)
                wanted[wanted_n++] = it->valuestring;
    } else if (cJSON_IsString(names_arg) && names_arg->valuestring[0]) {
        explicit_names = 1;
        const char *s = names_arg->valuestring;
        /* Try JSON array string: ["pc","sp"] */
        if (s[0] == '[') {
            parsed_names = cJSON_Parse(s);
            if (cJSON_IsArray(parsed_names)) {
                cJSON *it;
                cJSON_ArrayForEach(it, parsed_names)
                    if (cJSON_IsString(it) && it->valuestring[0] &&
                        wanted_n < SM_GDB_PROFILE_MAX_REGISTERS)
                        wanted[wanted_n++] = it->valuestring;
            }
            if (wanted_n == 0 && parsed_names) {
                cJSON_Delete(parsed_names);
                parsed_names = NULL;
            }
        }
        if (wanted_n == 0) {
            /* CSV or single token: "pc,sp,lr" / "pc" */
            char *dup = strdup(s);
            if (!dup) {
                if (nr) cJSON_Delete(nr);
                return strdup("[ERROR] out of memory");
            }
            char *save = NULL;
            for (char *tok = strtok_r(dup, ", \t", &save);
                 tok && wanted_n < SM_GDB_PROFILE_MAX_REGISTERS;
                 tok = strtok_r(NULL, ", \t", &save)) {
                if (!tok[0]) continue;
                char *copy = strdup(tok);
                if (!copy) continue;
                wanted_owned[wanted_owned_n++] = copy;
                wanted[wanted_n++] = copy;
            }
            free(dup);
        }
    }

    if (!explicit_names) {
        for (size_t i = 0; i < g.profile.register_count; i++)
            wanted[wanted_n++] = g.profile.important_registers[i];
    }

    /* Build the index list for the wanted subset. */
    sm_strbuf_t idx;
    sm_strbuf_init(&idx);
    if (wanted_n > 0) {
        if (!names) {
            if (explicit_names) {
                sm_strbuf_destroy(&idx);
                if (nr) cJSON_Delete(nr);
                if (parsed_names) cJSON_Delete(parsed_names);
                for (size_t i = 0; i < wanted_owned_n; i++)
                    free(wanted_owned[i]);
                return strdup("[ERROR] register names unavailable "
                              "(is the target attached?)");
            }
            /* profile defaults, no name table: fall through to all regs */
        } else {
            int count = cJSON_GetArraySize(names);
            for (size_t w = 0; w < wanted_n; w++) {
                int found = 0;
                for (int i = 0; i < count; i++) {
                    cJSON *nm = cJSON_GetArrayItem(names, i);
                    if (!cJSON_IsString(nm) || !nm->valuestring[0]) continue;
                    if (reg_name_match(nm->valuestring, wanted[w])) {
                        sm_strbuf_printf(&idx, "%s%d", idx.len ? " " : "", i);
                        found = 1;
                        break;
                    }
                }
                if (!found && explicit_names) {
                    char *msg = fmtdup("[ERROR] unknown register name: %s",
                                       wanted[w]);
                    sm_strbuf_destroy(&idx);
                    if (nr) cJSON_Delete(nr);
                    if (parsed_names) cJSON_Delete(parsed_names);
                    for (size_t i = 0; i < wanted_owned_n; i++)
                        free(wanted_owned[i]);
                    return msg;
                }
            }
            if (explicit_names && idx.len == 0) {
                sm_strbuf_destroy(&idx);
                if (nr) cJSON_Delete(nr);
                if (parsed_names) cJSON_Delete(parsed_names);
                for (size_t i = 0; i < wanted_owned_n; i++)
                    free(wanted_owned[i]);
                return strdup("[ERROR] no matching registers for names filter");
            }
        }
    }

    char cmd[512];
    if (idx.len > 0)
        snprintf(cmd, sizeof(cmd), "-data-list-register-values x %s", idx.data);
    else
        snprintf(cmd, sizeof(cmd), "-data-list-register-values x");
    sm_strbuf_destroy(&idx);
    if (parsed_names) cJSON_Delete(parsed_names);
    for (size_t i = 0; i < wanted_owned_n; i++) free(wanted_owned[i]);

    cJSON *r = gdb_mi_command(cmd, 5000, cls, sizeof(cls));
    char *err = mi_error_text(r, cls);
    if (err) { if (r) cJSON_Delete(r); if (nr) cJSON_Delete(nr); return err; }

    cJSON *rv = cJSON_GetObjectItem(r, "register-values");
    sm_strbuf_t out;
    sm_strbuf_init(&out);
    if (cJSON_IsArray(rv) && cJSON_GetArraySize(rv) > 0) {
        cJSON *e;
        cJSON_ArrayForEach(e, rv) {
            const char *num = jstr(e, "number", "?");
            const char *name = reg_name_at(names, num);
            if (name)
                sm_strbuf_printf(&out, "  %-6s %s\n", name, jstr(e, "value", "?"));
            else
                sm_strbuf_printf(&out, "  r%s: %s\n", num, jstr(e, "value", "?"));
        }
    } else {
        sm_strbuf_append_str(&out, "No register data");
    }
    cJSON_Delete(r);
    if (nr) cJSON_Delete(nr);
    return sm_strbuf_steal(&out);
}

static char *format_memory(cJSON *result)
{
    cJSON *mem = cJSON_GetObjectItem(result, "memory");
    sm_strbuf_t out;
    sm_strbuf_init(&out);
    if (cJSON_IsArray(mem) && cJSON_GetArraySize(mem) > 0) {
        cJSON *region;
        cJSON_ArrayForEach(region, mem) {
            sm_strbuf_printf(&out, "  %s: %s\n",
                jstr(region, "begin", "?"), jstr(region, "contents", ""));
        }
    } else {
        sm_strbuf_append_str(&out, "No data");
    }
    return sm_strbuf_steal(&out);
}

static char *tool_read_memory(cJSON *args)
{
    const char *addr = sm_json_get_string(args, "address");
    int length = sm_json_get_int(args, "length", 256);
    if (!addr) return strdup("[ERROR] missing 'address'");
    if (length <= 0) length = 256;

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "-data-read-memory-bytes %s %d", addr, length);
    char cls[32];
    cJSON *r = gdb_mi_command(cmd, 5000, cls, sizeof(cls));
    char *err = mi_error_text(r, cls);
    if (err) { if (r) cJSON_Delete(r); return err; }
    char *out = format_memory(r);
    cJSON_Delete(r);
    return out;
}

static char *tool_evaluate(cJSON *args)
{
    const char *expr = sm_json_get_string(args, "expression");
    if (!expr) return strdup("[ERROR] missing 'expression'");
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "-data-evaluate-expression \"%s\"", expr);
    char cls[32];
    cJSON *r = gdb_mi_command(cmd, 5000, cls, sizeof(cls));
    char *err = mi_error_text(r, cls);
    if (err) { if (r) cJSON_Delete(r); return err; }
    char *out = fmtdup("%s", jstr(r, "value", "void"));
    cJSON_Delete(r);
    return out;
}

static char *tool_threads(void)
{
    char cls[32];
    cJSON *r = gdb_mi_command("-thread-info", 5000, cls, sizeof(cls));
    char *err = mi_error_text(r, cls);
    if (err) { if (r) cJSON_Delete(r); return err; }

    cJSON *threads = cJSON_GetObjectItem(r, "threads");
    sm_strbuf_t out;
    sm_strbuf_init(&out);
    if (cJSON_IsArray(threads) && cJSON_GetArraySize(threads) > 0) {
        cJSON *t;
        cJSON_ArrayForEach(t, threads) {
            cJSON *frame = cJSON_GetObjectItem(t, "frame");
            sm_strbuf_printf(&out, "  Thread %s (%s) state=%s in %s\n",
                jstr(t, "id", "?"), jstr(t, "target-id", ""),
                jstr(t, "state", "?"), jstr(frame, "func", "?"));
        }
    } else {
        sm_strbuf_append_str(&out, "No threads");
    }
    cJSON_Delete(r);

    /* RTOS-awareness: run the profile's rtos_commands as raw MI and append. */
    if (g.profile.rtos_command_count > 0) {
        sm_strbuf_printf(&out, "\nRTOS (%s):\n",
                         g.profile.rtos[0] ? g.profile.rtos : "generic");
        for (size_t i = 0; i < g.profile.rtos_command_count; i++) {
            cJSON *rr = gdb_mi_command(g.profile.rtos_commands[i], 5000,
                                       cls, sizeof(cls));
            if (rr) {
                char *j = cJSON_PrintUnformatted(rr);
                sm_strbuf_printf(&out, "  %s: %s\n", g.profile.rtos_commands[i],
                                 j ? j : "{}");
                free(j);
                cJSON_Delete(rr);
            }
        }
    }
    return sm_strbuf_steal(&out);
}

static char *tool_load(void)
{
    char cls[32];
    cJSON *r = gdb_mi_command("-target-download", 60000, cls, sizeof(cls));
    char *err = mi_error_text(r, cls);
    if (err) { if (r) cJSON_Delete(r); return err; }
    cJSON_Delete(r);
    return strdup("Loaded (flashed) the ELF to the target");
}

static char *gdb_flush_register_cache(void)
{
    char cls[32];
    /* OpenOCD monitor reset leaves GDB's cached regs stale until flush. */
    cJSON *r = gdb_mi_command(
        "-interpreter-exec console \"maintenance flush register-cache\"",
        5000, cls, sizeof(cls));
    char *err = mi_error_text(r, cls);
    if (r) cJSON_Delete(r);
    return err; /* NULL on success; caller may ignore non-fatal flush errors */
}

static char *tool_reset(cJSON *args)
{
    const char *mode = jstr(args, "mode", "halt");
    char cmd[80];
    int then_continue = 0;

    /* mode "run": reset halt + GDB -exec-continue so breakpoints are applied.
     * Direct "monitor reset run" resumes under OpenOCD without GDB BPs. */
    if (strcmp(mode, "init") == 0) {
        snprintf(cmd, sizeof(cmd), "monitor reset init");
    } else if (strcmp(mode, "run") == 0) {
        snprintf(cmd, sizeof(cmd), "monitor reset halt");
        then_continue = 1;
    } else {
        snprintf(cmd, sizeof(cmd), "monitor reset halt");
    }

    char cls[32];
    cJSON *r = gdb_mi_command(cmd, 10000, cls, sizeof(cls));
    char *err = mi_error_text(r, cls);
    if (err) { if (r) cJSON_Delete(r); return err; }
    cJSON_Delete(r);

    char *ferr = gdb_flush_register_cache();
    if (ferr) free(ferr); /* best-effort; older GDBs may lack the command */

    if (then_continue) {
        cJSON *cr = gdb_mi_command("-exec-continue", 10000, cls, sizeof(cls));
        char *cerr = mi_error_text(cr, cls);
        if (cr) cJSON_Delete(cr);
        if (cerr) return cerr;
        g.target_running = 1;
        g.have_stopped = 0;
        return fmtdup("Reset (run): halt + flush + continue");
    }

    g.target_running = 0;
    g.have_stopped = 1;
    return fmtdup("Reset (%s): done, register cache flushed", mode);
}

static char *tool_status(void)
{
    return fmtdup("Profile: %s (%s)\nLink: %s\nTarget: %s",
        g.profile.name, g.profile.arch,
        g.conn.running ? "connected to broker" : "disconnected",
        g.target_running ? "running" : "stopped/unknown");
}

static char *tool_wait_stop(cJSON *args)
{
    int timeout_ms = sm_json_get_int(args, "timeout_ms", 30000);
    if (timeout_ms <= 0) timeout_ms = 30000;

    struct timespec dl;
    deadline_from_now(&dl, timeout_ms);
    while (g.conn.running && !g.have_stopped) {
        int remain = remaining_ms(&dl);
        if (remain <= 0) break;
        if (sm_broker_conn_pump(&g.conn, remain) < 0) break;
    }
    if (!g.have_stopped)
        return strdup("[TIMEOUT] target may still be running");

    g.have_stopped = 0;
    sm_strbuf_t out;
    sm_strbuf_init(&out);
    sm_strbuf_printf(&out, "Stopped: %s — %s at %s:%s",
        g.stop_reason[0] ? g.stop_reason : "unknown",
        g.stop_func[0] ? g.stop_func : "?",
        g.stop_file[0] ? g.stop_file : "?",
        g.stop_line[0] ? g.stop_line : "?");
    if (g.stop_signal[0])
        sm_strbuf_printf(&out, " (signal: %s — %s)",
                         g.stop_signal, g.stop_signal_meaning);
    return sm_strbuf_steal(&out);
}

static char *tool_console_output(void)
{
    if (g.console.len == 0) return strdup("(no console output)");
    char *text = sm_strbuf_steal(&g.console);
    sm_strbuf_init(&g.console);
    return text;
}

/* --- ARM Cortex-M fault register decoding --- */

static const struct { int bit; const char *name; } CFSR_BITS[] = {
    {0,"IACCVIOL"},{1,"DACCVIOL"},{3,"MUNSTKERR"},{4,"MSTKERR"},{7,"MMARVALID"},
    {8,"IBUSERR"},{9,"PRECISERR"},{10,"IMPRECISERR"},{11,"UNSTKERR"},
    {12,"STKERR"},{13,"LSPERR"},{15,"BFARVALID"},
    {16,"UNDEFINSTR"},{17,"INVSTATE"},{18,"INVPC"},{19,"NOCP"},
    {24,"UNALIGNED"},{25,"DIVBYZERO"},
};
static const struct { int bit; const char *name; } HFSR_BITS[] = {
    {1,"VECTTBL"},{30,"FORCED"},{31,"DEBUGEVT"},
};

static void decode_fault_bits(sm_strbuf_t *out, const char *reg, uint32_t v)
{
    int first = 1;
    if (strcmp(reg, "CFSR") == 0) {
        for (size_t i = 0; i < sizeof(CFSR_BITS)/sizeof(CFSR_BITS[0]); i++)
            if (v & (1u << CFSR_BITS[i].bit)) {
                sm_strbuf_printf(out, "%s%s", first ? "" : ", ", CFSR_BITS[i].name);
                first = 0;
            }
    } else if (strcmp(reg, "HFSR") == 0) {
        for (size_t i = 0; i < sizeof(HFSR_BITS)/sizeof(HFSR_BITS[0]); i++)
            if (v & (1u << HFSR_BITS[i].bit)) {
                sm_strbuf_printf(out, "%s%s", first ? "" : ", ", HFSR_BITS[i].name);
                first = 0;
            }
    }
}

/* Parse a -data-read-memory-bytes contents hex string (bytes in address order)
 * as a little-endian uint32. Returns 1 on success. */
static int parse_hex_le32(const char *hex, uint32_t *out)
{
    if (!hex) return 0;
    size_t n = strlen(hex);
    if (n < 8) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char byte[3] = { hex[i*2], hex[i*2+1], 0 };
        char *end;
        long b = strtol(byte, &end, 16);
        if (*end != '\0') return 0;
        v |= (uint32_t)(b & 0xFF) << (8 * i);
    }
    *out = v;
    return 1;
}

static char *tool_read_fault_registers(void)
{
    if (g.profile.fault_register_count == 0)
        return strdup("No fault_registers in target profile");

    sm_strbuf_t out;
    sm_strbuf_init(&out);
    uint32_t cfsr = 0;
    int have_cfsr = 0;

    for (size_t i = 0; i < g.profile.fault_register_count; i++) {
        sm_gdb_fault_reg_t *fr = &g.profile.fault_registers[i];
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "-data-read-memory-bytes %s 4", fr->address);
        char cls[32];
        cJSON *r = gdb_mi_command(cmd, 5000, cls, sizeof(cls));
        if (!r || strcmp(cls, "error") == 0) {
            sm_strbuf_printf(&out, "  %s (%s): read failed\n", fr->name, fr->address);
            if (r) cJSON_Delete(r);
            continue;
        }
        cJSON *mem = cJSON_GetObjectItem(r, "memory");
        const char *contents = NULL;
        if (cJSON_IsArray(mem) && cJSON_GetArraySize(mem) > 0)
            contents = sm_json_get_string(cJSON_GetArrayItem(mem, 0), "contents");

        uint32_t val = 0;
        int ok = parse_hex_le32(contents, &val);
        sm_strbuf_printf(&out, "  %s (%s): 0x%08x", fr->name, fr->address,
                         ok ? val : 0);
        if (ok) {
            if (strcmp(fr->name, "CFSR") == 0) { cfsr = val; have_cfsr = 1; }
            sm_strbuf_t bits;
            sm_strbuf_init(&bits);
            decode_fault_bits(&bits, fr->name, val);
            if (bits.len) sm_strbuf_printf(&out, " — %s", bits.data);
            sm_strbuf_destroy(&bits);
            if (strcmp(fr->name, "MMFAR") == 0 && have_cfsr && (cfsr & (1u << 7)))
                sm_strbuf_append_str(&out, " (MMARVALID — faulting address)");
            if (strcmp(fr->name, "BFAR") == 0 && have_cfsr && (cfsr & (1u << 15)))
                sm_strbuf_append_str(&out, " (BFARVALID — faulting address)");
        }
        sm_strbuf_append_str(&out, "\n");
        cJSON_Delete(r);
    }
    return sm_strbuf_steal(&out);
}

static char *tool_read_peripheral(cJSON *args)
{
    const char *name = sm_json_get_string(args, "name");
    int num = sm_json_get_int(args, "num_registers", 16);
    if (!name) return strdup("[ERROR] missing 'name'");
    if (num <= 0) num = 16;

    if (g.profile.peripheral_count == 0)
        return strdup("No peripheral_map in target profile");
    const char *addr = sm_gdb_profile_peripheral_addr(&g.profile, name);
    if (!addr) {
        sm_strbuf_t avail;
        sm_strbuf_init(&avail);
        for (size_t i = 0; i < g.profile.peripheral_count; i++)
            sm_strbuf_printf(&avail, "%s%s", i ? ", " : "",
                             g.profile.peripherals[i].name);
        char *out = fmtdup("Unknown peripheral '%s'. Available: %s",
                           name, avail.data ? avail.data : "");
        sm_strbuf_destroy(&avail);
        return out;
    }

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "-data-read-memory-bytes %s %d", addr, num * 4);
    char cls[32];
    cJSON *r = gdb_mi_command(cmd, 5000, cls, sizeof(cls));
    char *err = mi_error_text(r, cls);
    if (err) { if (r) cJSON_Delete(r); return err; }

    char *mem = format_memory(r);
    char *out = fmtdup("%s @ %s:\n%s", name, addr, mem);
    free(mem);
    cJSON_Delete(r);
    return out;
}

/* Console commands that reach host code execution — rejected client-side for a
 * clear error (the broker's GDB link also blocks these, but that would surface
 * as an opaque timeout here). Mirrors src/links/gdb.c. */
static int command_is_blocked(const char *cmd)
{
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    while (*cmd >= '0' && *cmd <= '9') cmd++;      /* skip any MI token */
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    if (*cmd == '!' || *cmd == '|') return 1;
    static const char *const blocked[] = { "shell","pipe","python","guile","make" };
    for (size_t i = 0; i < sizeof(blocked)/sizeof(blocked[0]); i++) {
        size_t n = strlen(blocked[i]);
        if (strncmp(cmd, blocked[i], n) == 0 &&
            (cmd[n] == '\0' || cmd[n] == ' ' || cmd[n] == '\t' ||
             cmd[n] == '\r' || cmd[n] == '\n'))
            return 1;
    }
    return 0;
}

static char *tool_command(cJSON *args)
{
    const char *command = sm_json_get_string(args, "command");
    if (!command) return strdup("[ERROR] missing 'command'");
    if (command_is_blocked(command))
        return strdup("[ERROR] blocked command (shell/pipe/python/guile/make)");

    /* Sanitize: a raw command must be a single line (no MI injection). */
    char sanitized[1024];
    size_t o = 0;
    for (const char *p = command; *p && o < sizeof(sanitized) - 1; p++)
        if (*p != '\n' && *p != '\r') sanitized[o++] = *p;
    sanitized[o] = '\0';

    char cls[32];
    cJSON *r = gdb_mi_command(sanitized, 10000, cls, sizeof(cls));
    if (!r) return strdup("[ERROR] no response from GDB (timeout or link down)");
    char *json = cJSON_PrintUnformatted(r);
    char *out;
    if (strcmp(cls, "error") == 0)
        out = fmtdup("[%s] %s", cls, json ? json : "{}");
    else
        out = fmtdup("^%s %s", cls, json ? json : "{}");
    free(json);
    cJSON_Delete(r);
    return out;
}

/* --- Target identification (gdb_identify_target) --- */

/* Read a 32-bit little-endian word from target memory via MI. Returns 0 on
 * success (word in *out), -1 on MI error / fault / timeout. Unmapped vendor ID
 * registers fault on the wrong silicon — that fault IS the negative signal, so
 * callers probe best-effort and tolerate -1. */
static int read_word_le32(const char *addr, uint32_t *out)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "-data-read-memory-bytes %s 4", addr);
    char cls[32];
    cJSON *r = gdb_mi_command(cmd, 5000, cls, sizeof(cls));
    if (!r || strcmp(cls, "error") == 0) { if (r) cJSON_Delete(r); return -1; }

    cJSON *mem = cJSON_GetObjectItem(r, "memory");
    const char *contents = NULL;
    if (cJSON_IsArray(mem) && cJSON_GetArraySize(mem) > 0)
        contents = sm_json_get_string(cJSON_GetArrayItem(mem, 0), "contents");
    int ok = parse_hex_le32(contents, out);
    cJSON_Delete(r);
    return ok ? 0 : -1;
}

/* Decode the SCB CPUID PARTNO field (bits [15:4]) to an ARM Cortex-M core. */
static const char *cortex_m_core(unsigned partno)
{
    switch (partno) {
    case 0xC20: return "Cortex-M0";
    case 0xC60: return "Cortex-M0+";
    case 0xC21: return "Cortex-M1";
    case 0xC23: return "Cortex-M3";
    case 0xC24: return "Cortex-M4";
    case 0xC27: return "Cortex-M7";
    case 0xD20: return "Cortex-M23";
    case 0xD21: return "Cortex-M33";
    case 0xD22: return "Cortex-M55";
    default:    return NULL;
    }
}

static void evidence_add(cJSON *ev, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void evidence_add(cJSON *ev, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    cJSON_AddItemToArray(ev, cJSON_CreateString(buf));
}

static cJSON *identify_findings(void)
{
    cJSON *out = cJSON_CreateObject();
    cJSON *ev = cJSON_AddArrayToObject(out, "evidence");

    /* 1. SCB CPUID @ 0xE000ED00 — architectural ground truth for the core. */
    uint32_t cpuid = 0;
    if (read_word_le32("0xE000ED00", &cpuid) == 0) {
        unsigned implementer = (cpuid >> 24) & 0xFF;
        unsigned variant     = (cpuid >> 20) & 0xF;   /* rN */
        unsigned partno      = (cpuid >> 4)  & 0xFFF;
        unsigned revision    = cpuid & 0xF;            /* pM */
        const char *core = cortex_m_core(partno);

        char hexbuf[16];
        snprintf(hexbuf, sizeof(hexbuf), "0x%08x", cpuid);
        cJSON_AddStringToObject(out, "cpuid", hexbuf);
        cJSON_AddStringToObject(out, "implementer",
                                implementer == 0x41 ? "ARM" : "unknown");
        if (core) cJSON_AddStringToObject(out, "core", core);
        else      cJSON_AddStringToObject(out, "core", "unknown Cortex-M");
        char rev[16];
        snprintf(rev, sizeof(rev), "r%up%u", variant, revision);
        cJSON_AddStringToObject(out, "revision", rev);
        evidence_add(ev, "CPUID 0xE000ED00 = 0x%08x -> %s %s %s", cpuid,
                     implementer == 0x41 ? "ARM" : "impl?",
                     core ? core : "unknown-core", rev);
    } else {
        cJSON_AddStringToObject(out, "core", "unknown");
        evidence_add(ev, "CPUID 0xE000ED00: read failed (not a Cortex-M, or "
                         "target not halted/attached)");
    }

    /* 2. CoreSight ROM table @ 0xE00FF000 — light presence probe. The Cortex-M
     * PPB ROM table's first entry has bit0 (ENTRY_PRESENT) set when debug
     * components (SCS/DWT/FPB/ITM) are exposed. A full walk is deferred: CPUID
     * plus vendor IDs are the deterministic identification. */
    uint32_t rom0 = 0;
    if (read_word_le32("0xE00FF000", &rom0) == 0)
        evidence_add(ev, "ROM table 0xE00FF000 entry0 = 0x%08x (%s)", rom0,
                     (rom0 & 1) ? "CoreSight debug present"
                                : "no first entry");
    else
        evidence_add(ev, "ROM table 0xE00FF000: read failed");

    /* 3. Vendor device-ID registers — best-effort. Whichever responds on this
     * silicon (the others fault) sets vendor_guess. */
    int have_vendor = 0;

    /* STM32: DBGMCU_IDCODE @ 0xE0042000 — DEV_ID[11:0], REV_ID[31:16]. DEV_ID is
     * a mandatory nonzero identifier; require it, because 0xE0042000 sits in the
     * ARM-reserved external-PPB region and reads back as 0x00000000 (not a fault)
     * on non-STM32 parts — e.g. the SAM C21 — which would otherwise false-match. */
    uint32_t idcode = 0;
    if (read_word_le32("0xE0042000", &idcode) == 0 &&
        idcode != 0xFFFFFFFF && (idcode & 0xFFF) != 0) {
        unsigned dev_id = idcode & 0xFFF;
        unsigned rev_id = (idcode >> 16) & 0xFFFF;
        char hexbuf[16];
        snprintf(hexbuf, sizeof(hexbuf), "0x%03x", dev_id);
        cJSON_AddStringToObject(out, "vendor_guess", "STMicroelectronics STM32");
        cJSON_AddStringToObject(out, "dev_id", hexbuf);
        evidence_add(ev, "STM32 DBGMCU_IDCODE 0xE0042000 = 0x%08x -> DEV_ID=0x%03x "
                         "REV_ID=0x%04x", idcode, dev_id, rev_id);
        have_vendor = 1;
    }

    /* SAM D/C/E: DSU DID @ 0x41002018 — PROCESSOR[31:28], FAMILY[27:23],
     * SERIES[21:16], DIE[15:12], REVISION[11:8], DEVSEL[7:0]. */
    uint32_t did = 0;
    if (!have_vendor && read_word_le32("0x41002018", &did) == 0 &&
        did != 0xFFFFFFFF && did != 0) {
        unsigned processor = (did >> 28) & 0xF;
        unsigned family    = (did >> 23) & 0x1F;
        unsigned series    = (did >> 16) & 0x3F;
        unsigned die       = (did >> 12) & 0xF;
        unsigned revision  = (did >> 8)  & 0xF;
        unsigned devsel    = did & 0xFF;
        char hexbuf[16];
        snprintf(hexbuf, sizeof(hexbuf), "0x%08x", did);
        cJSON_AddStringToObject(out, "vendor_guess", "Microchip/Atmel SAM");
        cJSON_AddStringToObject(out, "dev_id", hexbuf);
        evidence_add(ev, "SAM DSU DID 0x41002018 = 0x%08x -> PROCESSOR=%u FAMILY=%u "
                         "SERIES=%u DIE=%u REV=%c DEVSEL=0x%02x", did, processor,
                     family, series, die, (char)('A' + revision), devsel);
        have_vendor = 1;
    }

    /* Nordic nRF52/53: FICR.INFO @ 0x10000000 base — PART @ +0x100, RAM @ +0x10C
     * (kB), FLASH @ +0x110 (kB). (nRF91 uses a different FICR layout; validate
     * live later.) */
    uint32_t part = 0;
    if (!have_vendor && read_word_le32("0x10000100", &part) == 0 &&
        part != 0xFFFFFFFF && part != 0) {
        char hexbuf[16];
        snprintf(hexbuf, sizeof(hexbuf), "0x%05x", part);
        cJSON_AddStringToObject(out, "vendor_guess", "Nordic nRF");
        cJSON_AddStringToObject(out, "dev_id", hexbuf);
        evidence_add(ev, "nRF FICR.INFO.PART 0x10000100 = 0x%05x", part);
        uint32_t ram = 0, flash = 0;
        if (read_word_le32("0x1000010C", &ram) == 0 && ram != 0xFFFFFFFF)
            cJSON_AddNumberToObject(out, "ram_kb", (double)ram);
        if (read_word_le32("0x10000110", &flash) == 0 && flash != 0xFFFFFFFF)
            cJSON_AddNumberToObject(out, "flash_kb", (double)flash);
        have_vendor = 1;
    }

    if (!have_vendor) {
        cJSON_AddStringToObject(out, "vendor_guess", "unknown");
        evidence_add(ev, "No known vendor ID register responded (STM32 "
                         "0xE0042000, SAM 0x41002018, nRF 0x10000100). Read the "
                         "part's ID register from its datasheet with "
                         "gdb_read_memory.");
    }

    return out;
}

static char *tool_identify_target(void)
{
    cJSON *out = identify_findings();
    char *text = cJSON_Print(out);
    cJSON_Delete(out);
    return text ? text : strdup("{}");
}

/* --- Profile auto-generation (gdb_generate_profile) --- */

/* True for cores with SCB configurable fault registers and the mainline-only
 * registers (basepri/faultmask): ARMv7-M (M3/M4/M7) and ARMv8-M mainline
 * (M33/M55). False for ARMv6-M (M0/M0+/M1) and ARMv8-M baseline (M23), whose
 * CFSR/HFSR addresses are reserved. */
static int core_is_mainline(const char *core)
{
    if (!core) return 0;
    return strstr(core, "M3")  || strstr(core, "M4") || strstr(core, "M7") ||
           strstr(core, "M33") || strstr(core, "M55");
}

/* Short slug for a vendor_guess string, for the profile name. */
static const char *vendor_slug(const char *vendor)
{
    if (!vendor) return "unknown";
    if (strstr(vendor, "STM32")) return "stm32";
    if (strstr(vendor, "SAM"))   return "sam";
    if (strstr(vendor, "nRF"))   return "nrf";
    return "unknown";
}

/* Lowercase kebab slug (e.g. "Cortex-M0+" -> "cortex-m0plus"). */
static void slugify(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 4 < n; p++) {
        char c = *p;
        if (c == '+') { out[o++]='p'; out[o++]='l'; out[o++]='u'; out[o++]='s'; }
        else if (c >= 'A' && c <= 'Z') out[o++] = (char)(c - 'A' + 'a');
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[o++] = c;
        else if (c == ' ' || c == '-' || c == '/') out[o++] = '-';
        /* else drop */
    }
    out[o] = '\0';
    if (o == 0) snprintf(out, n, "unknown");
}

/* Expand a leading "~/" to $HOME. */
static void expand_path(const char *in, char *out, size_t n)
{
    if (in[0] == '~' && in[1] == '/') {
        const char *home = getenv("HOME");
        snprintf(out, n, "%s/%s", home ? home : ".", in + 2);
    } else {
        snprintf(out, n, "%s", in);
    }
}

/* mkdir -p the parent directories of a file path (best effort). */
static void mkdir_parents(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(tmp, 0755);
        *p = '/';
    }
}

static int write_text_file(const char *path, const char *text)
{
    mkdir_parents(path);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t len = strlen(text);
    int ok = fwrite(text, 1, len, f) == len;
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

static const char *const BASELINE_REGS[] = {
    "r0","r1","r2","r3","r4","r5","r6","r7","r8","r9","r10","r11","r12",
    "sp","lr","pc","xpsr","msp","psp","primask","control",
};
static const char *const MAINLINE_EXTRA_REGS[] = { "basepri", "faultmask" };

static const sm_gdb_fault_reg_t SCB_FAULT_REGS[] = {
    { "CFSR",  "0xE000ED28" }, { "HFSR",  "0xE000ED2C" },
    { "DFSR",  "0xE000ED30" }, { "MMFAR", "0xE000ED34" },
    { "BFAR",  "0xE000ED38" }, { "AFSR",  "0xE000ED3C" },
};

/* ARM architectural system/debug peripherals — fixed addresses on every
 * Cortex-M (Armv6/7/8-M reference manuals), so seeding these is fact, not a
 * datasheet guess. Immediately usable via gdb_read_peripheral. */
typedef struct { const char *name; const char *addr; } sm_periph_def_t;
static const sm_periph_def_t ARCH_PERIPHS[] = {
    { "SCB",      "0xE000ED00" },  /* System Control Block (CPUID/VTOR/AIRCR/...) */
    { "SYSTICK",  "0xE000E010" },  /* SysTick timer */
    { "NVIC",     "0xE000E100" },  /* Nested Vectored Interrupt Controller */
    { "DCB",      "0xE000EDF0" },  /* Debug Control Block (DHCSR/DCRSR/DEMCR) */
    { "ROMTABLE", "0xE00FF000" },  /* CoreSight ROM table */
};
/* Present on Armv7-M / Armv8-M mainline; optional or absent on baseline
 * (M0/M0+/M1/M23), so only seeded for mainline cores. */
static const sm_periph_def_t MAINLINE_PERIPHS[] = {
    { "ITM", "0xE0000000" },  /* Instrumentation Trace Macrocell */
    { "DWT", "0xE0001000" },  /* Data Watchpoint & Trace */
    { "FPB", "0xE0002000" },  /* Flash Patch & Breakpoint */
    { "MPU", "0xE000ED90" },  /* Memory Protection Unit */
};

static void add_periph(sm_gdb_profile_t *p, const char *name, const char *addr)
{
    if (p->peripheral_count >= SM_GDB_PROFILE_MAX_PERIPHERALS) return;
    sm_gdb_peripheral_t *s = &p->peripherals[p->peripheral_count++];
    snprintf(s->name, sizeof(s->name), "%s", name);
    snprintf(s->address, sizeof(s->address), "%s", addr);
}

static char *tool_generate_profile(cJSON *args)
{
    cJSON *f = identify_findings();
    const char *core     = jstr(f, "core", "unknown Cortex-M");
    const char *revision = jstr(f, "revision", "");
    const char *vendor   = jstr(f, "vendor_guess", "unknown");
    const char *dev_id   = jstr(f, "dev_id", "");
    int mainline = core_is_mainline(core);

    sm_gdb_profile_t prof;
    sm_gdb_profile_init_default(&prof);
    snprintf(prof.arch, sizeof(prof.arch), "arm");
    snprintf(prof.gdb_path, sizeof(prof.gdb_path), "gdb-multiarch");

    const char *vslug = vendor_slug(vendor);
    char core_slug[32];
    slugify(core, core_slug, sizeof(core_slug));
    snprintf(prof.name, sizeof(prof.name), "%s-%s", vslug, core_slug);

    /* Fold flash/RAM sizes into the description when identify recovered them
     * (nRF FICR reports them directly; others usually don't). */
    cJSON *jf = cJSON_GetObjectItem(f, "flash_kb");
    cJSON *jr = cJSON_GetObjectItem(f, "ram_kb");
    char memnote[80] = "";
    if (cJSON_IsNumber(jf) && cJSON_IsNumber(jr))
        snprintf(memnote, sizeof(memnote), " %d KB flash / %d KB RAM.",
                 (int)jf->valuedouble, (int)jr->valuedouble);
    else if (cJSON_IsNumber(jf))
        snprintf(memnote, sizeof(memnote), " %d KB flash.", (int)jf->valuedouble);
    else if (cJSON_IsNumber(jr))
        snprintf(memnote, sizeof(memnote), " %d KB RAM.", (int)jr->valuedouble);

    snprintf(prof.description, sizeof(prof.description),
             "Auto-gen from gdb_identify_target: %s %s, vendor %s%s%s.%s "
             "peripheral_map seeded with Cortex-M + vendor debug blocks; add "
             "application peripherals (UART/SPI/GPIO) + rtos from the datasheet.",
             core, revision, vendor, dev_id[0] ? ", dev_id " : "", dev_id, memnote);

    /* Register set: architectural, keyed off the core class. */
    prof.register_count = 0;
    for (size_t i = 0; i < sizeof(BASELINE_REGS)/sizeof(BASELINE_REGS[0]); i++)
        snprintf(prof.important_registers[prof.register_count++],
                 SM_GDB_REG_NAME_LEN, "%s", BASELINE_REGS[i]);
    if (mainline)
        for (size_t i = 0; i < sizeof(MAINLINE_EXTRA_REGS)/sizeof(MAINLINE_EXTRA_REGS[0]); i++)
            snprintf(prof.important_registers[prof.register_count++],
                     SM_GDB_REG_NAME_LEN, "%s", MAINLINE_EXTRA_REGS[i]);

    /* Fault registers: the SCB set on mainline cores; none on baseline (their
     * CFSR/HFSR addresses are reserved — reading them would fault). */
    prof.fault_register_count = 0;
    if (mainline)
        for (size_t i = 0; i < sizeof(SCB_FAULT_REGS)/sizeof(SCB_FAULT_REGS[0]); i++)
            prof.fault_registers[prof.fault_register_count++] = SCB_FAULT_REGS[i];

    /* peripheral_map: architectural Cortex-M blocks (fixed addresses) + the one
     * vendor debug/ID block identify just confirmed responds. Application
     * peripherals stay for the datasheet — guessing their addresses would be
     * worse than leaving them out. */
    prof.peripheral_count = 0;
    for (size_t i = 0; i < sizeof(ARCH_PERIPHS)/sizeof(ARCH_PERIPHS[0]); i++)
        add_periph(&prof, ARCH_PERIPHS[i].name, ARCH_PERIPHS[i].addr);
    if (mainline)
        for (size_t i = 0; i < sizeof(MAINLINE_PERIPHS)/sizeof(MAINLINE_PERIPHS[0]); i++)
            add_periph(&prof, MAINLINE_PERIPHS[i].name, MAINLINE_PERIPHS[i].addr);
    if      (strcmp(vslug, "stm32") == 0) add_periph(&prof, "DBGMCU", "0xE0042000");
    else if (strcmp(vslug, "sam")   == 0) add_periph(&prof, "DSU",    "0x41002000");
    else if (strcmp(vslug, "nrf")   == 0) add_periph(&prof, "FICR",   "0x10000000");

    /* RTOS is opt-in: it can't be detected over bare SWD without symbols, so we
     * only stamp it when the caller already knows (no false auto-detection). */
    const char *rtos = sm_json_get_string(args, "rtos");
    if (rtos && rtos[0]) {
        snprintf(prof.rtos, sizeof(prof.rtos), "%s", rtos);
        snprintf(prof.rtos_commands[prof.rtos_command_count++],
                 SM_GDB_CMD_LEN, "info threads");
    }

    cJSON *pj = sm_gdb_profile_to_json(&prof);
    char *pjson = pj ? cJSON_Print(pj) : NULL;
    cJSON_Delete(pj);
    cJSON_Delete(f);
    if (!pjson) return strdup("[ERROR] failed to serialize generated profile");

    const char *path = sm_json_get_string(args, "path");
    if (path && path[0]) {
        char expanded[512];
        expand_path(path, expanded, sizeof(expanded));
        sm_strbuf_t sb;
        sm_strbuf_init(&sb);
        if (write_text_file(expanded, pjson) == 0)
            sm_strbuf_printf(&sb,
                "Saved auto-generated target profile to %s\n"
                "Use it next session with `-p %s`, or drop it in "
                "~/.config/smolmux/ for auto-discovery.\n\n", expanded, expanded);
        else
            sm_strbuf_printf(&sb,
                "[ERROR] could not write %s — profile returned inline:\n\n",
                expanded);
        sm_strbuf_append_str(&sb, pjson);
        free(pjson);
        return sm_strbuf_steal(&sb);
    }

    return pjson;  /* no path: just the JSON, for the caller to save */
}

/* --- Tool dispatch --- */

static char *dispatch_tool(const char *name, cJSON *args)
{
    if (strcmp(name, "gdb_launch") == 0)            return tool_launch(args);
    if (strcmp(name, "gdb_breakpoint") == 0)        return tool_breakpoint(args);
    if (strcmp(name, "gdb_delete_breakpoint") == 0) return tool_delete_breakpoint(args);
    if (strcmp(name, "gdb_continue") == 0)          return tool_continue();
    if (strcmp(name, "gdb_interrupt") == 0)         return tool_interrupt();
    if (strcmp(name, "gdb_step") == 0)              return tool_step(args);
    if (strcmp(name, "gdb_backtrace") == 0)         return tool_backtrace();
    if (strcmp(name, "gdb_read_registers") == 0)    return tool_read_registers(args);
    if (strcmp(name, "gdb_read_memory") == 0)       return tool_read_memory(args);
    if (strcmp(name, "gdb_evaluate") == 0)          return tool_evaluate(args);
    if (strcmp(name, "gdb_threads") == 0)           return tool_threads();
    if (strcmp(name, "gdb_load") == 0)              return tool_load();
    if (strcmp(name, "gdb_reset") == 0)             return tool_reset(args);
    if (strcmp(name, "gdb_status") == 0)            return tool_status();
    if (strcmp(name, "gdb_wait_stop") == 0)         return tool_wait_stop(args);
    if (strcmp(name, "gdb_console_output") == 0)    return tool_console_output();
    if (strcmp(name, "gdb_read_fault_registers") == 0) return tool_read_fault_registers();
    if (strcmp(name, "gdb_read_peripheral") == 0)   return tool_read_peripheral(args);
    if (strcmp(name, "gdb_identify_target") == 0)   return tool_identify_target();
    if (strcmp(name, "gdb_generate_profile") == 0)  return tool_generate_profile(args);
    if (strcmp(name, "gdb_command") == 0)           return tool_command(args);
    return fmtdup("[ERROR] unknown tool: %s", name);
}

/* --- tools/list schema --- */

static cJSON *schema_object(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    return s;
}
static void add_str(cJSON *props, const char *n, const char *d)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "string");
    cJSON_AddStringToObject(p, "description", d);
    cJSON_AddItemToObject(props, n, p);
}
static void add_int(cJSON *props, const char *n, const char *d)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "integer");
    cJSON_AddStringToObject(p, "description", d);
    cJSON_AddItemToObject(props, n, p);
}
static void add_bool(cJSON *props, const char *n, const char *d)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "boolean");
    cJSON_AddStringToObject(p, "description", d);
    cJSON_AddItemToObject(props, n, p);
}
/* JSON Schema array of strings (MCP tools). */
static void add_str_array(cJSON *props, const char *n, const char *d)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "array");
    cJSON *items = cJSON_CreateObject();
    cJSON_AddStringToObject(items, "type", "string");
    cJSON_AddItemToObject(p, "items", items);
    cJSON_AddStringToObject(p, "description", d);
    cJSON_AddItemToObject(props, n, p);
}
static cJSON *tool_entry(const char *name, const char *desc, cJSON *props,
                         const char *const *required, size_t req_n)
{
    cJSON *s = schema_object();
    cJSON_AddItemToObject(s, "properties", props ? props : cJSON_CreateObject());
    if (required && req_n) {
        cJSON *req = cJSON_CreateArray();
        for (size_t i = 0; i < req_n; i++)
            cJSON_AddItemToArray(req, cJSON_CreateString(required[i]));
        cJSON_AddItemToObject(s, "required", req);
    }
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "name", name);
    cJSON_AddStringToObject(t, "description", desc);
    cJSON_AddItemToObject(t, "inputSchema", s);
    return t;
}

static cJSON *build_gdb_tools_list(void)
{
    cJSON *tools = cJSON_CreateArray();
    cJSON *props;
    const char *req1[1];

    props = cJSON_CreateObject();
    add_str(props, "executable", "Path to the ELF file to load symbols from.");
    add_str(props, "target", "Optional GDB target string to (re)select.");
    req1[0] = "executable";
    cJSON_AddItemToArray(tools, tool_entry("gdb_launch",
        "Load an ELF's symbols (the broker owns the gdb process/connection).",
        props, req1, 1));

    props = cJSON_CreateObject();
    add_str(props, "location", "Function name, file:line, or *0xADDRESS.");
    add_str(props, "condition", "Optional condition expression.");
    add_bool(props, "temporary", "Delete the breakpoint after the first hit.");
    req1[0] = "location";
    cJSON_AddItemToArray(tools, tool_entry("gdb_breakpoint",
        "Set a breakpoint.", props, req1, 1));

    props = cJSON_CreateObject();
    add_int(props, "number", "Breakpoint number to delete.");
    req1[0] = "number";
    cJSON_AddItemToArray(tools, tool_entry("gdb_delete_breakpoint",
        "Delete a breakpoint by number.", props, req1, 1));

    cJSON_AddItemToArray(tools, tool_entry("gdb_continue",
        "Resume execution. Returns immediately; use gdb_wait_stop to wait.",
        NULL, NULL, 0));

    cJSON_AddItemToArray(tools, tool_entry("gdb_interrupt",
        "Halt a running target (e.g. after a long continue/step). Follow with "
        "gdb_wait_stop.", NULL, NULL, 0));

    props = cJSON_CreateObject();
    add_str(props, "mode", "step (into), next (over), finish (out), stepi, nexti.");
    add_int(props, "count", "Number of steps (default 1).");
    cJSON_AddItemToArray(tools, tool_entry("gdb_step",
        "Step execution.", props, NULL, 0));

    cJSON_AddItemToArray(tools, tool_entry("gdb_backtrace",
        "Get the current call stack with frame details.", NULL, NULL, 0));

    props = cJSON_CreateObject();
    add_str_array(props, "names",
        "Optional list of register names to read (e.g. [\"pc\",\"sp\",\"lr\"]). "
        "Default = the target profile's important registers. "
        "Also accepts a CSV string for clients that send strings.");
    cJSON_AddItemToArray(tools, tool_entry("gdb_read_registers",
        "Read CPU registers (optionally filtered by names).", props, NULL, 0));

    props = cJSON_CreateObject();
    add_str(props, "address", "Hex address (0x...) or expression (&var).");
    add_int(props, "length", "Number of bytes to read (default 256).");
    req1[0] = "address";
    cJSON_AddItemToArray(tools, tool_entry("gdb_read_memory",
        "Read raw memory from the target.", props, req1, 1));

    props = cJSON_CreateObject();
    add_str(props, "expression", "C expression to evaluate in the current frame.");
    req1[0] = "expression";
    cJSON_AddItemToArray(tools, tool_entry("gdb_evaluate",
        "Evaluate a C expression in the current frame context.", props, req1, 1));

    cJSON_AddItemToArray(tools, tool_entry("gdb_threads",
        "List all threads (RTOS-aware if the profile configures it).",
        NULL, NULL, 0));

    cJSON_AddItemToArray(tools, tool_entry("gdb_load",
        "Flash the loaded ELF to the target via GDB.", NULL, NULL, 0));

    props = cJSON_CreateObject();
    add_str(props, "mode", "halt (stop at reset), run, or init.");
    cJSON_AddItemToArray(tools, tool_entry("gdb_reset",
        "Reset the target via OpenOCD monitor. "
        "mode halt/init: leave stopped and flush GDB register cache. "
        "mode run: reset halt, flush cache, then -exec-continue "
        "(so GDB breakpoints apply; not bare monitor reset run).",
        props, NULL, 0));

    cJSON_AddItemToArray(tools, tool_entry("gdb_status",
        "Get current GDB/target status.", NULL, NULL, 0));

    props = cJSON_CreateObject();
    add_int(props, "timeout_ms", "Maximum time to wait in ms (default 30000).");
    cJSON_AddItemToArray(tools, tool_entry("gdb_wait_stop",
        "Wait for the target to stop (breakpoint, signal, or step completion).",
        props, NULL, 0));

    cJSON_AddItemToArray(tools, tool_entry("gdb_console_output",
        "Drain buffered GDB console/target output since the last read.",
        NULL, NULL, 0));

    cJSON_AddItemToArray(tools, tool_entry("gdb_read_fault_registers",
        "Read and decode ARM Cortex-M fault status registers (CFSR/HFSR/"
        "MMFAR/BFAR).", NULL, NULL, 0));

    props = cJSON_CreateObject();
    add_str(props, "name", "Peripheral name from the target profile (e.g. UARTE0).");
    add_int(props, "num_registers", "Number of 32-bit registers to read (default 16).");
    req1[0] = "name";
    cJSON_AddItemToArray(tools, tool_entry("gdb_read_peripheral",
        "Read memory-mapped registers from a named peripheral.", props, req1, 1));

    cJSON_AddItemToArray(tools, tool_entry("gdb_identify_target",
        "Identify unknown silicon: decode SCB CPUID (ARM Cortex-M core + "
        "revision), probe the CoreSight ROM table, and best-effort read vendor "
        "device-ID registers (STM32 DBGMCU_IDCODE, SAM DSU DID, nRF FICR) -> "
        "structured JSON {core, revision, vendor_guess, dev_id, flash_kb, "
        "ram_kb, evidence}. Target must be halted and attached.",
        NULL, NULL, 0));

    props = cJSON_CreateObject();
    add_str(props, "path", "Optional file path to write the profile to (e.g. "
                           "~/.config/smolmux/mine.gdb-profile.json). '~/' is "
                           "expanded; parent dirs are created. Omit to return "
                           "the JSON inline only.");
    add_str(props, "rtos", "Optional RTOS name (e.g. 'zephyr', 'freertos') to "
                           "stamp into the profile — not auto-detected over SWD.");
    cJSON_AddItemToArray(tools, tool_entry("gdb_generate_profile",
        "Run gdb_identify_target and emit a starter *.gdb-profile.json for the "
        "detected silicon: arch, the architectural register set, and — keyed off "
        "the core — the SCB fault registers (present on M3/M4/M7/M33, empty on "
        "M0/M0+/M23). peripheral_map is seeded with the fixed-address Cortex-M "
        "system/debug blocks (SCB/NVIC/SysTick/DWT/...) plus the vendor debug "
        "block; add application peripherals from the datasheet. Optionally writes "
        "the file so the next session can load that profile with -p.",
        props, NULL, 0));

    props = cJSON_CreateObject();
    add_str(props, "command", "GDB/MI command (e.g. -break-list).");
    req1[0] = "command";
    cJSON_AddItemToArray(tools, tool_entry("gdb_command",
        "Send a raw GDB/MI command to the debugger.", props, req1, 1));

    return tools;
}

/* --- Resources --- */

#define GDB_PROFILE_RESOURCE_URI  "smolmux-gdb://target/profile"
#define GDB_PROBING_RESOURCE_URI   "smolmux-gdb://board-probing"

/* Unknown-board probing runbook. Served both as the board-probing MCP resource
 * (below) and the probe_unknown_board prompt (further down). */
static const char PROMPT_PROBE_UNKNOWN_BOARD[] =
    "You are probing an unknown embedded board over SWD/GDB. Read facts "
    "from the silicon — do not guess from a datasheet you haven't confirmed. Work "
    "top-down:\n\n"
    "1. Confirm the debug session is live:\n"
    "   - gdb_status(); if the target is running, gdb_interrupt() then "
    "gdb_wait_stop(). Most reads below need a halted core.\n"
    "2. Identify the silicon (the fast path):\n"
    "   - gdb_identify_target() — decodes SCB CPUID (ARM Cortex-M core + rNpM "
    "revision), probes the CoreSight ROM table, and reads well-known vendor ID "
    "registers (STM32 DBGMCU_IDCODE, SAM DSU DID, nRF FICR) into structured JSON "
    "with an evidence trail. Start here.\n"
    "   - If vendor_guess is 'unknown', read the part's ID register manually with "
    "gdb_read_memory once you know the family from CPUID + the datasheet. Key "
    "addresses: CPUID 0xE000ED00; STM32 DBGMCU_IDCODE 0xE0042000; SAM DSU DID "
    "0x41002018; nRF FICR.INFO.PART 0x10000100.\n"
    "   - gdb_command({command:\"monitor ...\"}) surfaces what the gdbserver "
    "already scanned (OpenOCD: 'monitor mdw', 'monitor flash banks').\n"
    "3. Map memory. With small gdb_read_memory() reads, confirm flash "
    "(0x00000000 / 0x08000000 / ...) and SRAM (0x20000000) respond and roughly "
    "where each ends. A fault means unmapped — that is itself information.\n"
    "4. Snapshot CPU state: gdb_reset({mode:\"halt\"}), then gdb_read_registers() "
    "(name-labeled), gdb_backtrace().\n"
    "5. Enumerate peripherals: for each peripheral in the target profile, "
    "gdb_read_peripheral({name:...}) and cross-reference the dump against the "
    "datasheet register table. gdb_read_fault_registers() decodes Cortex-M "
    "fault state (M3/M4/M7/M33; M0/M0+ have none).\n"
    "6. Correlate with the console. Over the serial tools (smolmux-mcp), reset "
    "the board and read serial_output_history() for the boot banner; confirm the "
    "CPUID-derived core matches the SoC the banner names. Note every place SWD, "
    "UART, and the datasheet disagree; record those discrepancies.\n"
    "7. Persist what you learned. gdb_generate_profile({path:\"~/.config/smolmux/"
    "<board>.gdb-profile.json\"}) writes a starter profile (arch, register set, "
    "core-correct fault registers). Load it next session with -p; then fill in "
    "peripheral_map and rtos from the datasheet.\n\n"
    "Full runbook: docs/board-exploration-workflow.md in the smolmux repo.\n";

static void handle_resources_list(cJSON *id)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(result, "resources");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "uri", GDB_PROFILE_RESOURCE_URI);
    cJSON_AddStringToObject(r, "name", "GDB target profile");
    cJSON_AddStringToObject(r, "description",
        "The active target profile: arch, important/fault registers, "
        "peripheral map, and RTOS-awareness commands the tools use.");
    cJSON_AddStringToObject(r, "mimeType", "application/json");
    cJSON_AddItemToArray(arr, r);

    cJSON *r2 = cJSON_CreateObject();
    cJSON_AddStringToObject(r2, "uri", GDB_PROBING_RESOURCE_URI);
    cJSON_AddStringToObject(r2, "name", "Unknown-board probing runbook");
    cJSON_AddStringToObject(r2, "description",
        "Runbook for identifying and mapping an unknown board over SWD/GDB: "
        "CPUID -> vendor ID -> memory map -> peripherals -> console banner.");
    cJSON_AddStringToObject(r2, "mimeType", "text/markdown");
    cJSON_AddItemToArray(arr, r2);

    mcp_send_result(id, result);
}

static void handle_resources_read(cJSON *id, cJSON *params)
{
    cJSON *uri = cJSON_GetObjectItemCaseSensitive(params, "uri");
    if (!cJSON_IsString(uri)) {
        mcp_send_error(id, -32602, "missing resource uri");
        return;
    }

    const char *u = uri->valuestring;
    const char *mime;
    char *text;
    if (strcmp(u, GDB_PROFILE_RESOURCE_URI) == 0) {
        cJSON *pj = sm_gdb_profile_to_json(&g.profile);
        text = pj ? cJSON_Print(pj) : NULL;
        cJSON_Delete(pj);
        if (!text) text = strdup("{}");
        mime = "application/json";
    } else if (strcmp(u, GDB_PROBING_RESOURCE_URI) == 0) {
        text = strdup(PROMPT_PROBE_UNKNOWN_BOARD);
        mime = "text/markdown";
    } else {
        mcp_send_error(id, -32602, "unknown resource uri");
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON *contents = cJSON_AddArrayToObject(result, "contents");
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "uri", u);
    cJSON_AddStringToObject(c, "mimeType", mime);
    cJSON_AddStringToObject(c, "text", text ? text : "");
    cJSON_AddItemToArray(contents, c);
    free(text);
    mcp_send_result(id, result);
}

/* --- Prompts --- */

/* Guide to diagnose a fault; interpolates the reported symptom. Malloc'd. */
static char *prompt_diagnose_fault(const char *symptom)
{
    sm_strbuf_t sb;
    sm_strbuf_init(&sb);
    sm_strbuf_printf(&sb,
        "The embedded target has experienced: %s\n\n"
        "Work through these steps using the gdb_* tools:\n"
        "1. gdb_status() — confirm GDB is connected and the target is halted.\n"
        "2. gdb_backtrace() — capture the call stack at the fault.\n"
        "3. gdb_read_fault_registers() — read CFSR/HFSR/MMFAR/BFAR and decode\n"
        "   the bits to classify the fault (usage/bus/mem-manage/hard).\n"
        "4. gdb_read_registers() — inspect CPU register state (pc, sp, lr, ...).\n"
        "5. gdb_evaluate() — inspect variables from the faulting frame.\n"
        "6. gdb_read_memory() — examine stack contents around SP.\n"
        "7. Cross-reference the backtrace with the source to find root cause.\n"
        "Note: on Cortex-M0/M0+ there are no configurable fault registers; the\n"
        "profile's fault_registers will be empty and step 3 reports none.\n",
        symptom);
    char *out = strdup(sb.data ? sb.data : "");
    sm_strbuf_destroy(&sb);
    return out;
}

static const char PROMPT_ANALYZE_CRASH[] =
    "A crash was detected. Correlate serial console output with GDB target "
    "state:\n\n"
    "1. From the smolmux serial tools (smolmux-mcp), get the crash context:\n"
    "   - serial_get_incidents() for the crash event and pre-crash output.\n"
    "   - serial_read()/history for the full output timeline.\n"
    "2. From the GDB tools, inspect the target:\n"
    "   - gdb_backtrace() for the call stack.\n"
    "   - gdb_read_fault_registers() for fault details.\n"
    "   - gdb_read_registers() for CPU state.\n"
    "   - gdb_evaluate() for variable inspection.\n"
    "3. Correlate the two:\n"
    "   - What was the last serial output before the crash?\n"
    "   - Does the backtrace match the expected code path?\n"
    "   - Is an interrupt handler in the call stack?\n"
    "4. Form a hypothesis and verify with gdb_read_memory().\n"
    "5. Propose a fix with a root-cause explanation.\n";

static void handle_prompts_list(cJSON *id)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(result, "prompts");

    cJSON *p1 = cJSON_CreateObject();
    cJSON_AddStringToObject(p1, "name", "diagnose_fault");
    cJSON_AddStringToObject(p1, "description",
        "Step-by-step guide to diagnose a target fault with the GDB tools.");
    cJSON *a1 = cJSON_AddArrayToObject(p1, "arguments");
    cJSON *arg = cJSON_CreateObject();
    cJSON_AddStringToObject(arg, "name", "symptom");
    cJSON_AddStringToObject(arg, "description",
        "What went wrong (e.g. 'hard fault', 'hang', 'stack overflow').");
    cJSON_AddBoolToObject(arg, "required", 0);
    cJSON_AddItemToArray(a1, arg);
    cJSON_AddItemToArray(arr, p1);

    cJSON *p2 = cJSON_CreateObject();
    cJSON_AddStringToObject(p2, "name", "analyze_crash");
    cJSON_AddStringToObject(p2, "description",
        "Guide to correlate serial console output with GDB target state "
        "after a crash.");
    cJSON_AddArrayToObject(p2, "arguments");   /* no arguments */
    cJSON_AddItemToArray(arr, p2);

    cJSON *p3 = cJSON_CreateObject();
    cJSON_AddStringToObject(p3, "name", "probe_unknown_board");
    cJSON_AddStringToObject(p3, "description",
        "Runbook to identify and map an unknown board over SWD/GDB "
        "(CPUID -> vendor ID -> memory map -> peripherals -> console banner).");
    cJSON_AddArrayToObject(p3, "arguments");   /* no arguments */
    cJSON_AddItemToArray(arr, p3);

    mcp_send_result(id, result);
}

static void handle_prompts_get(cJSON *id, cJSON *params)
{
    cJSON *name = cJSON_GetObjectItemCaseSensitive(params, "name");
    if (!cJSON_IsString(name)) {
        mcp_send_error(id, -32602, "missing prompt name");
        return;
    }

    char *text = NULL;
    const char *desc = NULL;
    if (strcmp(name->valuestring, "diagnose_fault") == 0) {
        const char *symptom = "hard fault";
        cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
        if (args) {
            cJSON *s = cJSON_GetObjectItemCaseSensitive(args, "symptom");
            if (cJSON_IsString(s) && s->valuestring[0])
                symptom = s->valuestring;
        }
        text = prompt_diagnose_fault(symptom);
        desc = "Diagnose a target fault with GDB";
    } else if (strcmp(name->valuestring, "analyze_crash") == 0) {
        text = strdup(PROMPT_ANALYZE_CRASH);
        desc = "Correlate serial output with GDB state after a crash";
    } else if (strcmp(name->valuestring, "probe_unknown_board") == 0) {
        text = strdup(PROMPT_PROBE_UNKNOWN_BOARD);
        desc = "Identify and map an unknown board over SWD/GDB";
    } else {
        mcp_send_error(id, -32602, "unknown prompt");
        return;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "description", desc);
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

/* --- MCP message dispatch --- */

static void handle_initialize(cJSON *id)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "protocolVersion", SM_MCP_PROTOCOL_VERSION);
    cJSON *caps = cJSON_CreateObject();
    cJSON_AddItemToObject(caps, "tools", cJSON_CreateObject());
    cJSON_AddItemToObject(caps, "resources", cJSON_CreateObject());
    cJSON_AddItemToObject(caps, "prompts", cJSON_CreateObject());
    cJSON_AddItemToObject(result, "capabilities", caps);
    cJSON *info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "name", SM_NAME "-gdb-mcp");
    cJSON_AddStringToObject(info, "version", SM_VERSION);
    cJSON_AddItemToObject(result, "serverInfo", info);
    mcp_send_result(id, result);
    g.initialized = 1;
    SM_LOG_INFO(LOG_TAG, "MCP initialized");
}

static void handle_tools_call(cJSON *id, cJSON *params)
{
    cJSON *name = cJSON_GetObjectItemCaseSensitive(params, "name");
    if (!cJSON_IsString(name)) {
        mcp_send_error(id, -32602, "missing tool name");
        return;
    }
    cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
    cJSON *args_tmp = NULL;
    if (!args) { args_tmp = cJSON_CreateObject(); args = args_tmp; }
    char *result = dispatch_tool(name->valuestring, args);
    if (result) { mcp_send_tool_result(id, result); free(result); }
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
    if (strcmp(m, "initialize") == 0) {
        handle_initialize(id);
    } else if (strcmp(m, "notifications/initialized") == 0) {
        /* no response */
    } else if (strcmp(m, "tools/list") == 0) {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddItemToObject(result, "tools", build_gdb_tools_list());
        mcp_send_result(id, result);
    } else if (strcmp(m, "tools/call") == 0) {
        cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");
        cJSON *params_tmp = NULL;
        if (!params) { params_tmp = cJSON_CreateObject(); params = params_tmp; }
        handle_tools_call(id, params);
        cJSON_Delete(params_tmp);
    } else if (strcmp(m, "resources/list") == 0) {
        handle_resources_list(id);
    } else if (strcmp(m, "resources/read") == 0) {
        cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");
        cJSON *params_tmp = NULL;
        if (!params) { params_tmp = cJSON_CreateObject(); params = params_tmp; }
        handle_resources_read(id, params);
        cJSON_Delete(params_tmp);
    } else if (strcmp(m, "prompts/list") == 0) {
        handle_prompts_list(id);
    } else if (strcmp(m, "prompts/get") == 0) {
        cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");
        cJSON *params_tmp = NULL;
        if (!params) { params_tmp = cJSON_CreateObject(); params = params_tmp; }
        handle_prompts_get(id, params);
        cJSON_Delete(params_tmp);
    } else {
        if (id) mcp_send_error(id, -32601, "method not found");
    }
}

/* --- stdin (MCP JSON-RPC) --- */

static void handle_stdin_data(void)
{
    if (g.stdin_len >= g.stdin_cap - 1) {
        if (g.stdin_cap >= SM_MCP_READ_BUF_MAX) { g.stdin_len = 0; return; }
        size_t new_cap = g.stdin_cap * 2;
        if (new_cap > SM_MCP_READ_BUF_MAX) new_cap = SM_MCP_READ_BUF_MAX;
        void *tmp = realloc(g.stdin_buf, new_cap);
        if (!tmp) return;
        g.stdin_buf = tmp;
        g.stdin_cap = new_cap;
    }

    ssize_t n = read(STDIN_FILENO, g.stdin_buf + g.stdin_len,
                     g.stdin_cap - g.stdin_len - 1);
    if (n <= 0) {
        if (n == 0) { SM_LOG_INFO(LOG_TAG, "stdin closed"); g.conn.running = 0; }
        return;
    }
    g.stdin_len += (size_t)n;

    while (1) {
        char *nl = memchr(g.stdin_buf, '\n', g.stdin_len);
        if (!nl) break;
        size_t line_len = (size_t)(nl - g.stdin_buf);
        g.stdin_buf[line_len] = '\0';

        cJSON *msg = cJSON_Parse(g.stdin_buf);
        if (msg) { dispatch_mcp_message(msg); cJSON_Delete(msg); }
        else { mcp_send_error(NULL, -32700, "parse error"); }

        size_t remaining = g.stdin_len - line_len - 1;
        memmove(g.stdin_buf, nl + 1, remaining);
        g.stdin_len = remaining;
    }
}

static void signal_handler(int sig) { (void)sig; g.conn.running = 0; }

/* --- Profile discovery --- */

/* Returns 0 on success, -1 if explicit/env profile cannot be resolved.
 * No -p means defaults only (never auto-pick first *.gdb-profile.json). */
static int discover_profile(const char *profile_path)
{
    char resolved[512];
    sm_gdb_profile_init_default(&g.profile);

    if (profile_path && profile_path[0]) {
        if (sm_profile_resolve_path(profile_path, SM_GDB_PROFILE_FILE_SUFFIX,
                                    resolved, sizeof(resolved)) != 0) {
            SM_LOG_ERROR(LOG_TAG, "failed to resolve GDB profile '%s'",
                         profile_path);
            return -1;
        }
        if (sm_gdb_profile_load(&g.profile, resolved) != 0) {
            SM_LOG_ERROR(LOG_TAG, "failed to load GDB profile: %s", resolved);
            return -1;
        }
        SM_LOG_INFO(LOG_TAG, "loaded GDB profile: %s", resolved);
        return 0;
    }

    const char *env = getenv(SM_GDB_PROFILE_ENV);
    if (env && env[0]) {
        if (sm_profile_resolve_path(env, SM_GDB_PROFILE_FILE_SUFFIX,
                                    resolved, sizeof(resolved)) != 0) {
            SM_LOG_ERROR(LOG_TAG, "failed to resolve $%s=%s", SM_GDB_PROFILE_ENV,
                         env);
            return -1;
        }
        if (sm_gdb_profile_load(&g.profile, resolved) != 0) {
            SM_LOG_ERROR(LOG_TAG, "failed to load GDB profile from env: %s",
                         resolved);
            return -1;
        }
        SM_LOG_INFO(LOG_TAG, "loaded GDB profile from env: %s", resolved);
        return 0;
    }

    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [socket] [options]\n\n"
        "Connect to a smolmux broker holding a GDB link (broker started with\n"
        "--gdb) and expose GDB debugging tools over MCP stdio.\n\n"
        "Options:\n"
        "  [socket]               Broker Unix socket path (positional)\n"
        "  -s, --socket <path>    Broker Unix socket path\n"
        "  -T, --tcp <host:port>  Connect via TCP instead\n"
        "  -p, --profile <spec>   Target profile path or short name\n"
        "  -n, --name <name>      Client name (default: claude-gdb-mcp)\n"
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

    memset(&g, 0, sizeof(g));
    g.next_token = 1;
    snprintf(g.name, sizeof(g.name), "claude-gdb-mcp");

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
    while ((opt = getopt_long(argc, argv, "s:T:p:n:vVh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 's': socket_path = optarg; break;
        case 'T': tcp_target = optarg; break;
        case 'p': profile_path = optarg; break;
        case 'n': snprintf(g.name, sizeof(g.name), "%s", optarg); break;
        case 'v': g.verbose = 1; break;
        case 'V': fprintf(stderr, "%s-gdb-mcp %s\n", SM_NAME, SM_VERSION); return 0;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    sm_logger_set_level(g.verbose ? SM_LOG_DEBUG : SM_LOG_WARN);

    sm_strbuf_init(&g.mi_buf);
    sm_strbuf_init(&g.console);
    g.stdin_cap = 8192;
    g.stdin_buf = malloc(g.stdin_cap);
    if (!g.stdin_buf || sm_broker_conn_init(&g.conn, 8192) != 0) {
        fprintf(stderr, "Error: allocation failed\n");
        return 1;
    }
    g.conn.verbose = g.verbose;
    sm_broker_conn_set_output_cb(&g.conn, gdb_on_broker_output, NULL);

    if (discover_profile(profile_path) != 0)
        return 1;

    if (tcp_target) {
        char host[240];
        int port = SM_TCP_DEFAULT_PORT;
        sm_parse_host_port(tcp_target, host, sizeof(host), &port);
        g.conn.fd = sm_connect_tcp(host, port);
        if (g.conn.fd < 0) {
            fprintf(stderr, "Error: cannot connect to %s:%d: %s\n",
                    host, port, strerror(errno));
            return 1;
        }
        SM_LOG_INFO(LOG_TAG, "connected to broker via TCP %s:%d", host, port);
    } else {
        char discovered[108];
        if (!socket_path && optind < argc) socket_path = argv[optind];
        if (!socket_path) {
            if (sm_discover_socket(discovered, sizeof(discovered)) == 0)
                socket_path = discovered;
            else {
                fprintf(stderr, "Error: no broker socket found\n");
                return 1;
            }
        }
        g.conn.fd = sm_connect_unix(socket_path);
        if (g.conn.fd < 0) {
            fprintf(stderr, "Error: cannot connect to %s: %s\n",
                    socket_path, strerror(errno));
            return 1;
        }
        SM_LOG_INFO(LOG_TAG, "connected to broker via %s", socket_path);
    }

    sm_broker_conn_send(&g.conn, sm_msg_hello(g.name, "controller"));
    cJSON *welcome = sm_broker_conn_wait(&g.conn, NULL, 5000);
    if (welcome) cJSON_Delete(welcome);
    else SM_LOG_WARN(LOG_TAG, "no welcome received from broker");

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    struct pollfd fds[2] = {
        { .fd = STDIN_FILENO, .events = POLLIN },
        { .fd = g.conn.fd,    .events = POLLIN },
    };

    while (g.conn.running) {
        int ret = poll(fds, 2, -1);
        if (ret < 0) { if (errno == EINTR) continue; break; }

        /* Broker output → MI stream (async records flow into g). */
        if (fds[1].revents & POLLIN) {
            cJSON *stray = sm_broker_conn_read(&g.conn, NULL);
            if (stray) cJSON_Delete(stray);
        }
        if (fds[1].revents & (POLLHUP | POLLERR)) {
            SM_LOG_INFO(LOG_TAG, "broker disconnected");
            break;
        }

        if (fds[0].revents & POLLIN)
            handle_stdin_data();
        else if (fds[0].revents & (POLLHUP | POLLERR)) {
            SM_LOG_INFO(LOG_TAG, "stdin closed");
            g.conn.running = 0;
        }
    }

    sm_broker_conn_destroy(&g.conn);
    sm_strbuf_destroy(&g.mi_buf);
    sm_strbuf_destroy(&g.console);
    if (g.result) cJSON_Delete(g.result);
    free(g.stdin_buf);
    return 0;
}
