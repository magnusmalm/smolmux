#ifndef SM_GDB_PROFILE_H
#define SM_GDB_PROFILE_H

#include <stddef.h>
#include "cJSON.h"

/* Target profile for device-specific GDB debugging. Provides the per-target
 * context the gdb-mcp tools need: which registers matter, where the ARM
 * Cortex-M fault status registers and named peripherals live, RTOS-awareness
 * commands, and gdb init commands. Loaded from JSON (*.gdb-profile.json).
 *
 * Bounded fixed arrays (no per-member allocation): the counts are small and
 * capped, so load never partially-allocates and there is nothing to free. */

#define SM_GDB_PROFILE_MAX_REGISTERS    64
#define SM_GDB_PROFILE_MAX_FAULT_REGS   16
#define SM_GDB_PROFILE_MAX_PERIPHERALS  64
#define SM_GDB_PROFILE_MAX_INIT_CMDS    16
#define SM_GDB_PROFILE_MAX_RTOS_CMDS    16

#define SM_GDB_REG_NAME_LEN   16
#define SM_GDB_ADDR_LEN       24
#define SM_GDB_PERIPH_NAME_LEN 32
#define SM_GDB_CMD_LEN        128

typedef struct sm_gdb_fault_reg {
    char name[SM_GDB_REG_NAME_LEN];  /* e.g. "CFSR" */
    char address[SM_GDB_ADDR_LEN];   /* e.g. "0xE000ED28" */
} sm_gdb_fault_reg_t;

typedef struct sm_gdb_peripheral {
    char name[SM_GDB_PERIPH_NAME_LEN]; /* e.g. "UARTE0" */
    char address[SM_GDB_ADDR_LEN];     /* e.g. "0x40008000" */
} sm_gdb_peripheral_t;

typedef struct sm_gdb_profile {
    char name[64];
    char description[256];
    char arch[16];
    char gdb_path[256];
    char rtos[32];  /* "zephyr", "freertos", or "" */

    char   init_commands[SM_GDB_PROFILE_MAX_INIT_CMDS][SM_GDB_CMD_LEN];
    size_t init_command_count;

    char   important_registers[SM_GDB_PROFILE_MAX_REGISTERS][SM_GDB_REG_NAME_LEN];
    size_t register_count;

    sm_gdb_fault_reg_t fault_registers[SM_GDB_PROFILE_MAX_FAULT_REGS];
    size_t             fault_register_count;

    sm_gdb_peripheral_t peripherals[SM_GDB_PROFILE_MAX_PERIPHERALS];
    size_t              peripheral_count;

    char   rtos_commands[SM_GDB_PROFILE_MAX_RTOS_CMDS][SM_GDB_CMD_LEN];
    size_t rtos_command_count;
} sm_gdb_profile_t;

/* Populate with the generic ARM Cortex-M defaults (arm-none-eabi-gdb, the
 * standard r0..pc/xpsr register set, and the SCB fault registers). */
void sm_gdb_profile_init_default(sm_gdb_profile_t *p);

/* Load a profile from a JSON file, starting from the defaults and overriding
 * only the keys present. Returns 0 on success, -1 on read/parse failure
 * (leaving *p at the defaults). */
int sm_gdb_profile_load(sm_gdb_profile_t *p, const char *path);

/* Parse a profile from a JSON string over the current *p (which the caller
 * should have init_default'd first). Returns 0 on success, -1 on parse error. */
int sm_gdb_profile_from_json(sm_gdb_profile_t *p, const char *json_str);

/* Serialize a profile to a cJSON object, using the same key names as
 * from_json so it round-trips. Caller owns the returned object (cJSON_Delete).
 * Returns NULL on allocation failure. */
cJSON *sm_gdb_profile_to_json(const sm_gdb_profile_t *p);

/* Look up a peripheral base address by name. Returns the address string, or
 * NULL if the peripheral is not in the profile. */
const char *sm_gdb_profile_peripheral_addr(const sm_gdb_profile_t *p,
                                           const char *name);

#endif /* SM_GDB_PROFILE_H */
