#include "protocol.h"
#include "constants.h"
#include "util/base64.h"
#include "util/json_helpers.h"

#include <stdlib.h>
#include <string.h>

static const struct { const char *name; sm_msg_type_t type; } msg_type_map[] = {
    {"hello",              SM_MSG_HELLO},
    {"welcome",            SM_MSG_WELCOME},
    {"send",               SM_MSG_SEND},
    {"send_expect",        SM_MSG_SEND_EXPECT},
    {"output",             SM_MSG_OUTPUT},
    {"input_echo",         SM_MSG_INPUT_ECHO},
    {"expect_result",      SM_MSG_EXPECT_RESULT},
    {"takeover",           SM_MSG_TAKEOVER},
    {"release",            SM_MSG_RELEASE},
    {"status",             SM_MSG_STATUS},
    {"status_response",    SM_MSG_STATUS_RESPONSE},
    {"pin_control",        SM_MSG_PIN_CONTROL},
    {"set_baud",           SM_MSG_SET_BAUD},
    {"suspend",            SM_MSG_SUSPEND},
    {"resume",             SM_MSG_RESUME},
    {"suspended",          SM_MSG_SUSPENDED},
    {"resumed",            SM_MSG_RESUMED},
    {"history_request",    SM_MSG_HISTORY_REQUEST},
    {"history_response",   SM_MSG_HISTORY_RESPONSE},
    {"incidents_request",  SM_MSG_INCIDENTS_REQUEST},
    {"incidents_response", SM_MSG_INCIDENTS_RESPONSE},
    {"configure_anomaly",  SM_MSG_CONFIGURE_ANOMALY},
    {"anomaly",            SM_MSG_ANOMALY},
    {"interrupt_autoboot", SM_MSG_INTERRUPT_AUTOBOOT},
    {"autoboot_result",    SM_MSG_AUTOBOOT_RESULT},
    {"boot_stage",         SM_MSG_BOOT_STAGE},
    {"boot_stall",         SM_MSG_BOOT_STALL},
    {"configure_autoresponder", SM_MSG_CONFIGURE_AUTORESPONDER},
    {"autoresponders_request",  SM_MSG_AUTORESPONDERS_REQUEST},
    {"autoresponders_response", SM_MSG_AUTORESPONDERS_RESPONSE},
    {"autoresponder_fired",     SM_MSG_AUTORESPONDER_FIRED},
    {"link_down",          SM_MSG_LINK_DOWN},
    {"link_up",            SM_MSG_LINK_UP},
    {"error",              SM_MSG_ERROR},
    {NULL, SM_MSG_UNKNOWN}
};

const char *sm_msg_type_name(sm_msg_type_t type)
{
    for (int i = 0; msg_type_map[i].name; i++)
        if (msg_type_map[i].type == type)
            return msg_type_map[i].name;
    return "unknown";
}

static sm_msg_type_t lookup_type(const char *name)
{
    if (!name) return SM_MSG_UNKNOWN;
    for (int i = 0; msg_type_map[i].name; i++)
        if (strcmp(msg_type_map[i].name, name) == 0)
            return msg_type_map[i].type;
    return SM_MSG_UNKNOWN;
}

char *sm_msg_encode(cJSON *msg, size_t *out_len)
{
    char *json = cJSON_PrintUnformatted(msg);
    if (!json) return NULL;
    size_t jlen = strlen(json);
    /* Don't realloc() a cJSON-owned buffer — its allocator may differ from
     * the default (L4). Allocate our own and copy. */
    char *line = malloc(jlen + 2);
    if (!line) { cJSON_free(json); return NULL; }
    memcpy(line, json, jlen);
    cJSON_free(json);
    line[jlen] = '\n';
    line[jlen + 1] = '\0';
    if (out_len) *out_len = jlen + 1;
    return line;
}

sm_msg_t sm_msg_decode(const char *line, size_t len)
{
    sm_msg_t msg = {SM_MSG_UNKNOWN, NULL};
    if (!line || len == 0) return msg;

    /* Reject deeply nested JSON to prevent stack overflow in cJSON parser */
    int depth = 0;
    for (size_t i = 0; i < len; i++) {
        if (line[i] == '{' || line[i] == '[') {
            if (++depth > 32) return msg;
        } else if (line[i] == '}' || line[i] == ']') {
            depth--;
        }
    }

    /* Strip trailing newline */
    char *buf = malloc(len + 1);
    if (!buf) return msg;
    memcpy(buf, line, len);
    buf[len] = '\0';
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';

    msg.root = cJSON_Parse(buf);
    free(buf);
    if (!msg.root) return msg;

    const char *type_str = sm_json_get_string(msg.root, "type");
    msg.type = lookup_type(type_str);
    return msg;
}

