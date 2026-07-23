#ifndef SM_IO_LOG_H
#define SM_IO_LOG_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct sm_io_log {
    FILE *fp;
    char *path;
    int records_since_flush;
} sm_io_log_t;

sm_io_log_t *sm_io_log_open(const char *path);
void sm_io_log_close(sm_io_log_t *log);
void sm_io_log_output(sm_io_log_t *log, const uint8_t *data, size_t len, double ts);
void sm_io_log_output_b64(sm_io_log_t *log, const char *b64, double ts);
void sm_io_log_flush(sm_io_log_t *log);
void sm_io_log_input(sm_io_log_t *log, const uint8_t *data, size_t len,
                     const char *sender, double ts);
void sm_io_log_incident(sm_io_log_t *log, const char *incident_id,
                        const char *pattern_name, const char *severity, double ts);

#endif /* SM_IO_LOG_H */
