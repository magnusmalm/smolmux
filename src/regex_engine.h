#ifndef SM_REGEX_ENGINE_H
#define SM_REGEX_ENGINE_H

#include <stddef.h>

/* Maximum allowed pattern length to reject overly complex regexes */
#define SM_REGEX_MAX_PATTERN_LEN 1024

/* Wall-clock budget for a single match. Patterns come from clients, run
 * inside the broker's single-threaded event loop, and are re-run against
 * every chunk of device output, so one slow match stalls everything. A
 * pattern that exceeds this once is disabled and reports no-match from then
 * on: the static checks in check_redos_risk() are a heuristic and cannot be
 * complete, so this measured bound is what actually caps the damage.
 * Legitimate patterns finish in microseconds; this leaves ~1000x headroom. */
#define SM_REGEX_MAX_MATCH_MS 50

/* Pattern prefix kept for diagnostics, so a disable warning can name it. */
#define SM_REGEX_LOG_PATTERN_LEN 96

typedef struct sm_regex sm_regex_t;

sm_regex_t *sm_regex_compile(const char *pattern, char *errbuf, size_t errbuf_len);
/* Execute regex against str[0..len). Returns 0 on match, non-zero otherwise.
 * If match_off is non-NULL, on a match it receives the byte offset of the
 * match start within str (its value is unspecified when there is no match). */
int sm_regex_exec(sm_regex_t *re, const char *str, size_t len, size_t *match_off);
void sm_regex_free(sm_regex_t *re);
const char *sm_regex_backend(void);

#endif /* SM_REGEX_ENGINE_H */
