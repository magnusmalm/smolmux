#include "util/mi_parse.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Recursive-descent cursor over one MI line. */
typedef struct {
    const char *s;
    size_t len;
    size_t pos;
} cur_t;

static cJSON *parse_value(cur_t *c);
static cJSON *parse_object_until(cur_t *c, int close);

/* Parse a GDB/MI c-string starting at c->pos == '"'. Returns a newly allocated,
 * unescaped, NUL-terminated string and advances past the closing quote. Handles
 * the common C escapes; an unknown escape keeps the escaped character verbatim
 * (dropping the backslash), matching GDB/MI string escaping. */
static char *parse_cstring(cur_t *c)
{
    c->pos++;  /* skip opening quote */
    /* Unescaping never grows the string, so remaining bytes + NUL is enough. */
    char *out = malloc(c->len - c->pos + 1);
    if (!out) return NULL;

    size_t o = 0;
    while (c->pos < c->len) {
        char ch = c->s[c->pos];
        if (ch == '\\' && c->pos + 1 < c->len) {
            char n = c->s[c->pos + 1];
            switch (n) {
            case 'n': out[o++] = '\n'; break;
            case 't': out[o++] = '\t'; break;
            case 'r': out[o++] = '\r'; break;
            case '"': out[o++] = '"';  break;
            case '\\': out[o++] = '\\'; break;
            default: out[o++] = n; break;  /* drop backslash, keep char */
            }
            c->pos += 2;
            continue;
        }
        if (ch == '"') { c->pos++; break; }  /* closing quote */
        out[o++] = ch;
        c->pos++;
    }
    out[o] = '\0';
    return out;
}

/* Parse a bare value: everything up to the next ',', '}' or ']'. */
static char *parse_bare(cur_t *c)
{
    size_t start = c->pos;
    while (c->pos < c->len && c->s[c->pos] != ',' &&
           c->s[c->pos] != '}' && c->s[c->pos] != ']')
        c->pos++;
    size_t n = c->pos - start;
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, c->s + start, n);
    out[n] = '\0';
    return out;
}

static cJSON *parse_list(cur_t *c)
{
    cJSON *arr = cJSON_CreateArray();
    c->pos++;  /* skip '[' */
    while (c->pos < c->len) {
        char ch = c->s[c->pos];
        if (ch == ']') { c->pos++; break; }
        if (ch == ',') { c->pos++; continue; }

        /* A list may hold plain values ([0,1,2]) or "result" entries
         * (frame={...},frame={...}); represent the latter as one-key objects,
         * so tool code can iterate them uniformly. */
        if (isalpha((unsigned char)ch) || ch == '_') {
            size_t look = c->pos;
            while (look < c->len && (isalnum((unsigned char)c->s[look]) ||
                                     c->s[look] == '_' || c->s[look] == '-'))
                look++;
            if (look < c->len && c->s[look] == '=') {
                size_t kn = look - c->pos;
                char *key = malloc(kn + 1);
                if (!key) break;
                memcpy(key, c->s + c->pos, kn);
                key[kn] = '\0';
                c->pos = look + 1;  /* skip key and '=' */
                cJSON *val = parse_value(c);
                cJSON *wrap = cJSON_CreateObject();
                cJSON_AddItemToObject(wrap, key, val);
                cJSON_AddItemToArray(arr, wrap);
                free(key);
                continue;
            }
        }
        cJSON_AddItemToArray(arr, parse_value(c));
    }
    return arr;
}

static cJSON *parse_value(cur_t *c)
{
    if (c->pos >= c->len) return cJSON_CreateString("");
    char ch = c->s[c->pos];
    if (ch == '"') {
        char *s = parse_cstring(c);
        cJSON *j = cJSON_CreateString(s ? s : "");
        free(s);
        return j;
    }
    if (ch == '{') { c->pos++; return parse_object_until(c, '}'); }
    if (ch == '[') return parse_list(c);
    char *s = parse_bare(c);
    cJSON *j = cJSON_CreateString(s ? s : "");
    free(s);
    return j;
}

