#include "mcp_instructions.h"
#include "util/str.h"
#include "util/json_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Wave 1 transfer from Arduino-Agent agent-UX patterns (re-implemented, not
 * copied). Keep short: clients inject this on every initialize.
 */
static const char SM_SERIAL_INSTRUCTIONS[] =
    "smolmux serial MCP — multiplexed access to a live device console held "
    "open by a smolmux broker. Prefer these tools over pasting serial "
    "traffic by hand.\n"
    "\n"
    "Recommended workflow:\n"
    "1. serial_port_status and serial_boot_status before thrashing writes.\n"
    "2. serial_send_command for interactive shells (command + expect); "
    "serial_write for raw; serial_wait_for for listen-only regex (no TX); "
    "serial_monitor for a timed listen window.\n"
    "3. Lossless capture: serial_output_history with since_seq (JSON page: "
    "cursor/dropped/has_more). Pass cursor back as since_seq. serial_read "
    "only drains the session buffer — not lossless across turns.\n"
    "4. Crashes: serial_get_incidents plus history. wait_for/send_command "
    "may return [ABORTED anomaly:…] on critical panics — not silence.\n"
    "5. Flash coexistence: serial_suspend, then flasher (or CLI with-port), "
    "then serial_resume. Do not fight TIOCEXCL on the port.\n"
    "6. Multi-client: another controller may hold write rights; if writes "
    "fail, check status / takeover.\n"
    "7. Baud must match firmware and the broker profile; garbage text is "
    "often the wrong baud or the wrong port (list_ports, by-id paths).\n";

const char *sm_mcp_serial_instructions(void)
{
    return SM_SERIAL_INSTRUCTIONS;
}

static const char *const PROMPT_NAMES[SM_MCP_SERIAL_PROMPT_COUNT] = {
    "bringup",
    "debug_serial",
    "flash_safe",
};

const char *const *sm_mcp_serial_prompt_names(size_t *count)
{
    if (count)
        *count = SM_MCP_SERIAL_PROMPT_COUNT;
    return PROMPT_NAMES;
}

static cJSON *prompt_arg(const char *name, const char *desc, int required)
{
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "name", name);
    cJSON_AddStringToObject(a, "description", desc);
    cJSON_AddBoolToObject(a, "required", required ? 1 : 0);
    return a;
}

cJSON *sm_mcp_serial_prompts_list_result(void)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(result, "prompts");

    /* bringup */
    {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "name", "bringup");
        cJSON_AddStringToObject(p, "description",
            "Bring up a board on the smolmux serial broker: status, boot "
            "stages, and proof of life.");
        cJSON *args = cJSON_AddArrayToObject(p, "arguments");
        cJSON_AddItemToArray(args, prompt_arg("port",
            "Hint only (broker already owns a port); omit to use status.",
            0));
        cJSON_AddItemToArray(arr, p);
    }

    /* debug_serial */
    {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "name", "debug_serial");
        cJSON_AddStringToObject(p, "description",
            "Capture serial output and diagnose crashes, stalls, or noise.");
        cJSON *args = cJSON_AddArrayToObject(p, "arguments");
        cJSON_AddItemToArray(args, prompt_arg("seconds",
            "How long to monitor if needed (default 30).", 0));
        cJSON_AddItemToArray(arr, p);
    }

    /* flash_safe */
    {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "name", "flash_safe");
        cJSON_AddStringToObject(p, "description",
            "Release the port for an external flasher, then resume and "
            "verify boot.");
        cJSON_AddArrayToObject(p, "arguments");
        cJSON_AddItemToArray(arr, p);
    }

    return result;
}

static char *build_bringup(const char *port_hint)
{
    sm_strbuf_t sb;
    sm_strbuf_init(&sb);
    sm_strbuf_printf(&sb,
        "Bring up the device on the smolmux serial broker%s%s%s "
        "end to end:\n"
        "\n"
        "1. serial_port_status — confirm the link is up, baud, clients, "
        "and whether the port is suspended.\n"
        "2. serial_list_ports only if you need to choose another device "
        "for a new broker; this MCP talks to the already-running broker.\n"
        "3. serial_boot_status — report furthest boot stage / stall if "
        "the device profile declares boot_stages.\n"
        "4. If the board needs a hard reset: serial_pin_control "
        "(dtr/rts/break) as appropriate for the board, then re-check "
        "serial_boot_status and serial_output_history.\n"
        "5. Proof of life: serial_monitor briefly, or serial_read after "
        "history, looking for a banner, prompt, or heartbeat line. "
        "If silent, serial_get_incidents — a crash-loop is not quiet.\n"
        "6. Report: port, baud, boot furthest stage, and the proof line.\n",
        port_hint && port_hint[0] ? " (user port hint: " : "",
        port_hint && port_hint[0] ? port_hint : "",
        port_hint && port_hint[0] ? ")" : "");
    char *out = strdup(sb.data ? sb.data : "");
    sm_strbuf_destroy(&sb);
    return out;
}

