#ifndef SM_SHARED_LINE_H
#define SM_SHARED_LINE_H

#include <stddef.h>

typedef struct sm_shared_line {
    char  *data;
    size_t len;
    int    refs;
} sm_shared_line_t;

/* Takes ownership of data. Initial refs=1. */
sm_shared_line_t *sm_shared_line_new(char *data, size_t len);

/* Increment refcount. */
void sm_shared_line_acquire(sm_shared_line_t *sl);

/* Decrement refcount; frees data when refs hit 0. */
void sm_shared_line_release(sm_shared_line_t *sl);

/* Test hooks (reset before/after tests to avoid cross-test pollution). */
extern int sm_shared_line_test_fail_next;
void sm_shared_line_test_reset_hooks(void);

#endif /* SM_SHARED_LINE_H */