#ifndef SM_BOOT_STAGE_H
#define SM_BOOT_STAGE_H

#include <stddef.h>
#include <stdint.h>
#include "regex_engine.h"

/*
 * Boot-stage progress tracking.
 *
 * A device profile can declare an ordered list of boot markers (name + regex).
 * As device output streams through the broker, the tracker records how far the
 * boot got: which stages were reached (with timestamps), the furthest stage,
 * and — computed on demand — whether the boot has stalled (no advance for
 * stall_timeout while the terminal stage is unreached).
 *
 * Matching is order-tolerant (a garbled or missed intermediate marker does not
 * block later ones); the ordered array position defines "progress". The last
 * declared stage is the terminal stage.
 */

typedef struct sm_boot_stage {
    char name[64];
    char pattern[256];
    sm_regex_t *compiled;
    int reached;
    int announced;         /* broker has already broadcast this stage */
    double reached_ts;     /* wall-clock ts when first reached */
} sm_boot_stage_t;

typedef struct sm_boot_tracker {
    sm_boot_stage_t *stages;
    size_t stage_count;
    size_t stage_cap;
    int furthest;              /* index of highest reached stage, -1 if none */
    double last_advance_ts;    /* ts of the most recent stage advance */
    double stall_timeout_s;    /* stall threshold in seconds */
    /* Sliding window for cross-chunk matching */
    uint8_t *window;
    size_t window_len;
    size_t window_cap;
    size_t search_offset;      /* start of unscanned region */
} sm_boot_tracker_t;

void sm_boot_init(sm_boot_tracker_t *t);
void sm_boot_destroy(sm_boot_tracker_t *t);

/* Append an ordered boot stage. Returns 0 on success, -1 on failure. */
int sm_boot_add_stage(sm_boot_tracker_t *t, const char *name, const char *pattern);

/* Override the stall timeout (milliseconds). <= 0 leaves the default. */
void sm_boot_set_stall_timeout(sm_boot_tracker_t *t, int timeout_ms);

/* Feed device output. Returns the number of stages newly reached this call. */
size_t sm_boot_feed(sm_boot_tracker_t *t, const uint8_t *data, size_t len, double ts);

/* Clear all reached/announced state (new boot). Keeps stage definitions and
 * the sliding window. */
void sm_boot_reset(sm_boot_tracker_t *t);

/* 1 if the boot appears stalled at wall-clock time `now`. */
int sm_boot_stalled(const sm_boot_tracker_t *t, double now);

/* 1 if the terminal (last) stage has been reached. */
int sm_boot_terminal_reached(const sm_boot_tracker_t *t);

#endif /* SM_BOOT_STAGE_H */
