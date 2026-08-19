#ifndef SC_TEST_MAIN_H
#define SC_TEST_MAIN_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int _test_pass = 0;
static int _test_fail = 0;

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__, msg); \
        _test_fail++; \
    } else { \
        _test_pass++; \
    } \
} while(0)

#define ASSERT_STR_EQ(a, b) do { \
    const char *_a = (a); const char *_b = (b); \
    if (_a == NULL && _b == NULL) { _test_pass++; } \
    else if (_a == NULL || _b == NULL || strcmp(_a, _b) != 0) { \
        fprintf(stderr, "  FAIL: %s:%d: \"%s\" != \"%s\"\n", \
                __FILE__, __LINE__, _a ? _a : "(null)", _b ? _b : "(null)"); \
        _test_fail++; \
    } else { _test_pass++; } \
} while(0)

#define ASSERT_INT_EQ(a, b) do { \
    int _a = (a); int _b = (b); \
    if (_a != _b) { \
        fprintf(stderr, "  FAIL: %s:%d: %d != %d\n", __FILE__, __LINE__, _a, _b); \
        _test_fail++; \
    } else { _test_pass++; } \
} while(0)

#define ASSERT_NOT_NULL(p) do { \
    if ((p) == NULL) { \
        fprintf(stderr, "  FAIL: %s:%d: unexpected NULL\n", __FILE__, __LINE__); \
        _test_fail++; \
    } else { _test_pass++; } \
} while(0)

#define ASSERT_NULL(p) do { \
    if ((p) != NULL) { \
        fprintf(stderr, "  FAIL: %s:%d: expected NULL\n", __FILE__, __LINE__); \
        _test_fail++; \
    } else { _test_pass++; } \
} while(0)

#define RUN_TEST(fn) do { \
    printf("  %s ...\n", #fn); \
    fn(); \
} while(0)

#define TEST_REPORT() do { \
    printf("\n%d passed, %d failed\n", _test_pass, _test_fail); \
    return _test_fail > 0 ? 1 : 0; \
} while(0)

#endif /* SC_TEST_MAIN_H */
