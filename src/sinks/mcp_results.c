/* Structured serial MCP tool results (outputSchema + structuredContent),
 * shared by the in-broker MCP sink and the standalone smolmux-mcp. */
#include "sinks/mcp_results.h"

#include "broker_info.h"
#include "util/json_helpers.h"
#include "util/sock_util.h"
#include "util/str.h"

#include <stdlib.h>
#include <string.h>

/* --- schema helpers --- */

static cJSON *prop(cJSON *props, const char *name, const char *type)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", type);
    cJSON_AddItemToObject(props, name, p);
    return p;
}

static void prop_nullable_string(cJSON *props, const char *name)
{
    cJSON *p = cJSON_CreateObject();
    const char *types[] = {"string", "null"};
    cJSON_AddItemToObject(p, "type", cJSON_CreateStringArray(types, 2));
    cJSON_AddItemToObject(props, name, p);
}

static void prop_enum(cJSON *props, const char *name,
                      const char *const *values, int n)
{
    cJSON *p = prop(props, name, "string");
    cJSON_AddItemToObject(p, "enum", cJSON_CreateStringArray(values, n));
}

static cJSON *object_schema(cJSON **props_out)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddStringToObject(s, "type", "object");
    *props_out = cJSON_AddObjectToObject(s, "properties");
    return s;
}

static void set_required(cJSON *schema, const char *const *names, int n)
{
    cJSON_AddItemToObject(schema, "required", cJSON_CreateStringArray(names, n));
}

static cJSON *array_of(cJSON *props, const char *name, cJSON **item_props)
{
    cJSON *a = prop(props, name, "array");
    cJSON *items = object_schema(item_props);
    cJSON_AddItemToObject(a, "items", items);
    return items;
}

static const char *const boot_states[] = {
    "unconfigured", "not started", "in progress", "stalled", "complete",
};

static cJSON *port_status_schema(void)
{
    cJSON *props;
    cJSON *s = object_schema(&props);
    prop(props, "port", "string");
    prop(props, "baud", "integer");
    prop(props, "connected", "boolean");
    prop(props, "suspended", "boolean");
    prop(props, "link_type", "string");
    prop_nullable_string(props, "takeover_client");
    prop(props, "identity_strength", "string");
    prop(props, "identity_by_id", "string");
    prop(props, "identity_by_path", "string");
    prop(props, "link_up_ts", "number");
    prop(props, "last_rx_age_ms", "number");
    prop(props, "bytes_rx_since_link_up", "number");
    prop(props, "last_link_event", "string");
    prop(props, "pin_states", "object");
    prop(props, "log_path", "string");
    prop(props, "board", "string");
    prop(props, "role", "string");
    cJSON *cprops;
    cJSON *items = array_of(props, "clients", &cprops);
    prop(cprops, "name", "string");
    prop(cprops, "role", "string");
    prop(cprops, "pid", "integer");
    const char *ireq[] = {"name", "role"};
    set_required(items, ireq, 2);
    const char *req[] = {"port", "baud", "connected", "suspended",
                         "takeover_client", "clients"};
    set_required(s, req, 6);
    return s;
}

static cJSON *boot_status_schema(void)
{
    cJSON *props;
    cJSON *s = object_schema(&props);
    prop(props, "configured", "boolean");
    prop_enum(props, "state", boot_states, 5);
    prop(props, "reached", "integer");
    prop(props, "total", "integer");
    prop_nullable_string(props, "furthest");
    cJSON *sprops;
    cJSON *items = array_of(props, "stages", &sprops);
    prop(sprops, "name", "string");
    prop(sprops, "reached", "boolean");
    prop(sprops, "timestamp", "number");
    const char *ireq[] = {"name", "reached"};
    set_required(items, ireq, 2);
    const char *req[] = {"configured", "state", "reached", "total",
                         "furthest", "stages"};
    set_required(s, req, 6);
    return s;
}