void sm_msg_free(sm_msg_t *msg)
{
    if (msg && msg->root) {
        cJSON_Delete(msg->root);
        msg->root = NULL;
    }
}

/* Convenience builders */

static cJSON *make_msg(const char *type)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", type);
    return msg;
}

static void add_b64_data(cJSON *msg, const char *key, const uint8_t *data, size_t len)
{
    char *b64 = sm_base64_encode(data, len);
    cJSON_AddStringToObject(msg, key, b64 ? b64 : "");
    free(b64);
}

cJSON *sm_msg_hello(const char *name, const char *role)
{
    cJSON *msg = make_msg("hello");
    cJSON_AddStringToObject(msg, "name", name ? name : "unnamed");
    cJSON_AddStringToObject(msg, "role", role ? role : "observer");
    cJSON_AddNumberToObject(msg, "protocol_version", SM_PROTOCOL_VERSION);

    /* Brokers with --auth-token require this from network clients. Picked
     * up from the environment so every client tool supports it uniformly. */
    const char *token = getenv("SMOLMUX_AUTH_TOKEN");
    if (token && token[0])
        cJSON_AddStringToObject(msg, "token", token);

    return msg;
}

cJSON *sm_msg_welcome(const char *version, const char *port, int baud, const char *role)
{
    cJSON *msg = make_msg("welcome");
    cJSON_AddStringToObject(msg, "broker_version", version);
    cJSON_AddNumberToObject(msg, "protocol_version", SM_PROTOCOL_VERSION);
    cJSON_AddStringToObject(msg, "port", port);
    cJSON_AddNumberToObject(msg, "baud", baud);
    cJSON_AddStringToObject(msg, "your_role", role);
    return msg;
}

cJSON *sm_msg_send(const char *id, const uint8_t *data, size_t len)
{
    cJSON *msg = make_msg("send");
    cJSON_AddStringToObject(msg, "id", id);
    add_b64_data(msg, "data", data, len);
    return msg;
}

cJSON *sm_msg_send_expect(const char *id, const uint8_t *data, size_t data_len,
                          const char *pattern, int timeout_ms)
{
    cJSON *msg = make_msg("send_expect");
    cJSON_AddStringToObject(msg, "id", id);
    add_b64_data(msg, "data", data, data_len);
    cJSON_AddStringToObject(msg, "pattern", pattern);
    cJSON_AddNumberToObject(msg, "timeout_ms", timeout_ms);
    return msg;
}

cJSON *sm_msg_output(const uint8_t *data, size_t len, double timestamp)
{
    cJSON *msg = make_msg("output");
    add_b64_data(msg, "data", data, len);
    cJSON_AddNumberToObject(msg, "timestamp", timestamp);
    return msg;
}

cJSON *sm_msg_output_b64(const char *b64, double timestamp)
{
    cJSON *msg = make_msg("output");
    cJSON_AddStringToObject(msg, "data", b64 ? b64 : "");
    cJSON_AddNumberToObject(msg, "timestamp", timestamp);
    return msg;
}

cJSON *sm_msg_input_echo(const uint8_t *data, size_t len, const char *sender, double timestamp)
{
    cJSON *msg = make_msg("input_echo");
    add_b64_data(msg, "data", data, len);
    cJSON_AddStringToObject(msg, "sender", sender);
    cJSON_AddNumberToObject(msg, "timestamp", timestamp);
    return msg;
}

cJSON *sm_msg_expect_result(const char *id, int matched, const uint8_t *data,
                            size_t len, const char *pattern)
{
    int truncated = 0;
    if (len > SM_MAX_EXPECT_RESULT_BYTES) {
        len = SM_MAX_EXPECT_RESULT_BYTES;
        truncated = 1;
    }
    cJSON *msg = make_msg("expect_result");
    cJSON_AddStringToObject(msg, "id", id);
    cJSON_AddBoolToObject(msg, "matched", matched);
    add_b64_data(msg, "data", data, len);
    cJSON_AddStringToObject(msg, "pattern", pattern);
    if (truncated)
        cJSON_AddBoolToObject(msg, "truncated", 1);
    return msg;
}

cJSON *sm_msg_takeover(const char *id)
{
    cJSON *msg = make_msg("takeover");
    cJSON_AddStringToObject(msg, "id", id);
    return msg;
}

cJSON *sm_msg_release(const char *id)
{
    cJSON *msg = make_msg("release");
    cJSON_AddStringToObject(msg, "id", id);
    return msg;
}

