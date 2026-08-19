#include "anomaly.h"
#include "constants.h"
#include "logger.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#define LOG_TAG "anomaly"

/* Return 1 if region contains any semicolon-separated literal needle. */
static int region_has_literal(const char *region, const char *literal)
{
    if (!literal[0])
        return 1;

    const char *p = literal;
    while (*p) {
        const char *sep = strchr(p, ';');
        size_t len = sep ? (size_t)(sep - p) : strlen(p);
        if (len > 0) {
            char tmp[32];
            if (len >= sizeof(tmp))
                len = sizeof(tmp) - 1;
            memcpy(tmp, p, len);
            tmp[len] = '\0';
            if (strstr(region, tmp) != NULL)
                return 1;
        }
        if (!sep)
            break;
        p = sep + 1;
    }
    return 0;
}

/* Copy the leading literal run of a regex pattern (stops at the first regex
 * metacharacter) into out, for use as the cheap prefilter needle. */
static void extract_literal_prefix(const char *pattern, char *out, size_t out_sz)
{
    size_t i = 0;
    while (pattern[i] && i < out_sz - 1) {
        char c = pattern[i];
        if (c == '[' || c == '(' || c == '.' || c == '*' || c == '+' ||
            c == '?' || c == '|' || c == '\\' || c == '^' || c == '$')
            break;
        out[i++] = c;
    }
    while (i > 0 && out[i - 1] == ' ')
        i--;
    out[i] = '\0';
}

static void gen_id(sm_anomaly_detector_t *det, char *buf, size_t len)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(buf, len, "%08lx%04x%02x",
             (unsigned long)(ts.tv_sec & 0xFFFFFFFF),
             (unsigned)(ts.tv_nsec / 100000) & 0xFFFF,
             det->id_seq++ & 0xFF);
}

void sm_anomaly_init(sm_anomaly_detector_t *det)
{
    memset(det, 0, sizeof(*det));
    det->cooldown_s = SM_ANOMALY_COOLDOWN_MS / 1000.0;
    det->window_cap = 4096;
    det->window = malloc(det->window_cap);
    if (!det->window) det->window_cap = 0;
}

void sm_anomaly_destroy(sm_anomaly_detector_t *det)
{
    for (size_t i = 0; i < det->pattern_count; i++)
        sm_regex_free(det->patterns[i].compiled);
    free(det->patterns);
    free(det->incidents);
    free(det->window);
    memset(det, 0, sizeof(*det));
}

int sm_anomaly_add_pattern(sm_anomaly_detector_t *det,
                           const char *name, const char *pattern, const char *severity)
{
    if (det->pattern_count >= SM_MAX_ANOMALY_PATTERNS) return -1;

    if (det->pattern_count >= det->pattern_cap) {
        size_t new_cap = det->pattern_cap ? det->pattern_cap * 2 : 16;
        void *tmp = realloc(det->patterns, new_cap * sizeof(sm_anomaly_pattern_t));
        if (!tmp) return -1;
        det->patterns = tmp;
        det->pattern_cap = new_cap;
    }

    sm_anomaly_pattern_t *p = &det->patterns[det->pattern_count];
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s", name);
    snprintf(p->severity, sizeof(p->severity), "%s", severity);

    p->compiled = sm_regex_compile(pattern, NULL, 0);
    if (!p->compiled) return -1;

    extract_literal_prefix(pattern, p->literal, sizeof(p->literal));

    det->pattern_count++;
    return 0;
}

static int add_builtin(sm_anomaly_detector_t *det,
                       const char *name, const char *pattern,
                       const char *severity, const char *literal)
{
    if (sm_anomaly_add_pattern(det, name, pattern, severity) < 0)
        return -1;
    sm_anomaly_pattern_t *p = &det->patterns[det->pattern_count - 1];
    snprintf(p->literal, sizeof(p->literal), "%s", literal);
    return 0;
}

