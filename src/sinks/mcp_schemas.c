/* MCP tool schemas — shared by the in-broker MCP sink (mcp.c) and the
 * standalone smolmux-mcp binary (mcp_client.c). Extracted from the two
 * previously-duplicated tool-list builders so they cannot drift. */
#include "sinks/mcp_schemas.h"
#include "sinks/mcp_results.h"

#include <stdlib.h>
#include <string.h>

static cJSON *schema_object(void)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    return s;
}

static void schema_add_string(cJSON *props, const char *name, const char *desc)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "string");
    cJSON_AddStringToObject(p, "description", desc);
    cJSON_AddItemToObject(props, name, p);
}

static void schema_add_integer(cJSON *props, const char *name, const char *desc)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "integer");
    cJSON_AddStringToObject(p, "description", desc);
    cJSON_AddItemToObject(props, name, p);
}

static void schema_add_number(cJSON *props, const char *name, const char *desc)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "number");
    cJSON_AddStringToObject(p, "description", desc);
    cJSON_AddItemToObject(props, name, p);
}

static void schema_add_boolean(cJSON *props, const char *name, const char *desc)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "boolean");
    cJSON_AddStringToObject(p, "description", desc);
    cJSON_AddItemToObject(props, name, p);
}

static cJSON *make_tool_ex(const char *name, const char *desc, cJSON *input_schema,
                           int read_only, int destructive, int open_world,
                           const char *title)
{
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "name", name);
    cJSON_AddStringToObject(t, "description", desc);
    cJSON_AddItemToObject(t, "inputSchema", input_schema);
    cJSON *output_schema = sm_mcp_output_schema(name);
    if (output_schema)
        cJSON_AddItemToObject(t, "outputSchema", output_schema);
    cJSON *ann = cJSON_CreateObject();
    if (title)
        cJSON_AddStringToObject(ann, "title", title);
    cJSON_AddBoolToObject(ann, "readOnlyHint", read_only ? 1 : 0);
    cJSON_AddBoolToObject(ann, "destructiveHint", destructive ? 1 : 0);
    cJSON_AddBoolToObject(ann, "openWorldHint", open_world ? 1 : 0);
    cJSON_AddItemToObject(t, "annotations", ann);
    return t;
}

static cJSON *make_tool(const char *name, const char *desc, cJSON *input_schema)
{
    return make_tool_ex(name, desc, input_schema, 0, 0, 1, NULL);
}

static cJSON *make_tool_ro(const char *name, const char *desc, cJSON *input_schema)
{
    return make_tool_ex(name, desc, input_schema, 1, 0, 0, NULL);
}

static cJSON *make_tool_destr(const char *name, const char *desc,
                              cJSON *input_schema)
{
    return make_tool_ex(name, desc, input_schema, 0, 1, 1, NULL);
}

/* --- tools/list --- */

int sm_mcp_mutate_enabled(void)
{
    const char *e = getenv("SMOLMUX_MCP_MUTATE");
    if (!e || !e[0])
        return 0;
    return e[0] == '1' || e[0] == 'y' || e[0] == 'Y' ||
           e[0] == 't' || e[0] == 'T';
}

int sm_mcp_tool_is_mutate(const char *name)
{
    static const char *mutate[] = {
        "serial_send_command",
        "serial_write",
        "serial_add_autoresponder",
        "serial_pin_control",
        "serial_reset",
        "serial_sysrq",
        "serial_suspend",
        "serial_resume",
        "serial_add_watchdog",
        NULL,
    };
    if (!name)
        return 0;
    for (int i = 0; mutate[i]; i++) {
        if (strcmp(name, mutate[i]) == 0)
            return 1;
    }
    return 0;
}

