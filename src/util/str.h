#ifndef SM_STR_H
#define SM_STR_H

#include <stddef.h>
#include <stdint.h>

/* Interpret common backslash escapes (\n \r \t \0 \\) in `s` into `out`,
 * copying other chars verbatim. Returns the decoded byte count (<= out_max). */
size_t sm_str_unescape(const char *s, uint8_t *out, size_t out_max);

typedef struct sm_strbuf {
    char *data;
    size_t len;
    size_t cap;
} sm_strbuf_t;

void sm_strbuf_init(sm_strbuf_t *sb);
void sm_strbuf_append(sm_strbuf_t *sb, const char *s, size_t len);
void sm_strbuf_append_str(sm_strbuf_t *sb, const char *s);
void sm_strbuf_printf(sm_strbuf_t *sb, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
char *sm_strbuf_steal(sm_strbuf_t *sb);
void sm_strbuf_destroy(sm_strbuf_t *sb);

#endif /* SM_STR_H */