cJSON *sm_msg_status(const char *id)
{
    cJSON *msg = make_msg("status");
    cJSON_AddStringToObject(msg, "id", id);
    return msg;
}

cJSON *sm_msg_status_response(const char *id, const char *port, int baud,
                              int connected, int suspended)
{
    cJSON *msg = make_msg("status_response");
    cJSON_AddStringToObject(msg, "id", id);
    cJSON_AddStringToObject(msg, "port", port);
    cJSON_AddNumberToObject(msg, "baud", baud);
    cJSON_AddBoolToObject(msg, "connected", connected);
    cJSON_AddBoolToObject(msg, "suspended", suspended);
    return msg;
}

cJSON *sm_msg_pin_control(const char *id, const char *pin, const char *action, int duration_ms)
{
    cJSON *msg = make_msg("pin_control");
    cJSON_AddStringToObject(msg, "id", id);
    cJSON_AddStringToObject(msg, "pin", pin);
    cJSON_AddStringToObject(msg, "action", action);
    cJSON_AddNumberToObject(msg, "duration_ms", duration_ms);
    return msg;
}

cJSON *sm_msg_set_baud(const char *id, int baud)
{
    cJSON *msg = make_msg("set_baud");
    cJSON_AddStringToObject(msg, "id", id);
    cJSON_AddNumberToObject(msg, "baud", baud);
    return msg;
}

cJSON *sm_msg_suspend(const char *id)
{
    cJSON *msg = make_msg("suspend");
    cJSON_AddStringToObject(msg, "id", id);
    return msg;
}

cJSON *sm_msg_resume(const char *id)
{
    cJSON *msg = make_msg("resume");
    cJSON_AddStringToObject(msg, "id", id);
    return msg;
}

cJSON *sm_msg_suspended(const char *port, const char *by_client)
{
    cJSON *msg = make_msg("suspended");
    cJSON_AddStringToObject(msg, "port", port);
    cJSON_AddStringToObject(msg, "by_client", by_client);
    return msg;
}

cJSON *sm_msg_resumed(const char *port)
{
    cJSON *msg = make_msg("resumed");
    cJSON_AddStringToObject(msg, "port", port);
    return msg;
}

cJSON *sm_msg_history_request(const char *id, double since_ts, int last_bytes)
{
    cJSON *msg = make_msg("history_request");
    cJSON_AddStringToObject(msg, "id", id);
    cJSON_AddNumberToObject(msg, "since_ts", since_ts);
    cJSON_AddNumberToObject(msg, "last_bytes", last_bytes);
    return msg;
}

cJSON *sm_msg_history_response(const char *id, cJSON *chunks)
{
    cJSON *msg = make_msg("history_response");
    cJSON_AddStringToObject(msg, "id", id);
    cJSON_AddItemToObject(msg, "chunks", chunks ? chunks : cJSON_CreateArray());
    return msg;
}

cJSON *sm_msg_incidents_request(const char *id, double since_ts)
{
    cJSON *msg = make_msg("incidents_request");
    cJSON_AddStringToObject(msg, "id", id);
    cJSON_AddNumberToObject(msg, "since_ts", since_ts);
    return msg;
}

cJSON *sm_msg_incidents_response(const char *id, cJSON *incidents)
{
    cJSON *msg = make_msg("incidents_response");
    cJSON_AddStringToObject(msg, "id", id);
    cJSON_AddItemToObject(msg, "incidents", incidents ? incidents : cJSON_CreateArray());
    return msg;
}

cJSON *sm_msg_configure_anomaly(const char *id, cJSON *patterns)
{
    cJSON *msg = make_msg("configure_anomaly");
    cJSON_AddStringToObject(msg, "id", id);
    cJSON_AddItemToObject(msg, "patterns", patterns ? patterns : cJSON_CreateArray());
    return msg;
}

cJSON *sm_msg_anomaly(const char *incident_id, const char *pattern_name,
                      const char *severity, double timestamp,
                      const char *match_text, const char *pre_context)
{
    cJSON *msg = make_msg("anomaly");
    cJSON_AddStringToObject(msg, "incident_id", incident_id);
    cJSON_AddStringToObject(msg, "pattern_name", pattern_name);
    cJSON_AddStringToObject(msg, "severity", severity);
    cJSON_AddNumberToObject(msg, "timestamp", timestamp);
    cJSON_AddStringToObject(msg, "match_text", match_text);
    cJSON_AddStringToObject(msg, "pre_context", pre_context);
    return msg;
}

