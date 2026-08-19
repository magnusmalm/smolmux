#include "boot_stage.h"
#include "constants.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sm_boot_init(sm_boot_tracker_t *t)
{
    memset(t, 0, sizeof(*t));
    t->furthest = -1;
    t->stall_timeout_s = SM_BOOT_STALL_TIMEOUT_MS / 1000.0;
    t->window_cap = SM_BOOT_WINDOW_BYTES;
    t->window = malloc(t->window_cap);
    if (!t->window) t->window_cap = 0;
}

void sm_boot_destroy(sm_boot_tracker_t *t)
{
    for (size_t i = 0; i < t->stage_count; i++)
        sm_regex_free(t->stages[i].compiled);
    free(t->stages);
    free(t->window);
    memset(t, 0, sizeof(*t));
    t->furthest = -1;
}

int sm_boot_add_stage(sm_boot_tracker_t *t, const char *name, const char *pattern)
{
    if (t->stage_count >= SM_MAX_BOOT_STAGES) return -1;

    if (t->stage_count >= t->stage_cap) {
        size_t new_cap = t->stage_cap ? t->stage_cap * 2 : 8;
        if (new_cap > SM_MAX_BOOT_STAGES) new_cap = SM_MAX_BOOT_STAGES;
        void *tmp = realloc(t->stages, new_cap * sizeof(sm_boot_stage_t));
        if (!tmp) return -1;
        t->stages = tmp;
        t->stage_cap = new_cap;
    }

    sm_boot_stage_t *s = &t->stages[t->stage_count];
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", name);
    snprintf(s->pattern, sizeof(s->pattern), "%s", pattern);
    s->compiled = sm_regex_compile(pattern, NULL, 0);
    if (!s->compiled) return -1;

    t->stage_count++;
    return 0;
}

void sm_boot_set_stall_timeout(sm_boot_tracker_t *t, int timeout_ms)
{
    if (timeout_ms > 0)
        t->stall_timeout_s = timeout_ms / 1000.0;
}

void sm_boot_reset(sm_boot_tracker_t *t)
{
    for (size_t i = 0; i < t->stage_count; i++) {
        t->stages[i].reached = 0;
        t->stages[i].announced = 0;
        t->stages[i].reached_ts = 0.0;
    }
    t->furthest = -1;
    t->last_advance_ts = 0.0;
}

/* Append data to the fixed-size sliding window, dropping the oldest bytes when
 * it would overflow. Keeps the buffer NUL-terminated for the regex engine and
 * slides search_offset along with any evicted prefix. */
static void window_append(sm_boot_tracker_t *t, const uint8_t *data, size_t len)
{
    if (t->window_cap == 0) return;

    /* A chunk that can't fit at all: keep only its trailing tail. */
    if (len >= t->window_cap) {
        size_t keep = t->window_cap - 1;
        memcpy(t->window, data + len - keep, keep);
        t->window_len = keep;
        t->window[keep] = '\0';
        t->search_offset = 0;
        return;
    }

    /* Evict oldest bytes if appending would overflow. */
    if (t->window_len + len > t->window_cap - 1) {
        size_t need = (t->window_len + len) - (t->window_cap - 1);
        memmove(t->window, t->window + need, t->window_len - need);
        t->window_len -= need;
        t->search_offset = (t->search_offset > need) ? t->search_offset - need : 0;
    }

    memcpy(t->window + t->window_len, data, len);
    t->window_len += len;
    t->window[t->window_len] = '\0';
}

size_t sm_boot_feed(sm_boot_tracker_t *t, const uint8_t *data, size_t len, double ts)
{
    if (len == 0 || t->stage_count == 0 || t->window_cap == 0)
        return 0;

    window_append(t, data, len);

    /* Reboot heuristic: if stage 0 reappears in the freshly-arrived bytes after
     * we had already advanced past it, treat it as a new boot. Match only the
     * strictly-new region (not the overlap) so a stale stage-0 marker still in
     * the window does not trigger it, then discard everything before the new
     * marker so the rescan below cannot re-reach later stages from stale text. */
    if (t->furthest >= 1 && t->stages[0].compiled) {
        const char *newp = (const char *)t->window + t->search_offset;
        size_t newlen = t->window_len - t->search_offset;
        size_t noff = 0;
        if (sm_regex_exec(t->stages[0].compiled, newp, newlen, &noff) == 0) {
            size_t abs = t->search_offset + noff;
            if (abs > 0) {
                memmove(t->window, t->window + abs, t->window_len - abs);
                t->window_len -= abs;
                t->window[t->window_len] = '\0';
            }
            t->search_offset = 0;
            sm_boot_reset(t);
        }
    }

    /* Search the newly-added region plus a small overlap so a marker straddling
     * a chunk boundary is not missed. */
    const size_t overlap = 256;
    size_t search_start = (t->search_offset > overlap) ? t->search_offset - overlap : 0;
    const char *region = (const char *)t->window + search_start;
    size_t region_len = t->window_len - search_start;

    size_t newly = 0;
    for (size_t i = 0; i < t->stage_count; i++) {
        sm_boot_stage_t *s = &t->stages[i];
        if (s->reached || !s->compiled) continue;

        size_t off = 0;
        if (sm_regex_exec(s->compiled, region, region_len, &off) == 0) {
            s->reached = 1;
            s->reached_ts = ts;
            if ((int)i > t->furthest) t->furthest = (int)i;
            t->last_advance_ts = ts;
            newly++;
        }
    }

    t->search_offset = t->window_len;
    return newly;
}

int sm_boot_terminal_reached(const sm_boot_tracker_t *t)
{
    if (t->stage_count == 0) return 0;
    return t->stages[t->stage_count - 1].reached;
}

int sm_boot_stalled(const sm_boot_tracker_t *t, double now)
{
    if (t->stage_count == 0) return 0;
    if (t->furthest < 0) return 0;                 /* boot not started */
    if (t->stall_timeout_s <= 0) return 0;
    if (t->last_advance_ts <= 0) return 0;
    if (sm_boot_terminal_reached(t)) return 0;     /* fully booted */
    return (now - t->last_advance_ts) > t->stall_timeout_s;
}
