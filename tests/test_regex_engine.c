#include "test_main.h"
#include "regex_engine.h"
#include "constants.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static double mono_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static int backend_is_posix(void)
{
    return strcmp(sm_regex_backend(), "posix") == 0;
}

/* "Did not stall" bound for a single match. An order of magnitude below the
 * ~0.9s an unbounded pathological match costs on glibc, and ~30x above the
 * worst observed under PCRE2's match_limit, so it separates the two without
 * being flaky on a loaded machine. */
#define STALL_BOUND_S 0.25

/* Subject that forces maximal backtracking: N 'a's the pattern can chew
 * through, then a byte that defeats the anchor. Caller frees. */
static char *pathological_subject(size_t n)
{
    char *s = malloc(n + 2);
    if (!s) return NULL;
    memset(s, 'a', n);
    s[n] = 'b';
    s[n + 1] = '\0';
    return s;
}

static void test_basic_match(void)
{
    sm_regex_t *re = sm_regex_compile("hello", NULL, 0);
    ASSERT_NOT_NULL(re);
    ASSERT_INT_EQ(sm_regex_exec(re, "hello world", 11, NULL), 0);
    sm_regex_free(re);
}

static void test_no_match(void)
{
    sm_regex_t *re = sm_regex_compile("hello", NULL, 0);
    ASSERT_NOT_NULL(re);
    ASSERT(sm_regex_exec(re, "goodbye", 7, NULL) != 0, "no match");
    sm_regex_free(re);
}

static void test_anchored_prompt(void)
{
    sm_regex_t *re = sm_regex_compile("\\$ *$", NULL, 0);
    ASSERT_NOT_NULL(re);
    ASSERT_INT_EQ(sm_regex_exec(re, "user@host:~$ ", 13, NULL), 0);
    ASSERT(sm_regex_exec(re, "no prompt here", 14, NULL) != 0, "no match");
    sm_regex_free(re);
}

static void test_invalid_pattern(void)
{
    char errbuf[128] = {0};
    sm_regex_t *re = sm_regex_compile("[invalid", errbuf, sizeof(errbuf));
    ASSERT_NULL(re);
    ASSERT(strlen(errbuf) > 0, "error message set");
}

static void test_backend_name(void)
{
    const char *name = sm_regex_backend();
    ASSERT_NOT_NULL(name);
    /* Should be either "posix" or "pcre2" */
    ASSERT(strcmp(name, "posix") == 0 || strcmp(name, "pcre2") == 0,
           "backend is posix or pcre2");
}

static void test_regex_alternation(void)
{
    sm_regex_t *re = sm_regex_compile("alpha|beta", NULL, 0);
    ASSERT_NOT_NULL(re);
    ASSERT_INT_EQ(sm_regex_exec(re, "has alpha here", 14, NULL), 0);
    ASSERT_INT_EQ(sm_regex_exec(re, "has beta here", 13, NULL), 0);
    ASSERT(sm_regex_exec(re, "has gamma here", 14, NULL) != 0, "no match");
    sm_regex_free(re);
}

static void test_free_null(void)
{
    sm_regex_free(NULL);  /* should not crash */
    ASSERT(1, "free NULL ok");
}

/* Classic catastrophic-backtracking shapes must never be able to stall the
 * broker's event loop. Each backend delivers that a different way, so assert
 * the property and the mechanism actually in play:
 *
 *   POSIX ERE - rejected at compile time. check_redos_risk() kept group risk
 *               in one pair of flags that every '(' reset, so an extra layer
 *               of parens laundered it: (a+)+ was caught but (x(a+))+ and
 *               ((a|a))* were accepted, and ((a|a))*$ then cost 0.41s against
 *               8 KiB. Risk now folds into the parent at every closing paren.
 *   PCRE2     - accepted, then bounded at match time by match_limit.
 */
static void test_redos_shapes_cannot_stall(void)
{
    static const char *const shapes[] = {
        "(a+)+$",           /* the shape that was already caught */
        "(x(a+))+$",        /* nested quantifier hidden one level deeper */
        "((a|a))*$",        /* quantified alternation hidden one level */
        "(((a|a)))*$",      /* ... and two levels */
        "((((a+))))+$",     /* ... and three */
    };

    size_t n = 8191;
    char *subject = pathological_subject(n);
    ASSERT_NOT_NULL(subject);
    if (!subject) return;

    for (size_t i = 0; i < sizeof(shapes) / sizeof(shapes[0]); i++) {
        char err[128] = "";
        sm_regex_t *re = sm_regex_compile(shapes[i], err, sizeof(err));

        if (backend_is_posix()) {
            ASSERT(re == NULL, shapes[i]);
            if (re) sm_regex_free(re);
            continue;
        }

        ASSERT(re != NULL, shapes[i]);
        if (!re) continue;
        double t0 = mono_s();
        sm_regex_exec(re, subject, n + 1, NULL);
        ASSERT(mono_s() - t0 < STALL_BOUND_S, shapes[i]);
        sm_regex_free(re);
    }

    free(subject);
}

/* The tightened validator must not start rejecting patterns smolmux itself
 * ships: builtin anomaly rules, profile patterns and boot-stage markers. A
 * false positive here silently stops anomaly detection. */