cJSON *sm_msg_interrupt_autoboot(const char *id, const uint8_t *key, size_t key_len,
                                 int interval_ms, int duration_ms,
                                 const char *stop_pattern)
{
    cJSON *msg = make_msg("interrupt_autoboot");
    if (id) cJSON_AddStringToObject(msg, "id", id);
    add_b64_data(msg, "key_b64", key, key_len);
    cJSON_AddNumberToObject(msg, "interval_ms", interval_ms);
    cJSON_AddNumberToObject(msg, "duration_ms", duration_ms);
    if (stop_pattern) cJSON_AddStringToObject(msg, "stop_pattern", stop_pattern);
    return msg;
}

cJSON *sm_msg_autoboot_result(const char *id, int matched, const char *reason,
                              int elapsed_ms, int sent)
{
    cJSON *msg = make_msg("autoboot_result");
    if (id) cJSON_AddStringToObject(msg, "id", id);
    cJSON_AddBoolToObject(msg, "matched", matched);
    cJSON_AddStringToObject(msg, "reason", reason);
    cJSON_AddNumberToObject(msg, "elapsed_ms", elapsed_ms);
    cJSON_AddNumberToObject(msg, "sent", sent);
    return msg;
}

cJSON *sm_msg_boot_stage(const char *name, int index, int total, double timestamp)
{
    cJSON *msg = make_msg("boot_stage");
    cJSON_AddStringToObject(msg, "name", name);
    cJSON_AddNumberToObject(msg, "index", index);
    cJSON_AddNumberToObject(msg, "total", total);
    cJSON_AddNumberToObject(msg, "timestamp", timestamp);
    return msg;
}

cJSON *sm_msg_boot_stall(const char *name, int index, int total, int stalled_ms)
{
    cJSON *msg = make_msg("boot_stall");
    cJSON_AddStringToObject(msg, "name", name);
    cJSON_AddNumberToObject(msg, "index", index);
    cJSON_AddNumberToObject(msg, "total", total);
    cJSON_AddNumberToObject(msg, "stalled_ms", stalled_ms);
    return msg;
}

cJSON *sm_msg_configure_autoresponder(const char *id, const char *name,
                                      const char *pattern,
                                      const uint8_t *response, size_t response_len,
                                      int once, int cooldown_ms, int remove)
{
    cJSON *msg = make_msg("configure_autoresponder");
    if (id) cJSON_AddStringToObject(msg, "id", id);
    if (name) cJSON_AddStringToObject(msg, "name", name);
    if (remove) {
        cJSON_AddBoolToObject(msg, "remove", 1);
        return msg;
    }
    if (pattern) cJSON_AddStringToObject(msg, "pattern", pattern);
    add_b64_data(msg, "response_b64", response, response_len);
    cJSON_AddBoolToObject(msg, "once", once);
    cJSON_AddNumberToObject(msg, "cooldown_ms", cooldown_ms);
    return msg;
}

cJSON *sm_msg_autoresponders_request(const char *id)
{
    cJSON *msg = make_msg("autoresponders_request");
    if (id) cJSON_AddStringToObject(msg, "id", id);
    return msg;
}

cJSON *sm_msg_autoresponders_response(const char *id, cJSON *rules)
{
    cJSON *msg = make_msg("autoresponders_response");
    if (id) cJSON_AddStringToObject(msg, "id", id);
    cJSON_AddItemToObject(msg, "rules", rules ? rules : cJSON_CreateArray());
    return msg;
}

cJSON *sm_msg_autoresponder_fired(const char *name, const char *matched_text, int sent)
{
    cJSON *msg = make_msg("autoresponder_fired");
    cJSON_AddStringToObject(msg, "name", name);
    cJSON_AddStringToObject(msg, "matched_text", matched_text);
    cJSON_AddNumberToObject(msg, "sent", sent);
    return msg;
}

cJSON *sm_msg_link_down(const char *port, const char *reason)
{
    cJSON *msg = make_msg("link_down");
    cJSON_AddStringToObject(msg, "port", port);
    cJSON_AddStringToObject(msg, "reason", reason);
    return msg;
}

cJSON *sm_msg_link_up(const char *port)
{
    cJSON *msg = make_msg("link_up");
    cJSON_AddStringToObject(msg, "port", port);
    return msg;
}

cJSON *sm_msg_error(const char *id, const char *message)
{
    cJSON *msg = make_msg("error");
    cJSON_AddStringToObject(msg, "id", id ? id : "");
    cJSON_AddStringToObject(msg, "message", message);
    return msg;
}

cJSON *sm_msg_ack(const char *type, const char *id)
{
    cJSON *msg = make_msg(type);
    cJSON_AddStringToObject(msg, "id", id ? id : "");
    cJSON_AddStringToObject(msg, "status", "ok");
    return msg;
}
