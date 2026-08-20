#include "mcp_explain.h"
#include "anomaly.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Case-insensitive substring. */
static int has_ci(const char *hay, const char *needle)
{
    if (!hay || !needle || !needle[0])
        return 0;
    size_t nlen = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               tolower((unsigned char)p[i]) ==
                   tolower((unsigned char)needle[i]))
            i++;
        if (i == nlen)
            return 1;
    }
    return 0;
}

const char *sm_mcp_error_hint(const char *message)
{
    if (!message || !message[0])
        return "Check serial_port_status; start the broker if needed "
               "(smolmux <device> -b <baud>).";

    /* Before the generic "no broker" rule: a rejected handshake means the
     * broker IS running and reachable, so telling the agent to start one
     * would send it the wrong way entirely. */
    if (has_ci(message, "authentication failed") ||
        has_ci(message, "rejected this client") ||
        has_ci(message, "hello handshake"))
        return "The broker is running but refused this client. It was started "
               "with --auth-token (or SMOLMUX_AUTH_TOKEN), so this MCP server "
               "needs the same token in its environment: export "
               "SMOLMUX_AUTH_TOKEN=<token> where the MCP server is launched, "
               "then retry. Restarting the broker without a token also works "
               "on a trusted host.";

    if (has_ci(message, "no broker") || has_ci(message, "not connected to broker") ||
        has_ci(message, "broker socket") || has_ci(message, "offline"))
        return "Start a broker first: smolmux <device> -b <baud>. "
               "List live brokers with smolmux-monitor -L. "
               "This MCP does not auto-spawn brokers.";

    if (has_ci(message, "suspended"))
        return "Port was released for an external tool. Call serial_resume "
               "(or wait for the other client). Use serial_suspend only around "
               "flashers; CLI with-port always resumes.";

    if (has_ci(message, "disconnected") || has_ci(message, "link_down") ||
        has_ci(message, "not connected"))
        return "Link is down (unplug or USB re-enumeration, common after ESP32 "
               "native-USB reset). serial_list_ports; restart broker on the new "
               "path if the device node changed.";

    if (has_ci(message, "not authorized") || has_ci(message, "takeover") ||
        has_ci(message, "observer"))
        return "Another client holds write control. serial_port_status for "
               "clients; use takeover if you must write (controller role).";

    if (has_ci(message, "invalid regex") || has_ci(message, "invalid pattern"))
        return "Regex failed to compile. Escape metacharacters (., *, [, etc.) "
               "or use a simpler literal pattern.";

    if (has_ci(message, "ABORTED") || has_ci(message, "anomaly:"))
        return "A critical anomaly aborted the wait (crash/panic). "
               "serial_get_incidents and serial_output_history; fix firmware "
               "or power — do not treat as a quiet device.";

    if (has_ci(message, "TIMEOUT") || has_ci(message, "timeout"))
        return "No match before timeout. Check baud/profile, serial_get_incidents "
               "(crash-loop vs silence), and serial_output_history for what "
               "actually printed.";

    if (has_ci(message, "busy") || has_ci(message, "open fail") ||
        has_ci(message, "cannot open") || has_ci(message, "Permission denied") ||
        has_ci(message, "Device or resource busy"))
        return "Port open failed or device busy. Another process may hold the "
               "TTY; use by-id paths; suspend smolmux before external tools.";

    if (has_ci(message, "write failed") || has_ci(message, "failed to send"))
        return "Write failed. Check suspended/disconnected, takeover, and that "
               "the broker still owns the port.";

    if (has_ci(message, "failed to reopen"))
        return "Resume could not reopen the port (device gone or path wrong). "
               "serial_list_ports; restart broker if the node changed.";

    return NULL;
}

char *sm_mcp_error_with_hint(const char *message)
{
    const char *msg = message && message[0] ? message : "[ERROR] unknown error";
    const char *hint = sm_mcp_error_hint(msg);
    if (!hint)
        return strdup(msg);

    size_t need = strlen(msg) + strlen(hint) + 16;
    char *out = malloc(need);
    if (!out)
        return strdup(msg);
    snprintf(out, need, "%s\nHint: %s", msg, hint);
    return out;
}

char *sm_mcp_offline_broker_message(void)
{
    return sm_mcp_error_with_hint(
        "[ERROR] no broker socket — smolmux broker is not running or not "
        "reachable");
}

char *sm_mcp_maybe_explain_result(char *text)
{
    if (!text)
        return sm_mcp_error_with_hint("[ERROR] unknown error");
    if (strncmp(text, "[ERROR]", 7) == 0 ||
        strncmp(text, "[TIMEOUT]", 9) == 0 ||
        strncmp(text, "[ABORTED", 8) == 0) {
        char *out = sm_mcp_error_with_hint(text);
        free(text);
        return out;
    }
    return text;
}

char *sm_mcp_append_recent_incidents(char *text,
                                     const struct sm_anomaly_incident *incs_void,
                                     size_t count, size_t max_n)
{
    const sm_anomaly_incident_t *incs = (const sm_anomaly_incident_t *)incs_void;
    if (!text)
        text = strdup("(no output)");
    if (!incs || count == 0 || max_n == 0)
        return text;

    size_t start = count > max_n ? count - max_n : 0;
    size_t need = strlen(text) + 64 + (count - start) * 200;
    char *out = malloc(need);
    if (!out)
        return text;
    size_t off = 0;
    off += (size_t)snprintf(out + off, need - off, "%s", text);
    off += (size_t)snprintf(out + off, need - off, "\n\n## Recent incidents\n");
    for (size_t i = start; i < count; i++) {
        char snip[80];
        size_t ml = strlen(incs[i].match_text);
        if (ml >= sizeof(snip))
            ml = sizeof(snip) - 1;
        memcpy(snip, incs[i].match_text, ml);
        snip[ml] = '\0';
        /* one line */
        for (char *p = snip; *p; p++)
            if (*p == '\n' || *p == '\r')
                *p = ' ';
        off += (size_t)snprintf(out + off, need - off, "- %s [%s]: %s\n",
                                incs[i].pattern_name, incs[i].severity, snip);
        if (off + 8 >= need)
            break;
    }
    free(text);
    return out;
}
