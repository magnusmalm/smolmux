#include "util/str.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

size_t sm_str_unescape(const char *s, uint8_t *out, size_t out_max)
{
    size_t n = 0;
    for (const char *p = s; *p && n < out_max; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n':  out[n++] = '\n'; break;
            case 'r':  out[n++] = '\r'; break;
            case 't':  out[n++] = '\t'; break;
            case '0':  out[n++] = '\0'; break;
            case '\\': out[n++] = '\\'; break;
            default:   out[n++] = (uint8_t)*p; break;
            }
        } else {
            out[n++] = (uint8_t)*p;
        }
    }
    return n;
}

void sm_strbuf_init(sm_strbuf_t *sb)
{
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

static int sm_strbuf_grow(sm_strbuf_t *sb, size_t needed)
{
    size_t new_cap = sb->cap ? sb->cap : 64;
    while (new_cap < sb->len + needed + 1)
        new_cap *= 2;
    if (new_cap > sb->cap) {
        void *tmp = realloc(sb->data, new_cap);
        if (!tmp) return -1;
        sb->data = tmp;
        sb->cap = new_cap;
    }
    return 0;
}

void sm_strbuf_append(sm_strbuf_t *sb, const char *s, size_t len)
{
    if (len == 0) return;
    if (sm_strbuf_grow(sb, len) < 0) return;
    memcpy(sb->data + sb->len, s, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
}

void sm_strbuf_append_str(sm_strbuf_t *sb, const char *s)
{
    if (s) sm_strbuf_append(sb, s, strlen(s));
}

void sm_strbuf_printf(sm_strbuf_t *sb, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed > 0) {
        if (sm_strbuf_grow(sb, (size_t)needed) < 0) { va_end(ap2); return; }
        vsnprintf(sb->data + sb->len, (size_t)needed + 1, fmt, ap2);
        sb->len += (size_t)needed;
    }
    va_end(ap2);
}

char *sm_strbuf_steal(sm_strbuf_t *sb)
{
    char *result = sb->data;
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
    return result;
}

void sm_strbuf_destroy(sm_strbuf_t *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}
