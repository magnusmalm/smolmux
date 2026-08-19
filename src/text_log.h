#ifndef SM_TEXT_LOG_H
#define SM_TEXT_LOG_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct sm_text_log {
    FILE *fp;
    char *dir;
    char *port_name;
    int current_day;       /* yday for rotation check */
    int current_year;      /* tm_year for rotation check */
    char line_buf[4096];
    size_t line_len;
} sm_text_log_t;

sm_text_log_t *sm_text_log_open(const char *dir, const char *port_name);
void sm_text_log_close(sm_text_log_t *log);
void sm_text_log_write(sm_text_log_t *log, const uint8_t *data, size_t len, double ts);

#endif /* SM_TEXT_LOG_H */
