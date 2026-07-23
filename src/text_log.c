#include "text_log.h"
#include "constants.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

static void open_file(sm_text_log_t *log, time_t now)
{
    struct tm tm;
    localtime_r(&now, &tm);
    log->current_day = tm.tm_yday;
    log->current_year = tm.tm_year;

    char path[512];
    snprintf(path, sizeof(path), "%s/%s-%04d%02d%02d.log",
             log->dir, log->port_name,
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

    if (log->fp) fclose(log->fp);
    log->fp = fopen(path, "a");
}

sm_text_log_t *sm_text_log_open(const char *dir, const char *port_name)
{
    sm_text_log_t *log = calloc(1, sizeof(*log));
    if (!log) return NULL;

    log->dir = strdup(dir);
    /* Strip /dev/ prefix for filename */
    if (strncmp(port_name, "/dev/", 5) == 0)
        log->port_name = strdup(port_name + 5);
    else
        log->port_name = strdup(port_name);

    if (!log->dir || !log->port_name) {
        free(log->dir);
        free(log->port_name);
        free(log);
        return NULL;
    }

    /* Replace / with - in port name */
    for (char *p = log->port_name; *p; p++)
        if (*p == '/') *p = '-';

    mkdir(dir, 0755);
    open_file(log, time(NULL));
    return log;
}

void sm_text_log_close(sm_text_log_t *log)
{
    if (!log) return;
    /* Flush any partial line */
    if (log->fp && log->line_len > 0) {
        fwrite(log->line_buf, 1, log->line_len, log->fp);
        fputc('\n', log->fp);
    }
    if (log->fp) fclose(log->fp);
    free(log->dir);
    free(log->port_name);
    free(log);
}

void sm_text_log_write(sm_text_log_t *log, const uint8_t *data, size_t len, double ts)
{
    if (!log || !log->fp) return;

    /* Check for day rotation */
    time_t sec = (time_t)ts;
    struct tm tm;
    localtime_r(&sec, &tm);
    if (tm.tm_yday != log->current_day || tm.tm_year != log->current_year)
        open_file(log, sec);
    if (!log->fp) return;

    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') {
            /* Write timestamp + accumulated line */
            fprintf(log->fp, "[%02d:%02d:%02d] ",
                    tm.tm_hour, tm.tm_min, tm.tm_sec);
            if (log->line_len > 0)
                fwrite(log->line_buf, 1, log->line_len, log->fp);
            fputc('\n', log->fp);
            log->line_len = 0;
            /* Flush every completed line. Text logs are low-volume; batching
             * (old SM_TEXT_LOG_FLUSH_LINES=32) left short dogfood sessions as
             * 0-byte files on disk while JSONL already had data, until exit.
             * fflush so still-running brokers show live .log content. */
            fflush(log->fp);
        } else if (data[i] != '\r') {
            if (log->line_len < sizeof(log->line_buf) - 1)
                log->line_buf[log->line_len++] = (char)data[i];
        }
    }
}
