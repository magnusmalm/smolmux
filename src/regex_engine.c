#include "regex_engine.h"
#include "sm_features.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Reject patterns that exceed length limit */
static int check_pattern_length(const char *pattern, char *errbuf, size_t errbuf_len)
{
    if (strlen(pattern) > SM_REGEX_MAX_PATTERN_LEN) {
        if (errbuf && errbuf_len > 0)
            snprintf(errbuf, errbuf_len, "pattern too long (max %d)",
                     SM_REGEX_MAX_PATTERN_LEN);
        return -1;
    }
    return 0;
}

#if SM_ENABLE_PCRE2

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

struct sm_regex {
    pcre2_code *code;
    pcre2_match_context *mctx;
    pcre2_match_data *mdata;
};

sm_regex_t *sm_regex_compile(const char *pattern, char *errbuf, size_t errbuf_len)
{
    if (check_pattern_length(pattern, errbuf, errbuf_len) < 0)
        return NULL;

    int errcode;
    PCRE2_SIZE erroffset;
    pcre2_code *code = pcre2_compile(
        (PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
        0, &errcode, &erroffset, NULL);
    if (!code) {
        if (errbuf && errbuf_len > 0)
            pcre2_get_error_message(errcode, (PCRE2_UCHAR *)errbuf, errbuf_len);
        return NULL;
    }

    /* Create match context with limits to prevent ReDoS */
    pcre2_match_context *mctx = pcre2_match_context_create(NULL);
    if (mctx) {
        pcre2_set_match_limit(mctx, 100000);
        pcre2_set_depth_limit(mctx, 1000);
    }

    pcre2_match_data *mdata = pcre2_match_data_create_from_pattern(code, NULL);

    sm_regex_t *re = malloc(sizeof(*re));
    if (!re) {
        pcre2_match_data_free(mdata);
        pcre2_code_free(code);
        pcre2_match_context_free(mctx);
        return NULL;
    }
    re->code = code;
    re->mctx = mctx;
    re->mdata = mdata;
    return re;
}

int sm_regex_exec(sm_regex_t *re, const char *str, size_t len, size_t *match_off)
{
    if (!re || !re->mdata) return 1;
    int rc = pcre2_match(re->code, (PCRE2_SPTR)str, len,
                         0, 0, re->mdata, re->mctx);
    if (rc < 0) return 1;
    if (match_off) {
        PCRE2_SIZE *ov = pcre2_get_ovector_pointer(re->mdata);
        *match_off = (size_t)ov[0];
    }
    return 0;
}

void sm_regex_free(sm_regex_t *re)
{
    if (re) {
        pcre2_code_free(re->code);
        if (re->mctx)
            pcre2_match_context_free(re->mctx);
        if (re->mdata)
            pcre2_match_data_free(re->mdata);
        free(re);
    }
}

const char *sm_regex_backend(void)
{
    return "pcre2";
}

#else /* POSIX ERE */

#include <regex.h>
#include <stdio.h>

struct sm_regex {
    regex_t compiled;
};

/* --- ReDoS risk detection helpers --- */

static void redos_set_error(char *errbuf, size_t errbuf_len, const char *msg)
{
    if (errbuf && errbuf_len > 0)
        snprintf(errbuf, errbuf_len, "%s", msg);
}

static int is_quantifier(char c)
{
    return c == '+' || c == '*' || c == '?' || c == '{';
}

/* Skip past a character class [...], advancing p to the closing ']'. */
static const char *skip_char_class(const char *p)
{
    p++;  /* skip '[' */
    if (*p == '^') p++;
    if (*p == ']') p++;
    while (*p && *p != ']') {
        if (*p == '\\' && p[1]) p++;
        p++;
    }
    return p;
}

/* Check if a quantified group close triggers a ReDoS risk. */
static int check_quantified_group(int has_quantifier, int has_alternation,
                                  char *errbuf, size_t errbuf_len)
{
    if (has_quantifier) {
        redos_set_error(errbuf, errbuf_len,
                        "nested quantifier rejected (ReDoS risk)");
        return -1;
    }
    if (has_alternation) {
        redos_set_error(errbuf, errbuf_len,
                        "quantified alternation rejected (ReDoS risk)");
        return -1;
    }
    return 0;
}

/* Reject patterns that risk exponential backtracking in POSIX ERE. */
static int check_redos_risk(const char *pattern, char *errbuf, size_t errbuf_len)
{
    int depth = 0;
    int group_has_quantifier = 0;
    int group_has_alternation = 0;
    int consecutive_wildcards = 0;
    int total_quantifiers = 0;

    for (const char *p = pattern; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            consecutive_wildcards = 0;
            continue;
        }

        if (*p == '[') {
            p = skip_char_class(p);
            consecutive_wildcards = 0;
            continue;
        }

        if (*p == '(') {
            depth++;
            group_has_quantifier = 0;
            group_has_alternation = 0;
            consecutive_wildcards = 0;
            continue;
        }

        if (*p == ')') {
            depth--;
            if (is_quantifier(p[1])) {
                if (check_quantified_group(group_has_quantifier,
                        group_has_alternation, errbuf, errbuf_len) < 0)
                    return -1;
            }
            group_has_quantifier = 0;
            group_has_alternation = 0;
            consecutive_wildcards = 0;
            continue;
        }

        if (*p == '|') {
            if (depth > 0) group_has_alternation = 1;
            consecutive_wildcards = 0;
            continue;
        }

        if (*p == '+' || *p == '*' || *p == '{') {
            total_quantifiers++;
            if (depth > 0) group_has_quantifier = 1;

            if (p > pattern && p[-1] == '.') {
                consecutive_wildcards++;
                if (consecutive_wildcards >= 3) {
                    redos_set_error(errbuf, errbuf_len,
                                   "repeated wildcards rejected (ReDoS risk)");
                    return -1;
                }
            } else {
                consecutive_wildcards = 0;
            }
            continue;
        }

        if (*p != '.') consecutive_wildcards = 0;
    }

    if (total_quantifiers > 20) {
        char msg[64];
        snprintf(msg, sizeof(msg), "too many quantifiers (%d, max 20)",
                 total_quantifiers);
        redos_set_error(errbuf, errbuf_len, msg);
        return -1;
    }

    return 0;
}

