#ifndef TEST_FIND_SHIPPED_H
#define TEST_FIND_SHIPPED_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Locate a repo file (e.g. configs/foo.json) from CMAKE_SOURCE_DIR or
 * by walking up from cwd. Missing fixture: return -1, out empty. */
static int test_find_shipped(char *out, size_t n, const char *rel)
{
    if (!out || n < 2 || !rel) {
        if (out && n)
            out[0] = '\0';
        return -1;
    }
#ifdef SMOLMUX_SOURCE_DIR
    if ((size_t)snprintf(out, n, "%s/%s", SMOLMUX_SOURCE_DIR, rel) < n &&
        access(out, R_OK) == 0)
        return 0;
#endif
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) {
        out[0] = '\0';
        return -1;
    }
    char *p = cwd;
    for (;;) {
        if ((size_t)snprintf(out, n, "%s/%s", p, rel) < n &&
            access(out, R_OK) == 0)
            return 0;
        char *slash = strrchr(p, '/');
        if (!slash || slash == p)
            break;
        *slash = '\0';
    }
    out[0] = '\0';
    return -1;
}

#endif /* TEST_FIND_SHIPPED_H */