static cJSON *incidents_schema(void)
{
    cJSON *props;
    cJSON *s = object_schema(&props);
    prop(props, "count", "integer");
    cJSON *iprops;
    cJSON *items = array_of(props, "incidents", &iprops);
    prop(iprops, "incident_id", "string");
    prop(iprops, "pattern_name", "string");
    prop(iprops, "severity", "string");
    prop(iprops, "timestamp", "number");
    prop(iprops, "match_text", "string");
    prop(iprops, "pre_context", "string");
    const char *ireq[] = {"pattern_name", "severity", "timestamp",
                          "match_text", "pre_context"};
    set_required(items, ireq, 5);
    const char *req[] = {"count", "incidents"};
    set_required(s, req, 2);
    return s;
}

static cJSON *list_ports_schema(void)
{
    cJSON *props;
    cJSON *s = object_schema(&props);
    cJSON *pprops;
    cJSON *items = array_of(props, "ports", &pprops);
    prop(pprops, "path", "string");
    prop(pprops, "by_id", "string");
    prop(pprops, "by_path", "string");
    prop(pprops, "vid", "string");
    prop(pprops, "pid", "string");
    prop(pprops, "manufacturer", "string");
    prop(pprops, "product", "string");
    const char *ids[] = {"STRONG", "WEAK", "n/a"};
    prop_enum(pprops, "identity", ids, 3);
    const char *ireq[] = {"path", "vid", "pid", "identity"};
    set_required(items, ireq, 4);
    const char *req[] = {"ports"};
    set_required(s, req, 1);
    return s;
}

static cJSON *history_schema(void)
{
    cJSON *props;
    cJSON *s = object_schema(&props);
    const char *modes[] = {"cursor", "text"};
    prop_enum(props, "mode", modes, 2);
    prop(props, "text", "string");
    prop(props, "cursor", "integer");
    prop(props, "dropped", "integer");
    prop(props, "has_more", "boolean");
    cJSON *cprops;
    cJSON *items = array_of(props, "chunks", &cprops);
    prop(cprops, "text", "string");
    prop(cprops, "timestamp", "number");
    prop(cprops, "seq_start", "integer");
    const char *ireq[] = {"text", "timestamp"};
    set_required(items, ireq, 2);
    const char *req[] = {"mode"};
    set_required(s, req, 1);
    return s;
}

cJSON *sm_mcp_output_schema(const char *tool)
{
    if (!tool) return NULL;
    if (strcmp(tool, "serial_port_status") == 0)    return port_status_schema();
    if (strcmp(tool, "serial_boot_status") == 0)    return boot_status_schema();
    if (strcmp(tool, "serial_get_incidents") == 0)  return incidents_schema();
    if (strcmp(tool, "serial_list_ports") == 0)     return list_ports_schema();
    if (strcmp(tool, "serial_output_history") == 0) return history_schema();
    return NULL;
}

int sm_mcp_tool_has_output_schema(const char *tool)
{
    cJSON *s = sm_mcp_output_schema(tool);
    cJSON_Delete(s);
    return s != NULL;
}

cJSON *sm_mcp_tool_call_result(const char *tool, const char *text,
                               cJSON *structured)
{
    if (!text) text = "(allocation failed)";
    cJSON *result = cJSON_CreateObject();
    cJSON *content = cJSON_AddArrayToObject(result, "content");
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text);
    cJSON_AddItemToArray(content, item);

    int is_error = strncmp(text, "[ERROR]", 7) == 0;
    if (!structured && sm_mcp_tool_has_output_schema(tool))
        is_error = 1;
    if (structured && !is_error)
        cJSON_AddItemToObject(result, "structuredContent", structured);
    else
        cJSON_Delete(structured);
    if (is_error)
        cJSON_AddBoolToObject(result, "isError", 1);
    return result;
}

/* --- small JSON helpers --- */

static char *steal_or_fallback(sm_strbuf_t *sb)
{
    char *out = sm_strbuf_steal(sb);
    return out ? out : strdup("(allocation failed)");
}

static void copy_string(cJSON *dst, const cJSON *src, const char *key)
{
    const char *v = sm_json_get_string(src, key);
    if (v)
        cJSON_AddStringToObject(dst, key, v);
}

static void copy_number(cJSON *dst, const cJSON *src, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(src, key);
    if (cJSON_IsNumber(v))
        cJSON_AddNumberToObject(dst, key, v->valuedouble);
}

/* --- serial_port_status --- */

