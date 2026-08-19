#ifndef SM_DEVICE_PROFILE_H
#define SM_DEVICE_PROFILE_H

#include <stddef.h>
#include "anomaly.h"
#include "boot_stage.h"

#define SM_PROFILE_MAX_COMMANDS 64
#define SM_PROFILE_MAX_ANOMALY_PATTERNS 64
#define SM_PROFILE_MAX_BOOT_STAGES 32

typedef struct sm_profile_command {
    char cmd[256];
    char description[256];
} sm_profile_command_t;

typedef struct sm_profile_anomaly {
    char name[64];
    char pattern[256];
    char severity[16];
} sm_profile_anomaly_t;

typedef struct sm_profile_boot_stage {
    char name[64];
    char pattern[256];
} sm_profile_boot_stage_t;

typedef struct sm_device_profile {
    char name[64];
    char device_type[64];
    char description[256];
    char prompt_pattern[256];
    char command_prefix[64];
    char response_mode[16];
    int default_timeout_ms;
    char boot_banner[256];

    sm_profile_command_t *commands;   /* dynamically allocated */
    size_t command_count;
    size_t command_cap;

    sm_profile_anomaly_t *anomaly_patterns;  /* dynamically allocated */
    size_t anomaly_count;
    size_t anomaly_cap;

    sm_profile_boot_stage_t *boot_stages;    /* dynamically allocated */
    size_t boot_stage_count;
    size_t boot_stage_cap;
    int boot_stall_timeout_ms;               /* 0 = use tracker default */
} sm_device_profile_t;

void sm_profile_init_default(sm_device_profile_t *p);
int  sm_profile_load(sm_device_profile_t *p, const char *path);
int  sm_profile_from_json(sm_device_profile_t *p, const char *json_str);
void sm_profile_apply_anomaly(const sm_device_profile_t *p, sm_anomaly_detector_t *det);
void sm_profile_apply_boot(const sm_device_profile_t *p, sm_boot_tracker_t *t);
void sm_profile_destroy(sm_device_profile_t *p);

#endif /* SM_DEVICE_PROFILE_H */
