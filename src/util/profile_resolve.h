#ifndef SM_PROFILE_RESOLVE_H
#define SM_PROFILE_RESOLVE_H

#include <stddef.h>

/* Resolve a profile specifier to a readable file path.
 *
 * spec: absolute/relative path, or short name (e.g. "uboot", "nrf9151").
 * suffix: e.g. SM_PROFILE_FILE_SUFFIX or SM_GDB_PROFILE_FILE_SUFFIX.
 *
 * Resolution order:
 *   1. spec as path if it exists and is readable
 *   2. $HOME/.config/smolmux/<spec><suffix>
 *   3. ./profiles/<spec><suffix>
 *   4. ./configs/<spec><suffix>
 *   5. scan $HOME/.config/smolmux/ for basename stem == spec
 *
 * Returns 0 and writes the path into out on success, -1 on failure.
 * out is always NUL-terminated when out_len > 0 (empty string on failure).
 */
int sm_profile_resolve_path(const char *spec, const char *suffix,
                            char *out, size_t out_len);

#endif /* SM_PROFILE_RESOLVE_H */
