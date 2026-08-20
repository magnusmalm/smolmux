#include "io_log.h"
#include "constants.h"
#include "logger.h"
#include "util/base64.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#define LOG_TAG "io_log"

/* Create the log's parent directory. Intermediates keep normal permissions;
 * the leaf (our own directory) is private, since everything we put in it is
 * console traffic. Best effort — a failure here surfaces as an open failure
 * below, with a specific reason. */
static void ensure_parent_dir(const char *path)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);

    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir)
        return;              /* no directory component, or the root itself */
    *slash = '\0';

    for (char *p = dir + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(dir, 0755);
        *p = '/';
    }
    mkdir(dir, 0700);
}

/* Open the log for append without following a symlink and without ever
 * inheriting a file somebody else set up.
 *
 * This file is effectively a credential store: it holds every byte typed at
 * the console. Plain fopen(path, "a") in a shared directory gave a local
 * attacker two no-race primitives — pre-create the path as a file they own
 * and read the victim's whole session, or pre-create it as a symlink and have
 * the broker append console traffic to a file of their choosing. Being first
 * was enough; /tmp's sticky bit does not help.
 *
 * So: O_NOFOLLOW defeats the symlink, the uid check defeats the planted file,
 * the link-count check defeats a hardlink farmed onto a file we would not
 * otherwise write, and 0600 keeps the result unreadable by other users. Every
 * rejection fails closed and says why — no logging beats logging secrets
 * somewhere unintended. */
static FILE *open_append_private(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC,
                  S_IRUSR | S_IWUSR);
    if (fd < 0) {
        /* O_NOFOLLOW reports a symlinked final component as ELOOP. */
        SM_LOG_ERROR(LOG_TAG, "cannot open %s: %s%s", path, strerror(errno),
                     errno == ELOOP ? " (path is a symlink - refusing)" : "");
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        SM_LOG_ERROR(LOG_TAG, "cannot stat %s: %s", path, strerror(errno));
        close(fd);
        return NULL;
    }

    if (!S_ISREG(st.st_mode)) {
        SM_LOG_ERROR(LOG_TAG, "%s is not a regular file - refusing to log",
                     path);
        close(fd);
        return NULL;
    }
    if (st.st_uid != geteuid()) {
        SM_LOG_ERROR(LOG_TAG,
                     "%s is owned by uid %u, not %u - refusing to log "
                     "(another user could be reading the console)",
                     path, (unsigned)st.st_uid, (unsigned)geteuid());
        close(fd);
        return NULL;
    }
    if (st.st_nlink != 1) {
        SM_LOG_ERROR(LOG_TAG, "%s has %u hard links - refusing to log", path,
                     (unsigned)st.st_nlink);
        close(fd);
        return NULL;
    }

    /* A log left by an older build (or a loose umask) may already be group- or
     * world-readable. Tighten it before appending anything further to it. */
    if (st.st_mode & (S_IRWXG | S_IRWXO)) {
        if (fchmod(fd, S_IRUSR | S_IWUSR) == 0)
            SM_LOG_INFO(LOG_TAG, "tightened permissions on %s to 0600", path);
        else
            SM_LOG_WARN(LOG_TAG, "cannot tighten permissions on %s: %s", path,
                        strerror(errno));
    }

    FILE *fp = fdopen(fd, "a");
    if (!fp) {
        SM_LOG_ERROR(LOG_TAG, "fdopen %s: %s", path, strerror(errno));
        close(fd);
    }
    return fp;
}

sm_io_log_t *sm_io_log_open(const char *path)
{
    sm_io_log_t *log = calloc(1, sizeof(*log));
    if (!log) return NULL;
    ensure_parent_dir(path);
    log->fp = open_append_private(path);
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
