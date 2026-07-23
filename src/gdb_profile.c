#include "gdb_profile.h"
#include "logger.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "gdb_profile"

/* Generic ARM Cortex-M defaults for an empty/minimal profile. */
static const char *const DEFAULT_REGISTERS[] = {
    "r0", "r1", "r2", "r3", "r12", "sp", "lr", "pc", "xpsr",
};

static const sm_gdb_fault_reg_t DEFAULT_FAULT_REGS[] = {
    { "CFSR",  "0xE000ED28" },
    { "HFSR",  "0xE000ED2C" },
    { "MMFAR", "0xE000ED34" },
    { "BFAR",  "0xE000ED38" },
};

void sm_gdb_profile_init_default(sm_gdb_profile_t *p)
{
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "generic");
    snprintf(p->description, sizeof(p->description),
             "Generic ARM Cortex-M target");
    snprintf(p->arch, sizeof(p->arch), "arm");
    snprintf(p->gdb_path, sizeof(p->gdb_path), "arm-none-eabi-gdb");

    for (size_t i = 0; i < sizeof(DEFAULT_REGISTERS) / sizeof(DEFAULT_REGISTERS[0]); i++)
        snprintf(p->important_registers[p->register_count++],
                 SM_GDB_REG_NAME_LEN, "%s", DEFAULT_REGISTERS[i]);

    for (size_t i = 0; i < sizeof(DEFAULT_FAULT_REGS) / sizeof(DEFAULT_FAULT_REGS[0]); i++)
        p->fault_registers[p->fault_register_count++] = DEFAULT_FAULT_REGS[i];
}

const char *sm_gdb_profile_peripheral_addr(const sm_gdb_profile_t *p,
                                           const char *name)
{
    for (size_t i = 0; i < p->peripheral_count; i++)
        if (strcmp(p->peripherals[i].name, name) == 0)
            return p->peripherals[i].address;
    return NULL;
}

/* Fill a fixed-size [count][width] string array from a JSON array of strings.
 * Resets *count to 0 first (the key's presence replaces the defaults). */
static void parse_string_array(cJSON *arr, char (*dst)[SM_GDB_CMD_LEN],
                               size_t *count, size_t cap)
{
    *count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        if (!cJSON_IsString(item)) continue;
        if (*count >= cap) break;
        snprintf(dst[*count], SM_GDB_CMD_LEN, "%s", item->valuestring);
        (*count)++;
    }
}