cJSON *sm_mcp_build_tools_list(void)
{
    cJSON *tools = cJSON_CreateArray();
    int mutate = sm_mcp_mutate_enabled();

    /* serial_send_command */
    if (mutate) {
        cJSON *s = schema_object();
        cJSON *props = cJSON_CreateObject();
        schema_add_string(props, "command", "The command string to send.");
        schema_add_string(props, "expect_pattern",
            "Regex pattern to match end of response (default: from device profile).");
        schema_add_integer(props, "timeout_ms",
            "Timeout in milliseconds (default: from device profile).");
        schema_add_string(props, "eol",
            "Line ending appended to command: lf (default), cr, or crlf.");
        cJSON_AddItemToObject(s, "properties", props);
        cJSON *req = cJSON_CreateArray();
        cJSON_AddItemToArray(req, cJSON_CreateString("command"));
        cJSON_AddItemToObject(s, "required", req);
        cJSON_AddItemToArray(tools, make_tool("serial_send_command",
            "Send a command line and wait for its response (up to "
            "expect_pattern or the profile prompt). Use for request/response "
            "shells; use serial_write for raw bytes with no wait.", s));
    }

    /* serial_read */
    {
        cJSON *s = schema_object();
        cJSON_AddItemToObject(s, "properties", cJSON_CreateObject());
        cJSON_AddItemToArray(tools, make_tool_ro("serial_read",
            "Drain the output this MCP session buffered since the last "
            "read, without sending. Quick look after your own command; lossy "
            "across turns. Lossless capture: serial_output_history with "
            "since_seq. Waiting for known text: serial_wait_for.", s));
    }

    /* serial_write */
    if (mutate) {
        cJSON *s = schema_object();
        cJSON *props = cJSON_CreateObject();
        schema_add_string(props, "data", "The string to send (sent as-is).");
        cJSON_AddItemToObject(s, "properties", props);
        cJSON *req = cJSON_CreateArray();
        cJSON_AddItemToArray(req, cJSON_CreateString("data"));
        cJSON_AddItemToObject(s, "required", req);
        cJSON_AddItemToArray(tools, make_tool("serial_write",
            "Write raw data to the serial port without waiting for a response. "
            "Use for keystrokes and binary; use serial_send_command when you "
            "need the reply.", s));
    }

    /* serial_port_status */
    {
        cJSON *s = schema_object();
        cJSON_AddItemToObject(s, "properties", cJSON_CreateObject());
        cJSON_AddItemToArray(tools, make_tool_ro("serial_port_status",
            "Check the link before sending, or when output looks wrong: port, "
            "baud, connected/suspended, by-id identity strength, pins, who "
            "holds write control, and connected clients. Boot progress: "
            "serial_boot_status. Crashes: serial_get_incidents.", s));
    }

    /* serial_boot_status */
    {
        cJSON *s = schema_object();
        cJSON_AddItemToObject(s, "properties", cJSON_CreateObject());
        cJSON_AddItemToArray(tools, make_tool_ro("serial_boot_status",
            "Report cold-boot progress: which boot stages the device has reached, "
            "the furthest stage, and whether the boot has stalled. Requires the "
            "device profile to declare boot_stages; otherwise reports none. "
            "Use after a reset or power cycle; if stalled, check "
            "serial_get_incidents.", s));
    }

    /* serial_add_autoresponder */
    if (mutate) {
        cJSON *s = schema_object();
        cJSON *props = cJSON_CreateObject();
        schema_add_string(props, "name", "Unique name for this rule (re-adding "
            "the same name replaces it).");
        schema_add_string(props, "pattern",
            "Regex to match in device output.");
        schema_add_string(props, "send",
            "Bytes to send when it matches; \\n \\r \\t \\0 escapes are decoded "
            "(e.g. \"y\\n\").");
        schema_add_boolean(props, "once",
            "If true, the rule fires once then disables itself.");
        schema_add_integer(props, "cooldown_ms",
            "Minimum ms between fires on still-visible text (default 1000).");
        cJSON_AddItemToObject(s, "properties", props);
        cJSON *req = cJSON_CreateArray();
        cJSON_AddItemToArray(req, cJSON_CreateString("name"));
        cJSON_AddItemToArray(req, cJSON_CreateString("pattern"));
        cJSON_AddItemToArray(req, cJSON_CreateString("send"));
        cJSON_AddItemToObject(s, "required", req);
        cJSON_AddItemToArray(tools, make_tool("serial_add_autoresponder",
            "Register a standing expect->send rule: when the device output "
            "matches `pattern`, the broker auto-sends `send` (for boot menus, "
            "y/N prompts, unattended login) with no round-trip. For a single "
            "reply now, use serial_send_command or serial_write; to only "
            "watch for a pattern, serial_add_watchdog.", s));
    }

    /* serial_pin_control */
    if (mutate) {
        cJSON *s = schema_object();
        cJSON *props = cJSON_CreateObject();
        schema_add_string(props, "pin", "Pin to control: dtr, rts, or break.");
        schema_add_string(props, "action",
            "Action: set, clear, toggle, or pulse for dtr/rts "
            "(send aliases pulse: assert then deassert immediately; "
            "duration_ms is for break only). For break: send or pulse.");
        schema_add_integer(props, "duration_ms",
            "Break duration in ms (default 250).");
        cJSON_AddItemToObject(s, "properties", props);
        cJSON *req = cJSON_CreateArray();
        cJSON_AddItemToArray(req, cJSON_CreateString("pin"));
        cJSON_AddItemToArray(req, cJSON_CreateString("action"));
        cJSON_AddItemToObject(s, "required", req);
        cJSON_AddItemToArray(tools, make_tool_destr("serial_pin_control",
            "Control DTR/RTS or send a break. DTR/RTS often drive reset and "
            "boot-mode lines (ESP32, many dev boards), so a pulse can reset "
            "the target. For a Linux SysRq use serial_sysrq, which sends the "
            "break and the key together.", s));
    }

    /* serial_reset */
    if (mutate) {
        cJSON *s = schema_object();
        cJSON *props = cJSON_CreateObject();
        schema_add_string(props, "pin",
            "Reset line: rts (default; ESP32 dev kits, most CH340/CP210x "
            "auto-reset circuits) or dtr.");
        schema_add_integer(props, "hold_ms",
            "How long the reset line is held (default 100, max 2000).");
        schema_add_string(props, "wait_pattern",
            "Optional regex for a boot line to wait for after the reset "
            "(armed before the release, so a fast banner is not missed).");
        schema_add_integer(props, "timeout_ms",
            "With wait_pattern: max wait in ms (default 10000).");
        cJSON_AddItemToObject(s, "properties", props);
        cJSON_AddItemToArray(tools, make_tool_destr("serial_reset",
            "Reset the target through the USB-serial auto-reset circuit: "
            "clears the other modem line, holds the reset line, releases it. "
            "Use this instead of serial_pin_control pulses, which do nothing "
            "on ESP32-style boards while DTR is asserted (it is, right after "
            "the broker opens the port). Then check serial_boot_status.", s));
    }

    /* serial_sysrq */
    if (mutate) {
        cJSON *s = schema_object();
        cJSON *props = cJSON_CreateObject();
        schema_add_string(props, "key", "Single character SysRq key (e.g. h, b, t).");
        schema_add_integer(props, "break_duration_ms",
            "BREAK signal duration in ms (default 500).");
        schema_add_integer(props, "delay_ms",
            "Delay between BREAK and key in ms (default 100).");
        cJSON_AddItemToObject(s, "properties", props);
        cJSON *req = cJSON_CreateArray();
        cJSON_AddItemToArray(req, cJSON_CreateString("key"));
        cJSON_AddItemToObject(s, "required", req);
        cJSON_AddItemToArray(tools, make_tool_destr("serial_sysrq",
            "Send a Linux SysRq command (BREAK + key) to the serial device. "
            "Use when a Linux target hangs: t dumps tasks, w blocked tasks; "
            "b reboots at once.", s));
    }

    /* serial_suspend */
    if (mutate) {
        cJSON *s = schema_object();
        cJSON_AddItemToObject(s, "properties", cJSON_CreateObject());
        cJSON_AddItemToArray(tools, make_tool_destr("serial_suspend",
            "Close the serial port so an external tool (flasher, esptool, "
            "OpenOCD) can open it; clients stay connected. Call "
            "serial_resume afterwards.", s));
    }

    /* serial_resume */
    if (mutate) {
        cJSON *s = schema_object();
        cJSON_AddItemToObject(s, "properties", cJSON_CreateObject());
        cJSON_AddItemToArray(tools, make_tool("serial_resume",
            "Reopen the serial port after serial_suspend, once the external "
            "tool has exited.", s));
    }

    /* serial_wait_for */
    {
        cJSON *s = schema_object();
        cJSON *props = cJSON_CreateObject();
        schema_add_string(props, "pattern",
            "Regex to match in device output (listen-only; no TX).");
        schema_add_integer(props, "timeout_ms",
            "Max wait in ms (default 30000, clamp 100–3600000).");
        cJSON_AddItemToObject(s, "properties", props);
        cJSON *req = cJSON_CreateArray();
        cJSON_AddItemToArray(req, cJSON_CreateString("pattern"));
        cJSON_AddItemToObject(s, "required", req);
        cJSON_AddItemToArray(tools, make_tool_ro("serial_wait_for",
            "Wait for a regex in serial output without sending. Use when you "
            "know the text to expect (a prompt, login:, a log line). Works "
            "for observers. Ends early on a critical anomaly ([ABORTED "
            "anomaly:...]). Open-ended listening: serial_monitor. Output that "
            "already arrived: serial_output_history.", s));
    }

    /* serial_output_history */
    {
        cJSON *s = schema_object();
        cJSON *props = cJSON_CreateObject();
        schema_add_number(props, "seconds",
            "If > 0, return output from the last N seconds (prose).");
        schema_add_integer(props, "last_bytes",
            "If > 0, return the last N bytes of output (prose).");
        schema_add_number(props, "since_seq",
            "Cursor from a prior history response: lossless page as JSON "
            "(cursor/dropped/has_more/chunks). Prefer this over serial_read.");
        schema_add_integer(props, "max_bytes",
            "With since_seq: max raw bytes in this page (default broker cap).");
        cJSON_AddItemToObject(s, "properties", props);
        cJSON_AddItemToArray(tools, make_tool_ro("serial_output_history",
            "Read past output from the broker's shared history without "
            "consuming it. For anything you must not miss, page with since_seq "
            "(start at 0; pass the returned cursor back; check dropped and "
            "has_more). Without since_seq: text by seconds or last_bytes. "
            "serial_read is drain-only.", s));
    }

    /* serial_get_incidents */
    {
        cJSON *s = schema_object();
        cJSON *props = cJSON_CreateObject();
        schema_add_number(props, "seconds",
            "If > 0, only return incidents from the last N seconds.");
        cJSON_AddItemToObject(s, "properties", props);
        cJSON_AddItemToArray(tools, make_tool_ro("serial_get_incidents",
            "List anomalies the broker detected in output (kernel panic, "
            "oops, hard fault, assert, watchdog, brownout, ESP resets, plus "
            "profile patterns) with the match and pre-context. Use when a "
            "boot stalls, a wait aborts, or output looks like a crash; pair "
            "with serial_output_history for the surrounding log.", s));
    }

    /* serial_add_watchdog */
    if (mutate) {
        cJSON *s = schema_object();
        cJSON *props = cJSON_CreateObject();
        schema_add_string(props, "name", "Name for this watchdog pattern.");
        schema_add_string(props, "pattern",
            "Regex pattern to match in serial output.");
        schema_add_string(props, "severity",
            "Severity level: critical, warning, or info.");
        cJSON_AddItemToObject(s, "properties", props);
        cJSON *req = cJSON_CreateArray();
        cJSON_AddItemToArray(req, cJSON_CreateString("name"));
        cJSON_AddItemToArray(req, cJSON_CreateString("pattern"));
        cJSON_AddItemToObject(s, "required", req);
        cJSON_AddItemToArray(tools, make_tool("serial_add_watchdog",
            "Add a custom anomaly detection pattern; matches then appear in "
            "serial_get_incidents and can abort serial_wait_for.", s));
    }

    /* serial_monitor */
    {
        cJSON *s = schema_object();
        cJSON *props = cJSON_CreateObject();
        schema_add_integer(props, "duration_seconds",
            "How long to monitor (max 300 seconds, default 30).");
        cJSON_AddItemToObject(s, "properties", props);
        cJSON_AddItemToArray(tools, make_tool_ro("serial_monitor",
            "Listen for a fixed time (default 30 s, max 300 s) and return "
            "what the device printed plus anomalies in that window. Use when "
            "you do not know what to expect; when you do, serial_wait_for "
            "returns as soon as it matches.", s));
    }

    /* serial_generate_report */
    {
        cJSON *s = schema_object();
        cJSON_AddItemToObject(s, "properties", cJSON_CreateObject());
        cJSON_AddItemToArray(tools, make_tool_ro("serial_generate_report",
            "Markdown summary of device profile, port status and incidents. "
            "Use to open a debugging session or hand a human a snapshot; for "
            "one fact, call the single-purpose tool.", s));
    }

    /* serial_list_ports */
    {
        cJSON *s = schema_object();
        cJSON_AddItemToObject(s, "properties", cJSON_CreateObject());
        cJSON_AddItemToArray(tools, make_tool_ro("serial_list_ports",
            "List serial ports on this host with by-id paths and USB VID/PID "
            "(works without a broker). Use to pick a port before starting a "
            "broker, or when output is garbage (wrong port). Bridge chips "
            "name the adapter, not the MCU.", s));
    }

    return tools;
}
