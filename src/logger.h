#ifndef SM_LOGGER_H
#define SM_LOGGER_H

#include <stdio.h>
#include <time.h>

typedef enum {
    SM_LOG_DEBUG = 0,
    SM_LOG_INFO  = 1,
    SM_LOG_WARN  = 2,
    SM_LOG_ERROR = 3
} sm_log_level_t;

void sm_log(sm_log_level_t level, const char *component, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

void sm_logger_set_level(sm_log_level_t level);

#define SM_LOG_DEBUG(comp, ...) sm_log(SM_LOG_DEBUG, comp, __VA_ARGS__)
#define SM_LOG_INFO(comp, ...)  sm_log(SM_LOG_INFO, comp, __VA_ARGS__)
#define SM_LOG_WARN(comp, ...)  sm_log(SM_LOG_WARN, comp, __VA_ARGS__)
#define SM_LOG_ERROR(comp, ...) sm_log(SM_LOG_ERROR, comp, __VA_ARGS__)

#endif /* SM_LOGGER_H */
