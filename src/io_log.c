#include "io_log.h"
#include "constants.h"
#include "util/base64.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

sm_io_log_t *sm_io_log_open(const char *path)
{
    sm_io_log_t *log = calloc(1, sizeof(*log));
    if (!log) return NULL;
    log->fp = fopen(path, "a");
    if (!log->fp) {
        free(log);
        return NULL;
    }
    log->path = strdup(path);
    return log;
}

void sm_io_log_close(sm_io_log_t *log)
{
    if (!log) return;
    sm_io_log_flush(log);
    if (log->fp) fclose(log->fp);
    free(log->path);
    free(log);
}

/* Durability (I3): fflush pushes each record to the OS, so the log
 * survives a broker crash, but there is no fsync — records may be lost on
 * power loss / kernel panic. This is deliberate: per-record fsync would
 * stall the event loop on every byte of device output. A post-mortem JSONL
 * log does not warrant that cost. */
static void write_json_line(sm_io_log_t *log, cJSON *obj, int force_flush)
{
    char *str = cJSON_PrintUnformatted(obj);
    if (str) {
        fprintf(log->fp, "%s\n", str);
        free(str);
        log->records_since_flush++;
        if (force_flush || log->records_since_flush >= SM_IO_LOG_FLUSH_RECORDS) {
            fflush(log->fp);
            log->records_since_flush = 0;
        }
    }
}

void sm_io_log_flush(sm_io_log_t *log)
{
    if (log && log->fp) {
        fflush(log->fp);
        log->records_since_flush = 0;
    }
}

void sm_io_log_output(sm_io_log_t *log, const uint8_t *data, size_t len, double ts)
{
    if (!log || !log->fp) return;
    char *b64 = sm_base64_encode(data, len);

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "type", "output");
    cJSON_AddNumberToObject(obj, "timestamp", ts);
    cJSON_AddStringToObject(obj, "data", b64 ? b64 : "");
    write_json_line(log, obj, 0);

    cJSON_Delete(obj);
    free(b64);
}

void sm_io_log_output_b64(sm_io_log_t *log, const char *b64, double ts)
{
    if (!log || !log->fp) return;

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "type", "output");
    cJSON_AddNumberToObject(obj, "timestamp", ts);
    cJSON_AddStringToObject(obj, "data", b64 ? b64 : "");
    write_json_line(log, obj, 0);
    cJSON_Delete(obj);
}

void sm_io_log_input(sm_io_log_t *log, const uint8_t *data, size_t len,
                     const char *sender, double ts)
{
    if (!log || !log->fp) return;
    char *b64 = sm_base64_encode(data, len);

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "type", "input");
    cJSON_AddNumberToObject(obj, "timestamp", ts);
    cJSON_AddStringToObject(obj, "sender", sender ? sender : "");
    cJSON_AddStringToObject(obj, "data", b64 ? b64 : "");
    write_json_line(log, obj, 0);

    cJSON_Delete(obj);
    free(b64);
}

void sm_io_log_incident(sm_io_log_t *log, const char *incident_id,
                        const char *pattern_name, const char *severity, double ts)
{
    if (!log || !log->fp) return;

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "type", "incident");
    cJSON_AddNumberToObject(obj, "timestamp", ts);
    cJSON_AddStringToObject(obj, "incident_id", incident_id);
    cJSON_AddStringToObject(obj, "pattern_name", pattern_name);
    cJSON_AddStringToObject(obj, "severity", severity);
    write_json_line(log, obj, 1);

    cJSON_Delete(obj);
}
