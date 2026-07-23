#ifndef SM_GDB_H
#define SM_GDB_H

#include "links/link.h"

sm_link_t *sm_gdb_new(const char *gdb_path, const char *target_spec);

#endif /* SM_GDB_H */
