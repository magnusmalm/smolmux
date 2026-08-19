#ifndef SM_AUTORESPONDER_H
#define SM_AUTORESPONDER_H

#include <stddef.h>
#include <stdint.h>
#include "regex_engine.h"

/*
 * Autoresponder: standing expect->send rules evaluated in the broker read path.
 *
 * Each rule pairs a regex against a response to write when the device output
 * matches — for menus ("Press any key"), y/n prompts, or a login sequence —
 * with zero client round-trip. A per-rule cooldown prevents re-firing on the
 * same still-visible prompt text; `once` disables a rule after its first fire.
 *
 * The engine only matches and reports which rules fired (with the bytes to
 * send); the broker performs the actual device write.
 */

typedef struct sm_ar_rule {
    char name[64];
    char pattern[256];
    sm_regex_t *compiled;
    uint8_t response[256];
    size_t response_len;
    int once;               /* disable after the first fire */
    int enabled;
    double cooldown_s;
    double last_fire_time;
} sm_ar_rule_t;

/* One fired rule, returned from feed(). `response` points into the rule and is
 * valid until the next add/remove/clear. */
typedef struct sm_ar_fired {
    int rule_index;
    const uint8_t *response;
    size_t response_len;
    char name[64];
    char matched_text[128];
} sm_ar_fired_t;

typedef struct sm_autoresponder {
    sm_ar_rule_t *rules;
    size_t rule_count;
    size_t rule_cap;
    uint8_t *window;
    size_t window_len;
    size_t window_cap;
    size_t search_offset;
} sm_autoresponder_t;

void sm_autoresponder_init(sm_autoresponder_t *ar);
void sm_autoresponder_destroy(sm_autoresponder_t *ar);

/* Add or replace (by name) a rule. cooldown_ms <= 0 uses the default.
 * Returns 0 on success, -1 on failure (bad regex, full, out of memory). */
int sm_autoresponder_add(sm_autoresponder_t *ar, const char *name,
                         const char *pattern, const uint8_t *response,
                         size_t response_len, int once, int cooldown_ms);

/* Remove a rule by name. Returns 1 if removed, 0 if not found. */
int sm_autoresponder_remove(sm_autoresponder_t *ar, const char *name);

/* Remove all rules. */
void sm_autoresponder_clear(sm_autoresponder_t *ar);

/* Feed device output. Fills `out` with up to max_out fired rules and returns
 * the count. */
size_t sm_autoresponder_feed(sm_autoresponder_t *ar, const uint8_t *data,
                             size_t len, double ts,
                             sm_ar_fired_t *out, size_t max_out);

#endif /* SM_AUTORESPONDER_H */
