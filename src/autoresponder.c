#include "autoresponder.h"
#include "constants.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sm_autoresponder_init(sm_autoresponder_t *ar)
{
    memset(ar, 0, sizeof(*ar));
    ar->window_cap = SM_AR_WINDOW_BYTES;
    ar->window = malloc(ar->window_cap);
    if (!ar->window) ar->window_cap = 0;
}

static void free_rule(sm_ar_rule_t *r)
{
    sm_regex_free(r->compiled);
    r->compiled = NULL;
}

void sm_autoresponder_destroy(sm_autoresponder_t *ar)
{
    for (size_t i = 0; i < ar->rule_count; i++)
        free_rule(&ar->rules[i]);
    free(ar->rules);
    free(ar->window);
    memset(ar, 0, sizeof(*ar));
}

void sm_autoresponder_clear(sm_autoresponder_t *ar)
{
    for (size_t i = 0; i < ar->rule_count; i++)
        free_rule(&ar->rules[i]);
    ar->rule_count = 0;
}

static sm_ar_rule_t *find_rule(sm_autoresponder_t *ar, const char *name)
{
    for (size_t i = 0; i < ar->rule_count; i++)
        if (strcmp(ar->rules[i].name, name) == 0)
            return &ar->rules[i];
    return NULL;
}

int sm_autoresponder_add(sm_autoresponder_t *ar, const char *name,
                         const char *pattern, const uint8_t *response,
                         size_t response_len, int once, int cooldown_ms)
{
    if (!name || !name[0] || !pattern || !pattern[0]) return -1;
    if (response_len > SM_AR_RESPONSE_MAX) return -1;

    sm_regex_t *re = sm_regex_compile(pattern, NULL, 0);
    if (!re) return -1;

    /* Replace an existing rule of the same name in place. */
    sm_ar_rule_t *r = find_rule(ar, name);
    if (!r) {
        if (ar->rule_count >= SM_MAX_AUTORESPONDERS) { sm_regex_free(re); return -1; }
        if (ar->rule_count >= ar->rule_cap) {
            size_t new_cap = ar->rule_cap ? ar->rule_cap * 2 : 8;
            if (new_cap > SM_MAX_AUTORESPONDERS) new_cap = SM_MAX_AUTORESPONDERS;
            void *tmp = realloc(ar->rules, new_cap * sizeof(sm_ar_rule_t));
            if (!tmp) { sm_regex_free(re); return -1; }
            ar->rules = tmp;
            ar->rule_cap = new_cap;
        }
        r = &ar->rules[ar->rule_count++];
        memset(r, 0, sizeof(*r));
    } else {
        sm_regex_free(r->compiled);
    }

    snprintf(r->name, sizeof(r->name), "%s", name);
    snprintf(r->pattern, sizeof(r->pattern), "%s", pattern);
    r->compiled = re;
    r->response_len = response_len;
    if (response_len > 0) memcpy(r->response, response, response_len);
    r->once = once ? 1 : 0;
    r->enabled = 1;
    r->cooldown_s = (cooldown_ms > 0 ? cooldown_ms : SM_AR_DEFAULT_COOLDOWN_MS) / 1000.0;
    r->last_fire_time = 0.0;
    return 0;
}

int sm_autoresponder_remove(sm_autoresponder_t *ar, const char *name)
{
    for (size_t i = 0; i < ar->rule_count; i++) {
        if (strcmp(ar->rules[i].name, name) == 0) {
            free_rule(&ar->rules[i]);
            memmove(&ar->rules[i], &ar->rules[i + 1],
                    (ar->rule_count - i - 1) * sizeof(sm_ar_rule_t));
            ar->rule_count--;
            return 1;
        }
    }
    return 0;
}

/* Append to the fixed-size sliding window, dropping oldest on overflow. Keeps
 * a NUL terminator for the regex engine and slides search_offset. */
static void window_append(sm_autoresponder_t *ar, const uint8_t *data, size_t len)
{
    if (ar->window_cap == 0) return;

    if (len >= ar->window_cap) {
        size_t keep = ar->window_cap - 1;
        memcpy(ar->window, data + len - keep, keep);
        ar->window_len = keep;
        ar->window[keep] = '\0';
        ar->search_offset = 0;
        return;
    }
    if (ar->window_len + len > ar->window_cap - 1) {
        size_t need = (ar->window_len + len) - (ar->window_cap - 1);
        memmove(ar->window, ar->window + need, ar->window_len - need);
        ar->window_len -= need;
        ar->search_offset = (ar->search_offset > need) ? ar->search_offset - need : 0;
    }
    memcpy(ar->window + ar->window_len, data, len);
    ar->window_len += len;
    ar->window[ar->window_len] = '\0';
}

size_t sm_autoresponder_feed(sm_autoresponder_t *ar, const uint8_t *data,
                             size_t len, double ts,
                             sm_ar_fired_t *out, size_t max_out)
{
    if (len == 0 || ar->rule_count == 0 || ar->window_cap == 0 || max_out == 0)
        return 0;

    window_append(ar, data, len);

    const size_t overlap = 256;
    size_t search_start = (ar->search_offset > overlap) ? ar->search_offset - overlap : 0;
    const char *region = (const char *)ar->window + search_start;
    size_t region_len = ar->window_len - search_start;

    size_t fired = 0;
    for (size_t i = 0; i < ar->rule_count && fired < max_out; i++) {
        sm_ar_rule_t *r = &ar->rules[i];
        if (!r->enabled || !r->compiled) continue;
        if (r->last_fire_time > 0 && (ts - r->last_fire_time) < r->cooldown_s)
            continue;

        size_t off = 0;
        if (sm_regex_exec(r->compiled, region, region_len, &off) != 0)
            continue;

        r->last_fire_time = ts;

        sm_ar_fired_t *f = &out[fired++];
        f->rule_index = (int)i;
        f->response = r->response;
        f->response_len = r->response_len;
        snprintf(f->name, sizeof(f->name), "%s", r->name);

        /* matched_text: the line containing the match. */
        size_t win_match = search_start + off;
        if (win_match > ar->window_len) win_match = ar->window_len;
        size_t line_start = win_match;
        while (line_start > 0 && ar->window[line_start - 1] != '\n')
            line_start--;
        size_t avail = ar->window_len - line_start;
        size_t mt = avail < sizeof(f->matched_text) - 1 ? avail : sizeof(f->matched_text) - 1;
        memcpy(f->matched_text, ar->window + line_start, mt);
        f->matched_text[mt] = '\0';

        if (r->once) r->enabled = 0;
    }

    ar->search_offset = ar->window_len;
    return fired;
}