static void test_redos_shipped_patterns_still_compile(void)
{
    static const char *const must_accept[] = {
        "watchdog.*reset|wdt.*timeout",
        "Assertion.*failed|BUG_ON|BUG:",
        "rst:0x[0-9a-fA-F]+",
        "FATAL EXCEPTION|E: \\*\\*\\* Booting|E: r[0-9]+/",
        "MPU FAULT|MemManage|BusFault|UsageFault|SecureFault|HardFault",
        "rst:0x[0-9a-fA-F]+ \\(RTCWDT|TG[0-9]WDT|SW_CPU|PANIC\\)",
        "^[#$][[:space:]]*$",
        SM_PROFILE_DEFAULT_PROMPT,
        "INFO: task .* blocked for more than",
        "(foo)+",                        /* quantified group, no inner risk */
        "(abc|def)",                     /* alternation, not quantified */
    };

    for (size_t i = 0; i < sizeof(must_accept) / sizeof(must_accept[0]); i++) {
        char err[128] = "";
        sm_regex_t *re = sm_regex_compile(must_accept[i], err, sizeof(err));
        ASSERT(re != NULL, must_accept[i]);
        if (re) sm_regex_free(re);
    }
}

/* Default send --expect must match common prompts on POSIX ERE *and* PCRE2. */
static void test_default_prompt_matches_shells(void)
{
    char err[128] = "";
    sm_regex_t *re = sm_regex_compile(SM_PROFILE_DEFAULT_PROMPT, err, sizeof(err));
    ASSERT(re != NULL, "default prompt compiles");
    if (!re) return;
    const char *ok[] = { "root@host# ", "user$ ", "Versal> ", "=> " };
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        size_t off = 0;
        ASSERT_INT_EQ(sm_regex_exec(re, ok[i], strlen(ok[i]), &off), 0);
    }
    sm_regex_free(re);
}

/* A static validator cannot be complete, and this pattern proves it: it is
 * neither a nested quantifier nor a quantified alternation, so check_redos_risk
 * accepts it, yet it costs ~0.9s against 8 KiB on glibc — enough to stall the
 * broker's single-threaded loop on every chunk of device output. It is the
 * reason the engine cannot rely on the validator alone.
 *
 * POSIX: the measured budget is the backstop — one over-budget match disables
 * the pattern for good, so the cost is paid once instead of per chunk.
 * PCRE2:  match_limit bounds it up front, so nothing needs disabling. */
static void test_slow_pattern_is_bounded(void)
{
    const char *pattern = "(a*)(a*)(a*)(a*)(a*)(a*)(a*)(a*)$";
    char err[128] = "";
    sm_regex_t *re = sm_regex_compile(pattern, err, sizeof(err));
    ASSERT(re != NULL, "the static validator does not model this shape");
    if (!re) return;

    size_t n = 8191;
    char *matching = malloc(n + 1);   /* all 'a' — matches, and cheaply */
    char *pathological = pathological_subject(n);
    ASSERT_NOT_NULL(matching);
    ASSERT_NOT_NULL(pathological);
    if (!matching || !pathological) { free(matching); free(pathological);
                                      sm_regex_free(re); return; }
    memset(matching, 'a', n);
    matching[n] = '\0';

    /* Baseline: the pattern works before anything trips. */
    ASSERT_INT_EQ(sm_regex_exec(re, matching, n, NULL), 0);

    double t0 = mono_s();
    sm_regex_exec(re, pathological, n + 1, NULL);
    double pathological_s = mono_s() - t0;

    t0 = mono_s();
    int rc = sm_regex_exec(re, matching, n, NULL);
    double after_s = mono_s() - t0;

    if (backend_is_posix()) {
        /* The first match is allowed to be slow — that is how the budget
         * notices. glibc ERE typically blows the 50ms cap on this shape;
         * musl often finishes inside the budget. Disable only when the
         * budget actually fired. */
        if (pathological_s > (double)SM_REGEX_MAX_MATCH_MS / 1000.0) {
            ASSERT(rc != 0, "over-budget pattern is disabled");
            ASSERT(after_s < 0.010, "disabled pattern returns immediately");
        } else {
            ASSERT(pathological_s < STALL_BOUND_S,
                   "in-budget pathological match stayed bounded");
            ASSERT_INT_EQ(rc, 0);
        }
    } else {
        /* match_limit caps the work up front, so the pattern stays usable. */
        ASSERT(pathological_s < STALL_BOUND_S, "match_limit bounds the match");
        ASSERT_INT_EQ(rc, 0);
    }

    free(matching);
    free(pathological);
    sm_regex_free(re);
}

int main(void)
{
    printf("test_regex_engine\n");

    RUN_TEST(test_basic_match);
    RUN_TEST(test_no_match);
    RUN_TEST(test_anchored_prompt);
    RUN_TEST(test_invalid_pattern);
    RUN_TEST(test_backend_name);
    RUN_TEST(test_regex_alternation);
    RUN_TEST(test_free_null);
    RUN_TEST(test_redos_shapes_cannot_stall);
    RUN_TEST(test_redos_shipped_patterns_still_compile);
    RUN_TEST(test_default_prompt_matches_shells);
    RUN_TEST(test_slow_pattern_is_bounded);

    TEST_REPORT();
}