static const char *identity_strength(const cJSON *status)
{
    const char *s = sm_json_get_string(status, "identity_strength");
    if (s) return s;
    return sm_json_get_bool(status, "identity_weak", 0) ? "WEAK" : "n/a";
}

char *sm_mcp_port_status_text(const cJSON *status, const char *last_link_event)
{
    sm_strbuf_t sb;
    sm_strbuf_init(&sb);
    const char *port = sm_json_get_string(status, "port");
    sm_strbuf_printf(&sb,
        "Port: %s\nBaud: %d\nConnected: %s\nSuspended: %s\n"
        "Link up ts: %.3f\nLast RX age ms: %d\nBytes since link up: %.0f\n",
        port ? port : "?",
        sm_json_get_int(status, "baud", 0),
        sm_json_get_bool(status, "connected", 0) ? "true" : "false",
        sm_json_get_bool(status, "suspended", 0) ? "true" : "false",
        sm_json_get_double(status, "link_up_ts", 0.0),
        sm_json_get_int(status, "last_rx_age_ms", 0),
        sm_json_get_double(status, "bytes_rx_since_link_up", 0.0));
    if (last_link_event)
        sm_strbuf_printf(&sb, "Last link event: %s\n",
                         last_link_event[0] ? last_link_event : "(none)");
    sm_strbuf_printf(&sb, "Identity: %s\n", identity_strength(status));
    const char *by_id = sm_json_get_string(status, "identity_by_id");
    if (by_id && by_id[0])
        sm_strbuf_printf(&sb, "Identity by-id: %s\n", by_id);
    const char *by_path = sm_json_get_string(status, "identity_by_path");
    if (by_path && by_path[0])
        sm_strbuf_printf(&sb, "Identity by-path: %s\n", by_path);

    const cJSON *pins = cJSON_GetObjectItemCaseSensitive(status, "pin_states");
    if (pins) {
        char *pin_str = cJSON_PrintUnformatted(pins);
        sm_strbuf_printf(&sb, "Pin states: %s\n", pin_str ? pin_str : "{}");
        free(pin_str);
    }

    const char *takeover = sm_json_get_string(status, "takeover_client");
    sm_strbuf_printf(&sb, "Takeover: %s\n", takeover ? takeover : "none");

    const char *log_path = sm_json_get_string(status, "log_path");
    if (log_path)
        sm_strbuf_printf(&sb, "Log: %s\n", log_path);

    const cJSON *clients = cJSON_GetObjectItemCaseSensitive(status, "clients");
    sm_strbuf_printf(&sb, "Clients:\n");
    if (cJSON_IsArray(clients)) {
        const cJSON *ci;
        cJSON_ArrayForEach(ci, clients) {
            const char *cname = sm_json_get_string(ci, "name");
            const char *crole = sm_json_get_string(ci, "role");
            sm_strbuf_printf(&sb, "  - %s (%s)\n",
                             cname ? cname : "?", crole ? crole : "?");
        }
    }
    return steal_or_fallback(&sb);
}

