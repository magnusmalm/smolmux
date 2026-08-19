#ifndef SM_REGEX_ENGINE_H
#define SM_REGEX_ENGINE_H

#include <stddef.h>

/* Maximum allowed pattern length to reject overly complex regexes */
#define SM_REGEX_MAX_PATTERN_LEN 1024

typedef struct sm_regex sm_regex_t;

sm_regex_t *sm_regex_compile(const char *pattern, char *errbuf, size_t errbuf_len);
/* Execute regex against str[0..len). Returns 0 on match, non-zero otherwise.
 * If match_off is non-NULL, on a match it receives the byte offset of the
 * match start within str (its value is unspecified when there is no match). */
int sm_regex_exec(sm_regex_t *re, const char *str, size_t len, size_t *match_off);
void sm_regex_free(sm_regex_t *re);
const char *sm_regex_backend(void);

#endif /* SM_REGEX_ENGINE_H */
