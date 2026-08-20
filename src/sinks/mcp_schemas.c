/* MCP tool schemas — shared by the in-broker MCP sink (mcp.c) and the
 * standalone smolmux-mcp binary (mcp_client.c). Extracted from the two
 * previously-duplicated tool-list builders so they cannot drift. */
#include "sinks/mcp_schemas.h"

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
        cJSON_AddItemToObject(s, "properties", props);
        cJSON *req = cJSON_CreateArray();
        cJSON_AddItemToArray(req, cJSON_CreateString("command"));
        cJSON_AddItemToObject(s, "required", req);
        cJSON_AddItemToArray(tools, make_tool("serial_send_command",
            "Send a command to the serial device and wait for a response.", s));
    }

    /* serial_read */
    {
        cJSON *s = schema_object();
        cJSON_AddItemToObject(s, "properties", cJSON_CreateObject());
        cJSON_AddItemToArray(tools, make_tool_ro("serial_read",
            "Read buffered serial output without sending anything "
            "(session drain; prefer history for lossless capture).", s));
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
            "Write raw data to the serial port without waiting for a response.", s));
    }

    /* serial_port_status */
    {
        cJSON *s = schema_object();
        cJSON_AddItemToObject(s, "properties", cJSON_CreateObject());
        cJSON_AddItemToArray(tools, make_tool_ro("serial_port_status",
            "Get the current status of the serial port and connected clients.", s));
    }

    /* serial_boot_status */
    {
        cJSON *s = schema_object();
        cJSON_AddItemToObject(s, "properties", cJSON_CreateObject());
        cJSON_AddItemToArray(tools, make_tool_ro("serial_boot_status",
            "Report cold-boot progress: which boot stages the device has reached, "
            "the furthest stage, and whether the boot has stalled. Requires the "
            "device profile to declare boot_stages; otherwise reports none.", s));
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
            "y/N prompts, unattended login) with no round-trip.", s));
    }

    /* serial_pin_control */
    if (mutate) {
        cJSON *s = schema_object();
        cJSON *props = cJSON_CreateObject();
        schema_add_string(props, "pin", "Pin to control: dtr, rts, or break.");
        schema_add_string(props, "action",
            "Action: set, clear, toggle (for dtr/rts), or send (for break).");
        schema_add_integer(props, "duration_ms",
            "Break duration in ms (default 250).");
        cJSON_AddItemToObject(s, "properties", props);
        cJSON *req = cJSON_CreateArray();
        cJSON_AddItemToArray(req, cJSON_CreateString("pin"));
        cJSON_AddItemToArray(req, cJSON_CreateString("action"));
        cJSON_AddItemToObject(s, "required", req);
        cJSON_AddItemToArray(tools, make_tool_destr("serial_pin_control",
            "Control serial port pins or send a break signal.", s));
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
            "Send a Linux SysRq command (BREAK + key) to the serial device.", s));
    }

    /* serial_suspend */
    if (mutate) {
        cJSON *s = schema_object();
        cJSON_AddItemToObject(s, "properties", cJSON_CreateObject());
        cJSON_AddItemToArray(tools, make_tool_destr("serial_suspend",
            "Suspend the serial port so external tools can access the device.", s));
    }

    /* serial_resume */
    if (mutate) {
        cJSON *s = schema_object();
        cJSON_AddItemToObject(s, "properties", cJSON_CreateObject());
        cJSON_AddItemToArray(tools, make_tool("serial_resume",
            "Resume the serial port after a suspend.", s));
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
            "Wait for a regex in serial output without sending. Works for "
            "observers. Ends early on critical anomaly (ABORTED).", s));
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
            "Non-destructive history. With since_seq: JSON cursor page. "
            "Without: prose by time/bytes. serial_read is drain-only.", s));
    }

    /* serial_get_incidents */
    {
        cJSON *s = schema_object();
        cJSON *props = cJSON_CreateObject();
        schema_add_number(props, "seconds",
            "If > 0, only return incidents from the last N seconds.");
        cJSON_AddItemToObject(s, "properties", props);
        cJSON_AddItemToArray(tools, make_tool_ro("serial_get_incidents",
            "Get detected anomalies/crashes from the broker.", s));
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
            "Add a custom anomaly detection pattern.", s));
    }

    /* serial_monitor */
    {
        cJSON *s = schema_object();
        cJSON *props = cJSON_CreateObject();
        schema_add_integer(props, "duration_seconds",
            "How long to monitor (max 300 seconds, default 30).");
        cJSON_AddItemToObject(s, "properties", props);
        cJSON_AddItemToArray(tools, make_tool_ro("serial_monitor",
            "Monitor serial output for a duration, returning output and anomalies.", s));
    }

    /* serial_generate_report */
    {
        cJSON *s = schema_object();
        cJSON_AddItemToObject(s, "properties", cJSON_CreateObject());
        cJSON_AddItemToArray(tools, make_tool_ro("serial_generate_report",
            "Generate a status report for the serial device.", s));
    }

    /* serial_list_ports */
    {
        cJSON *s = schema_object();
        cJSON_AddItemToObject(s, "properties", cJSON_CreateObject());
        cJSON_AddItemToArray(tools, make_tool_ro("serial_list_ports",
            "List serial ports with by-id and USB VID/PID when available. "
            "Bridge chips name the adapter, not the MCU.", s));
    }

    return tools;
}