cJSON *sm_mcp_port_status_structured(const cJSON *status,
                                     const char *last_link_event)
{
    /* Built field by field, not copied: link get_status() adds its own keys
     * to status_response (serial-tcp even a numeric "port"), and a copied
     * duplicate key would break the declared types. First match wins here,
     * which is the broker's own value. */
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    const char *port = sm_json_get_string(status, "port");
    cJSON_AddStringToObject(o, "port", port ? port : "");
    cJSON_AddNumberToObject(o, "baud", sm_json_get_int(status, "baud", 0));
    cJSON_AddBoolToObject(o, "connected", sm_json_get_bool(status, "connected", 0));
    cJSON_AddBoolToObject(o, "suspended", sm_json_get_bool(status, "suspended", 0));
    copy_string(o, status, "link_type");
    const char *takeover = sm_json_get_string(status, "takeover_client");
    if (takeover)
        cJSON_AddStringToObject(o, "takeover_client", takeover);
    else
        cJSON_AddNullToObject(o, "takeover_client");
    cJSON_AddStringToObject(o, "identity_strength", identity_strength(status));
    copy_string(o, status, "identity_by_id");
    copy_string(o, status, "identity_by_path");
    copy_number(o, status, "link_up_ts");
    copy_number(o, status, "last_rx_age_ms");
    copy_number(o, status, "bytes_rx_since_link_up");
    if (last_link_event && last_link_event[0])
        cJSON_AddStringToObject(o, "last_link_event", last_link_event);
    const cJSON *pins = cJSON_GetObjectItemCaseSensitive(status, "pin_states");
    if (cJSON_IsObject(pins))
        cJSON_AddItemToObject(o, "pin_states", cJSON_Duplicate(pins, 1));
    copy_string(o, status, "log_path");
    copy_string(o, status, "board");
    copy_string(o, status, "role");

    cJSON *out = cJSON_AddArrayToObject(o, "clients");
    const cJSON *clients = cJSON_GetObjectItemCaseSensitive(status, "clients");
    const cJSON *ci;
    cJSON_ArrayForEach(ci, clients) {
        cJSON *c = cJSON_CreateObject();
        const char *cname = sm_json_get_string(ci, "name");
        const char *crole = sm_json_get_string(ci, "role");
        cJSON_AddStringToObject(c, "name", cname ? cname : "");
        cJSON_AddStringToObject(c, "role", crole ? crole : "");
        copy_number(c, ci, "pid");
        cJSON_AddItemToArray(out, c);
    }
    return o;
}

/* --- serial_boot_status --- */

typedef struct boot_view {
    const cJSON *boot;    /* NULL when the profile declares no stages */
    const cJSON *stages;
    int furthest, total, reached;
    const char *furthest_name;
    const char *state;    /* one of boot_states */
} boot_view_t;

static boot_view_t boot_view(const cJSON *status)
{
    boot_view_t v = {0};
    v.boot = cJSON_GetObjectItemCaseSensitive(status, "boot");
    v.furthest = -1;
    v.state = boot_states[0];
    if (!cJSON_IsObject(v.boot)) {
        v.boot = NULL;
        return v;
    }
    v.furthest = sm_json_get_int(v.boot, "furthest", -1);
    v.total = sm_json_get_int(v.boot, "total", 0);
    v.stages = cJSON_GetObjectItemCaseSensitive(v.boot, "stages");
    int idx = 0;
    const cJSON *st;
    cJSON_ArrayForEach(st, v.stages) {
        if (sm_json_get_bool(st, "reached", 0)) v.reached++;
        if (idx == v.furthest) v.furthest_name = sm_json_get_string(st, "name");
        idx++;
    }
    if (sm_json_get_bool(v.boot, "terminal_reached", 0))
        v.state = boot_states[4];
    else if (sm_json_get_bool(v.boot, "stalled", 0))
        v.state = boot_states[3];
    else
        v.state = v.furthest < 0 ? boot_states[1] : boot_states[2];
    return v;
}

char *sm_mcp_boot_status_text(const cJSON *status)
{
    boot_view_t v = boot_view(status);
    if (!v.boot)
        return strdup("No boot_stages defined in the device profile — "
                      "boot progress tracking is not configured.");

    sm_strbuf_t sb;
    sm_strbuf_init(&sb);
    sm_strbuf_printf(&sb, "Boot: %d/%d stages reached", v.reached, v.total);
    if (v.furthest_name) sm_strbuf_printf(&sb, " (furthest: %s)", v.furthest_name);
    sm_strbuf_printf(&sb, ", state: %s\n",
                     strcmp(v.state, "stalled") == 0 ? "STALLED" : v.state);
    int idx = 0;
    const cJSON *st;
    cJSON_ArrayForEach(st, v.stages) {
        const char *nm = sm_json_get_string(st, "name");
        sm_strbuf_printf(&sb, "  [%c] %s%s\n",
                         sm_json_get_bool(st, "reached", 0) ? 'x' : ' ',
                         nm ? nm : "?", idx == v.furthest ? "  <- furthest" : "");
        idx++;
    }
    return steal_or_fallback(&sb);
}