void sm_anomaly_add_builtins(sm_anomaly_detector_t *det)
{
    add_builtin(det, "kernel_panic", "Kernel panic", "critical", "Kernel panic");
    add_builtin(det, "oops", "Oops:", "critical", "Oops:");
    add_builtin(det, "hard_fault", "HardFault", "critical", "HardFault");
    add_builtin(det, "segfault", "Segmentation [Ff]ault", "critical",
                "Segmentation");
    add_builtin(det, "oom_killer", "Out of memory", "critical", "Out of memory");
    add_builtin(det, "stack_trace", "Call [Tt]race:", "warning", "Call ");
    add_builtin(det, "watchdog", "watchdog.*reset|wdt.*timeout", "warning",
                "watchdog;wdt");
    add_builtin(det, "assert_fail", "Assertion.*failed|BUG_ON|BUG:", "warning",
                "Assertion;BUG_ON;BUG:");
}

size_t sm_anomaly_feed(sm_anomaly_detector_t *det,
                       const uint8_t *data, size_t len, double ts)
{
    if (len == 0) return 0;

    /* Trim window before appending to prevent silent overflow */
    if (det->window_len > 8192) {
        size_t keep = 4096;
        size_t removed = det->window_len - keep;
        memmove(det->window, det->window + removed, keep);
        det->window_len = keep;
        det->search_offset = (det->search_offset > removed)
                              ? det->search_offset - removed : 0;
    }

    /* Append to sliding window */
    while (det->window_len + len >= det->window_cap) {
        size_t new_cap = det->window_cap * 2;
        if (new_cap > SM_MAX_ANOMALY_WINDOW) {
            /* Still can't fit — discard oldest data to make room */
            if (det->window_len > len) {
                size_t keep = det->window_cap / 2;
                if (keep < len) keep = len;
                if (keep > det->window_len) keep = det->window_len;
                size_t removed = det->window_len - keep;
                memmove(det->window, det->window + removed, keep);
                det->window_len = keep;
                det->search_offset = (det->search_offset > removed)
                                      ? det->search_offset - removed : 0;
            } else {
                det->window_len = 0;
                det->search_offset = 0;
            }
            break;
        }
        void *tmp = realloc(det->window, new_cap);
        if (!tmp) {
            SM_LOG_WARN(LOG_TAG, "window alloc failed: %zu bytes of device "
                                 "output not scanned for anomalies", len);
            return 0;
        }
        det->window = tmp;
        det->window_cap = new_cap;
    }
    if (det->window_len + len < det->window_cap) {
        memcpy(det->window + det->window_len, data, len);
        det->window_len += len;
    } else {
        /* Chunk larger than the window can ever hold — drop loudly (M11) */
        SM_LOG_WARN(LOG_TAG, "window overflow: %zu bytes of device output "
                             "not scanned for anomalies", len);
    }
    if (det->window_len < det->window_cap)
        det->window[det->window_len] = '\0';

    /* Update pre-context ring: keep the last SM_ANOMALY_CONTEXT_SIZE bytes
     * seen, so a recorded incident can include what preceded it (I6).
     * Three cases:
     *   1. chunk alone fills the ring   -> keep its last CONTEXT_SIZE bytes
     *   2. chunk fits in remaining room  -> append
     *   3. chunk fits only after evicting -> slide out the oldest `shift`
     *      bytes (shift = len - free space), then append
     * Invariant: context_len <= SM_ANOMALY_CONTEXT_SIZE at all times. */
    if (len >= SM_ANOMALY_CONTEXT_SIZE) {
        memcpy(det->context_buf, data + len - SM_ANOMALY_CONTEXT_SIZE,
               SM_ANOMALY_CONTEXT_SIZE);
        det->context_len = SM_ANOMALY_CONTEXT_SIZE;
    } else {
        size_t avail = SM_ANOMALY_CONTEXT_SIZE - det->context_len;
        if (len <= avail) {
            memcpy(det->context_buf + det->context_len, data, len);
            det->context_len += len;
        } else {
            size_t shift = len - avail;
            memmove(det->context_buf, det->context_buf + shift,
                    det->context_len - shift);
            det->context_len -= shift;
            memcpy(det->context_buf + det->context_len, data, len);
            det->context_len += len;
        }
    }

    size_t new_incidents = 0;

    /* Only search the newly-added region (with overlap for multi-byte patterns) */
    size_t overlap = 256;  /* enough for most patterns to span chunk boundaries */
    size_t search_start = det->search_offset > overlap
                          ? det->search_offset - overlap : 0;
    const char *search_str = (const char *)det->window + search_start;

    /* Check each pattern against new data */
    for (size_t i = 0; i < det->pattern_count; i++) {
        sm_anomaly_pattern_t *p = &det->patterns[i];

        /* Cooldown check */
        if (p->last_fire_time > 0 && (ts - p->last_fire_time) < det->cooldown_s)
            continue;

        /* Literal prefilter: skip regex when no needle present in search region */
        if (!region_has_literal(search_str, p->literal))
            continue;

        size_t search_len = det->window_len - search_start;
        size_t match_off = 0;
        if (sm_regex_exec(p->compiled, search_str, search_len, &match_off) == 0) {
            p->last_fire_time = ts;

            /* Record incident — batch-evict the oldest quarter at cap so
             * eviction cost is amortized instead of moving the whole
             * ~1.4MB array on every new incident (M7) */
            if (det->incident_count >= SM_MAX_ANOMALY_INCIDENTS) {
                size_t drop = SM_MAX_ANOMALY_INCIDENTS / 4;
                memmove(det->incidents, det->incidents + drop,
                        (det->incident_count - drop) *
                            sizeof(sm_anomaly_incident_t));
                det->incident_count -= drop;
                SM_LOG_WARN(LOG_TAG, "incident store full: dropped %zu "
                                     "oldest incidents", drop);
            }
            if (det->incident_count >= det->incident_cap) {
                size_t new_cap = det->incident_cap ? det->incident_cap * 2 : 16;
                void *tmp = realloc(det->incidents,
                                     new_cap * sizeof(sm_anomaly_incident_t));
                if (!tmp) continue;
                det->incidents = tmp;
                det->incident_cap = new_cap;
            }

            sm_anomaly_incident_t *inc = &det->incidents[det->incident_count++];
            gen_id(det, inc->id, sizeof(inc->id));
            snprintf(inc->pattern_name, sizeof(inc->pattern_name), "%s", p->name);
            snprintf(inc->severity, sizeof(inc->severity), "%s", p->severity);
            inc->timestamp = ts;

            /* Match text: capture the matched line (and what follows), not the
             * window head. match_off is relative to search_str, so translate it
             * to a window offset, then rewind to the start of that line so the
             * signature ("Kernel panic ...") reads from its beginning rather
             * than from stale bytes at window[0]. */
            size_t win_match = search_start + match_off;
            if (win_match > det->window_len) win_match = det->window_len;
            size_t line_start = win_match;
            while (line_start > 0 && det->window[line_start - 1] != '\n')
                line_start--;
            size_t avail = det->window_len - line_start;
            size_t mt_len = avail < sizeof(inc->match_text) - 1
                            ? avail : sizeof(inc->match_text) - 1;
            memcpy(inc->match_text, det->window + line_start, mt_len);
            inc->match_text[mt_len] = '\0';

            /* Pre-context */
            size_t ctx_len = det->context_len < sizeof(inc->pre_context) - 1
                             ? det->context_len : sizeof(inc->pre_context) - 1;
            memcpy(inc->pre_context, det->context_buf, ctx_len);
            inc->pre_context[ctx_len] = '\0';

            new_incidents++;
        }
    }

    /* Update search offset to current end of window */
    det->search_offset = det->window_len;

    return new_incidents;
}

const sm_anomaly_incident_t *sm_anomaly_get_incidents(const sm_anomaly_detector_t *det,
                                                      size_t *count)
{
    *count = det->incident_count;
    return det->incidents;
}
