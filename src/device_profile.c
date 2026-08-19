#include "device_profile.h"
#include "constants.h"
#include "logger.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sm_profile_init_default(sm_device_profile_t *p)
{
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "generic");
    snprintf(p->device_type, sizeof(p->device_type), "unknown");
    snprintf(p->description, sizeof(p->description), "Generic serial device");
    snprintf(p->prompt_pattern, sizeof(p->prompt_pattern), "%s",
             SM_PROFILE_DEFAULT_PROMPT);
    snprintf(p->response_mode, sizeof(p->response_mode), "prompt");
    p->default_timeout_ms = SM_PROFILE_DEFAULT_TIMEOUT;
}

void sm_profile_destroy(sm_device_profile_t *p)
{
    free(p->commands);
    free(p->anomaly_patterns);
    free(p->boot_stages);
    p->commands = NULL;
    p->anomaly_patterns = NULL;
    p->boot_stages = NULL;
    p->command_count = 0;
    p->command_cap = 0;
    p->anomaly_count = 0;
    p->anomaly_cap = 0;
    p->boot_stage_count = 0;
    p->boot_stage_cap = 0;
}

static sm_profile_command_t *alloc_command(sm_device_profile_t *p)
{
    if (p->command_count >= SM_PROFILE_MAX_COMMANDS) return NULL;
    if (p->command_count >= p->command_cap) {
        size_t new_cap = p->command_cap ? p->command_cap * 2 : 8;
        if (new_cap > SM_PROFILE_MAX_COMMANDS) new_cap = SM_PROFILE_MAX_COMMANDS;
        void *tmp = realloc(p->commands, new_cap * sizeof(sm_profile_command_t));
        if (!tmp) return NULL;
        p->commands = tmp;
        p->command_cap = new_cap;
    }
    return &p->commands[p->command_count];
}

static sm_profile_anomaly_t *alloc_anomaly(sm_device_profile_t *p)
{
    if (p->anomaly_count >= SM_PROFILE_MAX_ANOMALY_PATTERNS) return NULL;
    if (p->anomaly_count >= p->anomaly_cap) {
        size_t new_cap = p->anomaly_cap ? p->anomaly_cap * 2 : 8;
        if (new_cap > SM_PROFILE_MAX_ANOMALY_PATTERNS) new_cap = SM_PROFILE_MAX_ANOMALY_PATTERNS;
        void *tmp = realloc(p->anomaly_patterns, new_cap * sizeof(sm_profile_anomaly_t));
        if (!tmp) return NULL;
        p->anomaly_patterns = tmp;
        p->anomaly_cap = new_cap;
    }
    return &p->anomaly_patterns[p->anomaly_count];
}

static sm_profile_boot_stage_t *alloc_boot_stage(sm_device_profile_t *p)
{
    if (p->boot_stage_count >= SM_PROFILE_MAX_BOOT_STAGES) return NULL;
    if (p->boot_stage_count >= p->boot_stage_cap) {
        size_t new_cap = p->boot_stage_cap ? p->boot_stage_cap * 2 : 8;
        if (new_cap > SM_PROFILE_MAX_BOOT_STAGES) new_cap = SM_PROFILE_MAX_BOOT_STAGES;
        void *tmp = realloc(p->boot_stages, new_cap * sizeof(sm_profile_boot_stage_t));
        if (!tmp) return NULL;
        p->boot_stages = tmp;
        p->boot_stage_cap = new_cap;
    }
    return &p->boot_stages[p->boot_stage_count];
}

