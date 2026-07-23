#include "logger.h"

#include <stdarg.h>
#include <string.h>

static sm_log_level_t g_log_level = SM_LOG_INFO;

static const char *level_names[] = {"DEBUG", "INFO", "WARN", "ERROR"};

void sm_logger_set_level(sm_log_level_t level)
{
    g_log_level = level;
}

void sm_log(sm_log_level_t level, const char *component, const char *fmt, ...)
{
    if (level < g_log_level) return;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    fprintf(stderr, "%02d:%02d:%02d.%03ld [%-5s] %s: ",
            tm.tm_hour, tm.tm_min, tm.tm_sec,
            ts.tv_nsec / 1000000,
            level_names[level], component);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
}
