#ifndef SM_PROTOCOL_H
#define SM_PROTOCOL_H

#include "cJSON.h"
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SM_MSG_HELLO,
    SM_MSG_WELCOME,
    SM_MSG_SEND,
    SM_MSG_SEND_EXPECT,
    SM_MSG_OUTPUT,
    SM_MSG_INPUT_ECHO,
    SM_MSG_EXPECT_RESULT,
    SM_MSG_TAKEOVER,
    SM_MSG_RELEASE,
    SM_MSG_STATUS,
    SM_MSG_STATUS_RESPONSE,
    SM_MSG_PIN_CONTROL,
    SM_MSG_SET_BAUD,
    SM_MSG_SUSPEND,
    SM_MSG_RESUME,
    SM_MSG_SUSPENDED,
    SM_MSG_RESUMED,
    SM_MSG_HISTORY_REQUEST,
    SM_MSG_HISTORY_RESPONSE,
    SM_MSG_INCIDENTS_REQUEST,
    SM_MSG_INCIDENTS_RESPONSE,
    SM_MSG_CONFIGURE_ANOMALY,
    SM_MSG_ANOMALY,
    SM_MSG_INTERRUPT_AUTOBOOT,
    SM_MSG_AUTOBOOT_RESULT,
    SM_MSG_BOOT_STAGE,
    SM_MSG_BOOT_STALL,
    SM_MSG_CONFIGURE_AUTORESPONDER,
    SM_MSG_AUTORESPONDERS_REQUEST,
    SM_MSG_AUTORESPONDERS_RESPONSE,
    SM_MSG_AUTORESPONDER_FIRED,
    SM_MSG_LINK_DOWN,
    SM_MSG_LINK_UP,
    SM_MSG_ERROR,
    SM_MSG_UNKNOWN
} sm_msg_type_t;

typedef struct sm_msg {
    sm_msg_type_t type;
    cJSON *root;
} sm_msg_t;

/* Encode cJSON to newline-terminated JSON string. Caller frees. */
char *sm_msg_encode(cJSON *msg, size_t *out_len);

/* Decode a JSON line into sm_msg_t. Caller must sm_msg_free(). */
sm_msg_t sm_msg_decode(const char *line, size_t len);

void sm_msg_free(sm_msg_t *msg);

/* Convenience builders — return cJSON* (caller must cJSON_Delete or send) */
cJSON *sm_msg_hello(const char *name, const char *role);
cJSON *sm_msg_welcome(const char *version, const char *port, int baud, const char *role);
cJSON *sm_msg_send(const char *id, const uint8_t *data, size_t len);
cJSON *sm_msg_send_expect(const char *id, const uint8_t *data, size_t data_len,
                          const char *pattern, int timeout_ms);
cJSON *sm_msg_output(const uint8_t *data, size_t len, double timestamp);
cJSON *sm_msg_output_b64(const char *b64, double timestamp);
cJSON *sm_msg_input_echo(const uint8_t *data, size_t len, const char *sender, double timestamp);
cJSON *sm_msg_expect_result(const char *id, int matched, const uint8_t *data,
                            size_t len, const char *pattern);
cJSON *sm_msg_takeover(const char *id);
cJSON *sm_msg_release(const char *id);
cJSON *sm_msg_status(const char *id);
cJSON *sm_msg_status_response(const char *id, const char *port, int baud,
                              int connected, int suspended);
cJSON *sm_msg_pin_control(const char *id, const char *pin, const char *action, int duration_ms);
cJSON *sm_msg_set_baud(const char *id, int baud);
cJSON *sm_msg_suspend(const char *id);
cJSON *sm_msg_resume(const char *id);
cJSON *sm_msg_suspended(const char *port, const char *by_client);
cJSON *sm_msg_resumed(const char *port);
cJSON *sm_msg_history_request(const char *id, double since_ts, int last_bytes);
cJSON *sm_msg_history_response(const char *id, cJSON *chunks);
cJSON *sm_msg_incidents_request(const char *id, double since_ts);
cJSON *sm_msg_incidents_response(const char *id, cJSON *incidents);
cJSON *sm_msg_configure_anomaly(const char *id, cJSON *patterns);
cJSON *sm_msg_anomaly(const char *incident_id, const char *pattern_name,
                      const char *severity, double timestamp,
                      const char *match_text, const char *pre_context);
cJSON *sm_msg_interrupt_autoboot(const char *id, const uint8_t *key, size_t key_len,
                                 int interval_ms, int duration_ms,
                                 const char *stop_pattern);
cJSON *sm_msg_autoboot_result(const char *id, int matched, const char *reason,
                              int elapsed_ms, int sent);
cJSON *sm_msg_boot_stage(const char *name, int index, int total, double timestamp);
cJSON *sm_msg_boot_stall(const char *name, int index, int total, int stalled_ms);
cJSON *sm_msg_configure_autoresponder(const char *id, const char *name,
                                      const char *pattern,
                                      const uint8_t *response, size_t response_len,
                                      int once, int cooldown_ms, int remove);
cJSON *sm_msg_autoresponders_request(const char *id);
cJSON *sm_msg_autoresponders_response(const char *id, cJSON *rules);
cJSON *sm_msg_autoresponder_fired(const char *name, const char *matched_text, int sent);
cJSON *sm_msg_link_down(const char *port, const char *reason);
cJSON *sm_msg_link_up(const char *port);
cJSON *sm_msg_error(const char *id, const char *message);
cJSON *sm_msg_ack(const char *type, const char *id);

/* Type name string lookup */
const char *sm_msg_type_name(sm_msg_type_t type);

#endif /* SM_PROTOCOL_H */
