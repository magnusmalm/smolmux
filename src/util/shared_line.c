#include "util/shared_line.h"

#include <stdlib.h>

int sm_shared_line_test_fail_next;

void sm_shared_line_test_reset_hooks(void)
{
    sm_shared_line_test_fail_next = 0;
}

sm_shared_line_t *sm_shared_line_new(char *data, size_t len)
{
    if (sm_shared_line_test_fail_next) {
        sm_shared_line_test_fail_next = 0;
        free(data);
        return NULL;
    }

    if (!data || len == 0) {
        free(data);
        return NULL;
    }
    sm_shared_line_t *sl = malloc(sizeof(*sl));
    if (!sl) {
        free(data);
        return NULL;
    }
    sl->data = data;
    sl->len = len;
    sl->refs = 1;
    return sl;
}

void sm_shared_line_acquire(sm_shared_line_t *sl)
{
    if (sl)
        sl->refs++;
}

void sm_shared_line_release(sm_shared_line_t *sl)
{
    if (!sl) return;
    if (--sl->refs <= 0) {
        free(sl->data);
        free(sl);
    }
}