static void parse_profile_json(sm_device_profile_t *p, cJSON *root)
{
    cJSON *item;

    item = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (cJSON_IsString(item))
        snprintf(p->name, sizeof(p->name), "%s", item->valuestring);

    item = cJSON_GetObjectItemCaseSensitive(root, "device_type");
    if (cJSON_IsString(item))
        snprintf(p->device_type, sizeof(p->device_type), "%s", item->valuestring);

    item = cJSON_GetObjectItemCaseSensitive(root, "description");
    if (cJSON_IsString(item))
        snprintf(p->description, sizeof(p->description), "%s", item->valuestring);

    item = cJSON_GetObjectItemCaseSensitive(root, "prompt_pattern");
    if (cJSON_IsString(item))
        snprintf(p->prompt_pattern, sizeof(p->prompt_pattern), "%s", item->valuestring);

    item = cJSON_GetObjectItemCaseSensitive(root, "command_prefix");
    if (cJSON_IsString(item)) {
        snprintf(p->command_prefix, sizeof(p->command_prefix), "%s", item->valuestring);
        /* Validate: only printable chars, \r, \n allowed */
        for (const char *cp = p->command_prefix; *cp; cp++) {
            unsigned char ch = (unsigned char)*cp;
            if (ch != '\r' && ch != '\n' && (ch < 0x20 || ch == 0x7F)) {
                SM_LOG_WARN("profile", "command_prefix contains invalid char 0x%02x, clearing", ch);
                p->command_prefix[0] = '\0';
                break;
            }
        }
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "response_mode");
    if (cJSON_IsString(item))
        snprintf(p->response_mode, sizeof(p->response_mode), "%s", item->valuestring);

    item = cJSON_GetObjectItemCaseSensitive(root, "default_timeout_ms");
    if (cJSON_IsNumber(item))
        p->default_timeout_ms = item->valueint;

    item = cJSON_GetObjectItemCaseSensitive(root, "boot_banner");
    if (cJSON_IsString(item))
        snprintf(p->boot_banner, sizeof(p->boot_banner), "%s", item->valuestring);

    /* Parse commands array — accepts both string and {cmd, description} entries */
    cJSON *commands = cJSON_GetObjectItemCaseSensitive(root, "commands");
    if (cJSON_IsArray(commands)) {
        cJSON *cmd;
        cJSON_ArrayForEach(cmd, commands) {
            sm_profile_command_t *c = alloc_command(p);
            if (!c) break;
            if (cJSON_IsString(cmd)) {
                snprintf(c->cmd, sizeof(c->cmd), "%s", cmd->valuestring);
                c->description[0] = '\0';
                p->command_count++;
            } else if (cJSON_IsObject(cmd)) {
                cJSON *cmd_str = cJSON_GetObjectItemCaseSensitive(cmd, "cmd");
                cJSON *desc = cJSON_GetObjectItemCaseSensitive(cmd, "description");
                if (cJSON_IsString(cmd_str)) {
                    snprintf(c->cmd, sizeof(c->cmd), "%s", cmd_str->valuestring);
                    if (cJSON_IsString(desc))
                        snprintf(c->description, sizeof(c->description), "%s",
                                 desc->valuestring);
                    else
                        c->description[0] = '\0';
                    p->command_count++;
                }
            }
        }
    }

    /* Parse anomaly patterns — accept both key names */
    cJSON *anomalies = cJSON_GetObjectItemCaseSensitive(root, "custom_anomaly_patterns");
    if (!anomalies)
        anomalies = cJSON_GetObjectItemCaseSensitive(root, "anomaly_patterns");
    if (cJSON_IsArray(anomalies)) {
        cJSON *a;
        cJSON_ArrayForEach(a, anomalies) {
            if (!cJSON_IsObject(a)) continue;

            cJSON *name = cJSON_GetObjectItemCaseSensitive(a, "name");
            cJSON *pattern = cJSON_GetObjectItemCaseSensitive(a, "pattern");
            cJSON *severity = cJSON_GetObjectItemCaseSensitive(a, "severity");

            if (!cJSON_IsString(name) || !cJSON_IsString(pattern)) continue;

            sm_profile_anomaly_t *ap = alloc_anomaly(p);
            if (!ap) break;

            snprintf(ap->name, sizeof(ap->name), "%s", name->valuestring);
            snprintf(ap->pattern, sizeof(ap->pattern), "%s", pattern->valuestring);
            if (cJSON_IsString(severity))
                snprintf(ap->severity, sizeof(ap->severity), "%s", severity->valuestring);
            else
                snprintf(ap->severity, sizeof(ap->severity), "warning");
            p->anomaly_count++;
        }
    }

    /* Parse boot stages — ordered {name, pattern} entries */
    item = cJSON_GetObjectItemCaseSensitive(root, "boot_stall_timeout_ms");
    if (cJSON_IsNumber(item))
        p->boot_stall_timeout_ms = item->valueint;

    cJSON *stages = cJSON_GetObjectItemCaseSensitive(root, "boot_stages");
    if (cJSON_IsArray(stages)) {
        cJSON *st;
        cJSON_ArrayForEach(st, stages) {
            if (!cJSON_IsObject(st)) continue;

            cJSON *name = cJSON_GetObjectItemCaseSensitive(st, "name");
            cJSON *pattern = cJSON_GetObjectItemCaseSensitive(st, "pattern");
            if (!cJSON_IsString(name) || !cJSON_IsString(pattern)) continue;

            sm_profile_boot_stage_t *bs = alloc_boot_stage(p);
            if (!bs) break;

            snprintf(bs->name, sizeof(bs->name), "%s", name->valuestring);
            snprintf(bs->pattern, sizeof(bs->pattern), "%s", pattern->valuestring);
            p->boot_stage_count++;
        }
    }
}

int sm_profile_load(sm_device_profile_t *p, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) { fclose(f); return -1; }
    if (size > SM_MAX_PROFILE_FILE_BYTES) { fclose(f); return -1; }  /* L8 */

    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return -1; }

    size_t nread = fread(buf, 1, (size_t)size, f);
    buf[nread] = '\0';
    fclose(f);

    int rc = sm_profile_from_json(p, buf);
    free(buf);
    return rc;
}

int sm_profile_from_json(sm_device_profile_t *p, const char *json_str)
{
    sm_profile_init_default(p);

    cJSON *root = cJSON_Parse(json_str);
    if (!root) return -1;

    parse_profile_json(p, root);
    cJSON_Delete(root);
    return 0;
}

void sm_profile_apply_anomaly(const sm_device_profile_t *p, sm_anomaly_detector_t *det)
{
    for (size_t i = 0; i < p->anomaly_count; i++) {
        const sm_profile_anomaly_t *a = &p->anomaly_patterns[i];
        sm_anomaly_add_pattern(det, a->name, a->pattern, a->severity);
    }
}

void sm_profile_apply_boot(const sm_device_profile_t *p, sm_boot_tracker_t *t)
{
    sm_boot_set_stall_timeout(t, p->boot_stall_timeout_ms);
    for (size_t i = 0; i < p->boot_stage_count; i++) {
        const sm_profile_boot_stage_t *bs = &p->boot_stages[i];
        sm_boot_add_stage(t, bs->name, bs->pattern);
    }
}