/* Parse "key=value" pairs into an object. close is '}' for a tuple, or -1 to
 * parse to end-of-input (the top-level results of a record). */
static cJSON *parse_object_until(cur_t *c, int close)
{
    cJSON *obj = cJSON_CreateObject();
    while (c->pos < c->len) {
        char ch = c->s[c->pos];
        if (close != -1 && ch == (char)close) { c->pos++; break; }
        if (ch == ',') { c->pos++; continue; }

        /* Read key up to '='. Stop at ',' or the close char too, so a
         * malformed pair can't run away or spin. */
        size_t ks = c->pos;
        while (c->pos < c->len && c->s[c->pos] != '=' && c->s[c->pos] != ',' &&
               (close == -1 || c->s[c->pos] != (char)close))
            c->pos++;
        if (c->pos >= c->len || c->s[c->pos] != '=')
            break;  /* no '=' — malformed; stop rather than loop */

        size_t kn = c->pos - ks;
        char *key = malloc(kn + 1);
        if (!key) break;
        memcpy(key, c->s + ks, kn);
        key[kn] = '\0';
        c->pos++;  /* skip '=' */
        cJSON *val = parse_value(c);
        cJSON_AddItemToObject(obj, key, val);
        free(key);
    }
    return obj;
}

int sm_mi_parse_line(const char *line, size_t len, sm_mi_record_t *out)
{
    memset(out, 0, sizeof(*out));
    out->token = -1;

    /* Tolerate a trailing CR/LF (single line expected). */
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        len--;

    if (len == 5 && memcmp(line, "(gdb)", 5) == 0) {
        out->type = SM_MI_PROMPT;
        return 0;
    }

    size_t pos = 0;
    long token = -1;
    int had_token = 0;
    while (pos < len && isdigit((unsigned char)line[pos])) {
        if (!had_token) { token = 0; had_token = 1; }
        token = token * 10 + (line[pos] - '0');
        pos++;
    }
    if (pos >= len) return -1;

    sm_mi_type_t type;
    int is_stream = 0;
    switch (line[pos]) {
    case '^': type = SM_MI_RESULT; break;
    case '*': type = SM_MI_EXEC_ASYNC; break;
    case '+': type = SM_MI_STATUS_ASYNC; break;
    case '=': type = SM_MI_NOTIFY_ASYNC; break;
    case '~': type = SM_MI_CONSOLE; is_stream = 1; break;
    case '@': type = SM_MI_TARGET; is_stream = 1; break;
    case '&': type = SM_MI_LOG; is_stream = 1; break;
    default: return -1;
    }
    pos++;

    /* Streams never carry a token; digits before a stream char are not a valid
     * record (matches the reference, which returns no match). */
    if (is_stream && had_token) return -1;

    out->type = type;

    if (is_stream) {
        cur_t c = { line, len, pos };
        if (pos < len && line[pos] == '"')
            out->stream_data = parse_cstring(&c);
        if (!out->stream_data)
            out->stream_data = strdup("");
        return 0;
    }

    out->token = had_token ? token : -1;

    /* Class runs to the first ',' (or end of line). */
    size_t cs = pos;
    while (pos < len && line[pos] != ',') pos++;
    size_t cn = pos - cs;
    if (cn >= sizeof(out->class_)) cn = sizeof(out->class_) - 1;
    memcpy(out->class_, line + cs, cn);
    out->class_[cn] = '\0';

    if (pos < len && line[pos] == ',') {
        cur_t c = { line, len, pos + 1 };
        out->results = parse_object_until(&c, -1);
    } else {
        out->results = cJSON_CreateObject();
    }
    return 0;
}

void sm_mi_record_free(sm_mi_record_t *rec)
{
    if (!rec) return;
    if (rec->results) { cJSON_Delete(rec->results); rec->results = NULL; }
    if (rec->stream_data) { free(rec->stream_data); rec->stream_data = NULL; }
}