static char *build_debug_serial(const char *seconds)
{
    const char *sec = (seconds && seconds[0]) ? seconds : "30";
    sm_strbuf_t sb;
    sm_strbuf_init(&sb);
    sm_strbuf_printf(&sb,
        "Diagnose what the device is doing on serial:\n"
        "\n"
        "1. serial_port_status — connected? suspended? baud?\n"
        "2. serial_output_history with since_seq=0 (or omit for prose tail) "
        "— non-destructive; keep cursor for lossless paging.\n"
        "3. serial_get_incidents — anomalies/crashes the broker already "
        "detected (Guru Meditation, brownout, kernel panic, etc.).\n"
        "4. For a specific line: serial_wait_for pattern=... (listen-only). "
        "Critical anomalies abort early with [ABORTED anomaly:…].\n"
        "5. Timed window: serial_monitor duration_seconds=%s. Interactive: "
        "serial_send_command; raw: serial_write. Avoid serial_read alone "
        "for capture (it drains).\n"
        "6. Report: last meaningful output, any incidents with severity, "
        "and a concrete next step (wrong baud, crash-loop, boot stall, "
        "or healthy idle).\n",
        sec);
    char *out = strdup(sb.data ? sb.data : "");
    sm_strbuf_destroy(&sb);
    return out;
}

static char *build_flash_safe(void)
{
    return strdup(
        "Flash or run an external tool that needs exclusive access to the "
        "same serial port the broker holds:\n"
        "\n"
        "1. serial_port_status — confirm you can control the session "
        "(not observer-only).\n"
        "2. serial_suspend — broker closes the port (TIOCEXCL released) "
        "so esptool/OpenOCD/avrdude/etc. can open it.\n"
        "3. Run the external flasher/tool (user or shell). Prefer CLI "
        "smolmux-cli with-port <cmd> when available: it always resumes.\n"
        "4. serial_resume — broker reopens the port.\n"
        "5. serial_boot_status and serial_output_history (or a short "
        "serial_monitor) to prove the new firmware is alive.\n"
        "6. If resume fails or the port vanished (USB re-enum): recheck "
        "serial_list_ports and restart the broker on the new path if "
        "needed.\n"
        "Never leave the port suspended after a failed flash without "
        "saying so.\n");
}

int sm_mcp_serial_prompt_get(const char *name, const cJSON *args,
                             char **out_text, const char **out_desc)
{
    if (!name || !out_text || !out_desc)
        return -1;
    *out_text = NULL;
    *out_desc = NULL;

    const char *port = NULL;
    const char *seconds = NULL;
    char secbuf[32];
    if (args) {
        port = sm_json_get_string((cJSON *)args, "port");
        seconds = sm_json_get_string((cJSON *)args, "seconds");
        /* MCP clients sometimes pass numeric seconds */
        if (!seconds) {
            cJSON *s = cJSON_GetObjectItemCaseSensitive((cJSON *)args,
                                                        "seconds");
            if (cJSON_IsNumber(s)) {
                snprintf(secbuf, sizeof(secbuf), "%d",
                         (int)s->valuedouble);
                seconds = secbuf;
            }
        }
    }

    if (strcmp(name, "bringup") == 0) {
        *out_desc = "Bring up a board via smolmux serial tools";
        *out_text = build_bringup(port);
        return *out_text ? 0 : -1;
    }
    if (strcmp(name, "debug_serial") == 0) {
        *out_desc = "Diagnose serial output and crashes";
        *out_text = build_debug_serial(seconds);
        return *out_text ? 0 : -1;
    }
    if (strcmp(name, "flash_safe") == 0) {
        *out_desc = "Suspend for flash, resume, verify boot";
        *out_text = build_flash_safe();
        return *out_text ? 0 : -1;
    }
    return -1;
}
