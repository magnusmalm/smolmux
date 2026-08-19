#include "util/profile_resolve.h"
#include "constants.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <glob.h>
#include <libgen.h>

static int path_readable(const char *path)
{
    return path && path[0] && access(path, R_OK) == 0;
}

static int copy_path(char *out, size_t out_len, const char *path)
{
    if (!out || out_len == 0) return -1;
    snprintf(out, out_len, "%s", path);
    return (out[0] != '\0') ? 0 : -1;
}

/* basename of path without directory; strip trailing suffix if present. */
static void stem_from_path(const char *path, const char *suffix,
                           char *stem, size_t stem_len)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    const char *base = basename(tmp);
    snprintf(stem, stem_len, "%s", base ? base : "");
    if (suffix && suffix[0]) {
        size_t sl = strlen(suffix);
        size_t bl = strlen(stem);
        if (bl > sl && strcmp(stem + bl - sl, suffix) == 0)
            stem[bl - sl] = '\0';
    }
}

static int try_candidate(const char *path, char *out, size_t out_len)
{
    if (!path_readable(path)) return -1;
    return copy_path(out, out_len, path);
}

int sm_profile_resolve_path(const char *spec, const char *suffix,
                            char *out, size_t out_len)
{
    if (out && out_len > 0)
        out[0] = '\0';
    if (!spec || !spec[0] || !suffix || !out || out_len == 0)
        return -1;

    /* 1. Direct path */
    if (try_candidate(spec, out, out_len) == 0)
        return 0;

    /* If it looks like a path (contains /) and failed, do not invent names. */
    if (strchr(spec, '/'))
        return -1;

    char candidate[512];

    /* 2. ~/.config/smolmux/<spec><suffix> */
    const char *home = getenv("HOME");
    if (home && home[0]) {
        snprintf(candidate, sizeof(candidate), SM_PROFILE_CONFIG_DIR_FMT "/%s%s",
                 home, spec, suffix);
        if (try_candidate(candidate, out, out_len) == 0)
            return 0;
    }

    /* 3. ./profiles/<spec><suffix> (Pro zip / checkout) */
    snprintf(candidate, sizeof(candidate), "profiles/%s%s", spec, suffix);
    if (try_candidate(candidate, out, out_len) == 0)
        return 0;

    /* 4. ./configs/<spec><suffix> (dev tree) */
    snprintf(candidate, sizeof(candidate), "configs/%s%s", spec, suffix);
    if (try_candidate(candidate, out, out_len) == 0)
        return 0;

    /* 5. Scan config dir: basename stem match */
    if (home && home[0]) {
        char pattern[512];
        snprintf(pattern, sizeof(pattern), SM_PROFILE_CONFIG_DIR_FMT "/*%s",
                 home, suffix);
        glob_t g;
        memset(&g, 0, sizeof(g));
        if (glob(pattern, 0, NULL, &g) == 0) {
            for (size_t i = 0; i < g.gl_pathc; i++) {
                char stem[256];
                stem_from_path(g.gl_pathv[i], suffix, stem, sizeof(stem));
                if (strcmp(stem, spec) == 0 &&
                    try_candidate(g.gl_pathv[i], out, out_len) == 0) {
                    globfree(&g);
                    return 0;
                }
            }
        }
        globfree(&g);
    }

    return -1;
}