sm_regex_t *sm_regex_compile(const char *pattern, char *errbuf, size_t errbuf_len)
{
    if (check_pattern_length(pattern, errbuf, errbuf_len) < 0)
        return NULL;

    if (check_redos_risk(pattern, errbuf, errbuf_len) < 0)
        return NULL;

    sm_regex_t *re = malloc(sizeof(*re));
    if (!re) {
        if (errbuf && errbuf_len > 0)
            snprintf(errbuf, errbuf_len, "out of memory");
        return NULL;
    }
    /* No REG_NOSUB: we want regexec to report the match offset (pmatch). */
    int rc = regcomp(&re->compiled, pattern, REG_EXTENDED);
    if (rc != 0) {
        if (errbuf && errbuf_len > 0)
            regerror(rc, &re->compiled, errbuf, errbuf_len);
        regfree(&re->compiled);
        free(re);
        return NULL;
    }

    return re;
}

int sm_regex_exec(sm_regex_t *re, const char *str, size_t len, size_t *match_off)
{
    /*
     * POSIX ERE has no built-in backtracking limit. The static pattern
     * validator (check_redos_risk) catches most dangerous patterns at
     * compile time. Additionally, we limit the input length to prevent
     * polynomial-time blowup on large inputs.
     *
     * Callers NUL-terminate at str[len]; regexec scans until that byte.
     */
    size_t base = 0;
    if (len > 65536) {
        base = len - 65536;
        str = str + base;
    }
    regmatch_t m[1];
    int rc = regexec(&re->compiled, str, 1, m, 0);
    if (rc != 0)
        return rc;
    if (match_off)
        *match_off = base + (size_t)m[0].rm_so;
    return 0;
}

void sm_regex_free(sm_regex_t *re)
{
    if (re) {
        regfree(&re->compiled);
        free(re);
    }
}

const char *sm_regex_backend(void)
{
    return "posix";
}

#endif
