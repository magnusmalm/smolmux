#ifndef SM_ANOMALY_H
#define SM_ANOMALY_H

#include <stddef.h>
#include <stdint.h>
#include "regex_engine.h"

#include "constants.h"

typedef struct sm_anomaly_incident {
    char id[16];
    char pattern_name[64];
    char severity[16];
    double timestamp;
    char match_text[256];
    char pre_context[SM_ANOMALY_CONTEXT_SIZE];
} sm_anomaly_incident_t;

typedef struct sm_anomaly_pattern {
    char name[64];
    char severity[16];
    char literal[48];  /* optional prefilter; skip regex when absent in window */
    sm_regex_t *compiled;
    double last_fire_time;
} sm_anomaly_pattern_t;

typedef struct sm_anomaly_detector {
    sm_anomaly_pattern_t *patterns;
    size_t pattern_count;
    size_t pattern_cap;
    unsigned id_seq;
    sm_anomaly_incident_t *incidents;
    size_t incident_count;
    size_t incident_cap;
    uint8_t context_buf[SM_ANOMALY_CONTEXT_SIZE];
    size_t context_len;
    double cooldown_s;
    /* Sliding window for matching */
    uint8_t *window;
    size_t window_len;
    size_t window_cap;
    size_t search_offset;  /* start of unscanned region */
} sm_anomaly_detector_t;

void sm_anomaly_init(sm_anomaly_detector_t *det);
void sm_anomaly_destroy(sm_anomaly_detector_t *det);

/* Add the built-in patterns */
void sm_anomaly_add_builtins(sm_anomaly_detector_t *det);

/* Add a custom pattern */
int sm_anomaly_add_pattern(sm_anomaly_detector_t *det,
                           const char *name, const char *pattern, const char *severity);

/* Feed data. Returns number of new incidents detected. */
size_t sm_anomaly_feed(sm_anomaly_detector_t *det,
                       const uint8_t *data, size_t len, double ts);

/* Get recent incidents */
const sm_anomaly_incident_t *sm_anomaly_get_incidents(const sm_anomaly_detector_t *det,
                                                      size_t *count);

#endif /* SM_ANOMALY_H */
