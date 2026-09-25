#ifndef SM_GDB_H
#define SM_GDB_H

#include "links/link.h"

#include <stddef.h>
#include <stdint.h>

sm_link_t *sm_gdb_new(const char *gdb_path, const char *target_spec);

/* Guard-rail (SM-06), not a jail. Exposed so tests can feed an embedded
 * newline; contains_shell_command splits on \n and would hide a hang. */
int sm_gdb_line_invokes_shell(const uint8_t *line, size_t len);

#endif /* SM_GDB_H */