int sm_gdb_profile_from_json(sm_gdb_profile_t *p, const char *json_str)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        SM_LOG_WARN(LOG_TAG, "profile JSON parse failed");
        return -1;
    }

    cJSON *item;

    item = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (cJSON_IsString(item))
        snprintf(p->name, sizeof(p->name), "%s", item->valuestring);

    item = cJSON_GetObjectItemCaseSensitive(root, "description");
    if (cJSON_IsString(item))
        snprintf(p->description, sizeof(p->description), "%s", item->valuestring);

    item = cJSON_GetObjectItemCaseSensitive(root, "arch");
    if (cJSON_IsString(item))
        snprintf(p->arch, sizeof(p->arch), "%s", item->valuestring);

    item = cJSON_GetObjectItemCaseSensitive(root, "gdb_path");
    if (cJSON_IsString(item))
        snprintf(p->gdb_path, sizeof(p->gdb_path), "%s", item->valuestring);

    item = cJSON_GetObjectItemCaseSensitive(root, "rtos");
    if (cJSON_IsString(item))
        snprintf(p->rtos, sizeof(p->rtos), "%s", item->valuestring);

    item = cJSON_GetObjectItemCaseSensitive(root, "gdb_init_commands");
    if (cJSON_IsArray(item))
        parse_string_array(item, p->init_commands, &p->init_command_count,
                            SM_GDB_PROFILE_MAX_INIT_CMDS);

    item = cJSON_GetObjectItemCaseSensitive(root, "rtos_commands");
    if (cJSON_IsArray(item))
        parse_string_array(item, p->rtos_commands, &p->rtos_command_count,
                            SM_GDB_PROFILE_MAX_RTOS_CMDS);

    /* important_registers: array of short register-name strings. Its own width
     * differs from SM_GDB_CMD_LEN, so it is filled directly rather than via
     * parse_string_array. */
    item = cJSON_GetObjectItemCaseSensitive(root, "important_registers");
    if (cJSON_IsArray(item)) {
        p->register_count = 0;
        cJSON *reg;
        cJSON_ArrayForEach(reg, item) {
            if (!cJSON_IsString(reg)) continue;
            if (p->register_count >= SM_GDB_PROFILE_MAX_REGISTERS) break;
            snprintf(p->important_registers[p->register_count++],
                     SM_GDB_REG_NAME_LEN, "%s", reg->valuestring);
        }
    }

    /* fault_registers: array of {name, address}. */
    item = cJSON_GetObjectItemCaseSensitive(root, "fault_registers");
    if (cJSON_IsArray(item)) {
        p->fault_register_count = 0;
        cJSON *fr;
        cJSON_ArrayForEach(fr, item) {
            if (!cJSON_IsObject(fr)) continue;
            if (p->fault_register_count >= SM_GDB_PROFILE_MAX_FAULT_REGS) break;
            cJSON *name = cJSON_GetObjectItemCaseSensitive(fr, "name");
            cJSON *addr = cJSON_GetObjectItemCaseSensitive(fr, "address");
            if (!cJSON_IsString(name) || !cJSON_IsString(addr)) continue;
            sm_gdb_fault_reg_t *slot = &p->fault_registers[p->fault_register_count++];
            snprintf(slot->name, sizeof(slot->name), "%s", name->valuestring);
            snprintf(slot->address, sizeof(slot->address), "%s", addr->valuestring);
        }
    }

    /* peripheral_map: object of NAME -> "0xADDRESS". */
    item = cJSON_GetObjectItemCaseSensitive(root, "peripheral_map");
    if (cJSON_IsObject(item)) {
        p->peripheral_count = 0;
        cJSON *periph;
        cJSON_ArrayForEach(periph, item) {
            if (!cJSON_IsString(periph) || !periph->string) continue;
            if (p->peripheral_count >= SM_GDB_PROFILE_MAX_PERIPHERALS) break;
            sm_gdb_peripheral_t *slot = &p->peripherals[p->peripheral_count++];
            snprintf(slot->name, sizeof(slot->name), "%s", periph->string);
            snprintf(slot->address, sizeof(slot->address), "%s", periph->valuestring);
        }
    }

    cJSON_Delete(root);
    return 0;
}

int sm_gdb_profile_load(sm_gdb_profile_t *p, const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        SM_LOG_WARN(LOG_TAG, "cannot open profile %s", path);
        return -1;
    }

    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return -1; }
    long size = ftell(fp);
    if (size < 0 || size > 1024 * 1024) { fclose(fp); return -1; }
    rewind(fp);

    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(fp); return -1; }
    size_t rd = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[rd] = '\0';

    int rc = sm_gdb_profile_from_json(p, buf);
    free(buf);
    if (rc == 0)
        SM_LOG_INFO(LOG_TAG, "loaded target profile: %s (%s)", p->name, path);
    return rc;
}

cJSON *sm_gdb_profile_to_json(const sm_gdb_profile_t *p)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddStringToObject(root, "name", p->name);
    cJSON_AddStringToObject(root, "description", p->description);
    cJSON_AddStringToObject(root, "arch", p->arch);
    cJSON_AddStringToObject(root, "gdb_path", p->gdb_path);
    cJSON_AddStringToObject(root, "rtos", p->rtos);

    cJSON *init = cJSON_AddArrayToObject(root, "gdb_init_commands");
    for (size_t i = 0; i < p->init_command_count; i++)
        cJSON_AddItemToArray(init, cJSON_CreateString(p->init_commands[i]));

    cJSON *regs = cJSON_AddArrayToObject(root, "important_registers");
    for (size_t i = 0; i < p->register_count; i++)
        cJSON_AddItemToArray(regs, cJSON_CreateString(p->important_registers[i]));

    cJSON *fr = cJSON_AddArrayToObject(root, "fault_registers");
    for (size_t i = 0; i < p->fault_register_count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", p->fault_registers[i].name);
        cJSON_AddStringToObject(o, "address", p->fault_registers[i].address);
        cJSON_AddItemToArray(fr, o);
    }

    cJSON *pm = cJSON_AddObjectToObject(root, "peripheral_map");
    for (size_t i = 0; i < p->peripheral_count; i++)
        cJSON_AddStringToObject(pm, p->peripherals[i].name,
                                p->peripherals[i].address);

    cJSON *rc = cJSON_AddArrayToObject(root, "rtos_commands");
    for (size_t i = 0; i < p->rtos_command_count; i++)
        cJSON_AddItemToArray(rc, cJSON_CreateString(p->rtos_commands[i]));

    return root;
}