cJSON *sm_mcp_boot_status_structured(const cJSON *status)
{
    boot_view_t v = boot_view(status);
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddBoolToObject(o, "configured", v.boot != NULL);
    cJSON_AddStringToObject(o, "state", v.state);
    cJSON_AddNumberToObject(o, "reached", v.reached);
    cJSON_AddNumberToObject(o, "total", v.total);
    if (v.furthest_name)
        cJSON_AddStringToObject(o, "furthest", v.furthest_name);
    else
        cJSON_AddNullToObject(o, "furthest");
    cJSON *out = cJSON_AddArrayToObject(o, "stages");
    const cJSON *st;
    cJSON_ArrayForEach(st, v.stages) {
        cJSON *s = cJSON_CreateObject();
        const char *nm = sm_json_get_string(st, "name");
        cJSON_AddStringToObject(s, "name", nm ? nm : "");
        cJSON_AddBoolToObject(s, "reached", sm_json_get_bool(st, "reached", 0));
        copy_number(s, st, "timestamp");
        cJSON_AddItemToArray(out, s);
    }
    return o;
}

/* --- serial_get_incidents --- */

char *sm_mcp_incidents_text(const cJSON *incidents)
{
    if (!cJSON_IsArray(incidents) || cJSON_GetArraySize(incidents) == 0)
        return strdup("No anomalies detected.");

    /* sm_strbuf: a fixed count*512 budget once underflowed when one
     * incident's match_text + pre_context passed the cap. */
    sm_strbuf_t sb;
    sm_strbuf_init(&sb);
    int num = 0;
    const cJSON *inc;
    cJSON_ArrayForEach(inc, incidents) {
        num++;
        const char *pname = sm_json_get_string(inc, "pattern_name");
        const char *sev = sm_json_get_string(inc, "severity");
        const char *match = sm_json_get_string(inc, "match_text");
        const char *pre = sm_json_get_string(inc, "pre_context");
        sm_strbuf_printf(&sb, "### Incident %d: %s [%s]\nMatch: %s\n",
                         num, pname ? pname : "?", sev ? sev : "?",
                         match ? match : "");
        if (pre && pre[0])
            sm_strbuf_printf(&sb, "Pre-context:\n```\n%s\n```\n", pre);
        sm_strbuf_printf(&sb, "\n");
    }
    return steal_or_fallback(&sb);
}

cJSON *sm_mcp_incidents_structured(const cJSON *incidents)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON *out = cJSON_AddArrayToObject(o, "incidents");
    int n = 0;
    const cJSON *inc;
    cJSON_ArrayForEach(inc, incidents) {
        cJSON *i = cJSON_CreateObject();
        copy_string(i, inc, "incident_id");
        const char *keys[] = {"pattern_name", "severity", "match_text",
                              "pre_context"};
        for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
            const char *v = sm_json_get_string(inc, keys[k]);
            cJSON_AddStringToObject(i, keys[k], v ? v : "");
        }
        cJSON_AddNumberToObject(i, "timestamp",
                                sm_json_get_double(inc, "timestamp", 0.0));
        cJSON_AddItemToArray(out, i);
        n++;
    }
    cJSON_AddNumberToObject(o, "count", n);
    return o;
}

/* --- serial_list_ports --- */

char *sm_mcp_list_ports(cJSON **structured)
{
    sm_serial_port_info_t infos[SM_SERIAL_PORT_INFO_MAX];
    size_t n = sm_list_serial_ports_info(infos, SM_SERIAL_PORT_INFO_MAX);
    if (structured) {
        cJSON *o = cJSON_CreateObject();
        cJSON *ports = o ? cJSON_AddArrayToObject(o, "ports") : NULL;
        for (size_t i = 0; ports && i < n; i++)
            cJSON_AddItemToArray(ports, sm_serial_port_info_to_json(&infos[i]));
        *structured = o;
    }
    return sm_format_serial_ports_info_text(infos, n);
}

/* --- serial_output_history --- */

char *sm_mcp_history_page_text(const cJSON *page)
{
    char *out = cJSON_PrintUnformatted(page);
    return out ? out : strdup("(allocation failed)");
}

cJSON *sm_mcp_history_page_structured(const cJSON *page)
{
    cJSON *o = cJSON_Duplicate(page, 1);
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "mode", "cursor");
    return o;
}

cJSON *sm_mcp_history_text_structured(const char *text)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "mode", "text");
    cJSON_AddStringToObject(o, "text", text ? text : "");
    return o;
}
