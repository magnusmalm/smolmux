#include "test_main.h"
#include "device_profile.h"
#include "constants.h"
#include "util/profile_resolve.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdlib.h>
/* access() for shipped profile path probe */

static void test_defaults(void)
{
    sm_device_profile_t p;
    sm_profile_init_default(&p);

    ASSERT_STR_EQ(p.name, "generic");
    ASSERT_STR_EQ(p.device_type, "unknown");
    ASSERT_STR_EQ(p.response_mode, "prompt");
    ASSERT_INT_EQ(p.default_timeout_ms, SM_PROFILE_DEFAULT_TIMEOUT);
    ASSERT_INT_EQ((int)p.command_count, 0);
    ASSERT_INT_EQ((int)p.anomaly_count, 0);
    ASSERT(strlen(p.prompt_pattern) > 0, "prompt pattern set");
}

static void test_from_json_basic(void)
{
    sm_device_profile_t p;
    const char *json =
        "{"
        "  \"name\": \"test-device\","
        "  \"device_type\": \"linux\","
        "  \"description\": \"Test device\","
        "  \"prompt_pattern\": \"\\\\$ $\","
        "  \"default_timeout_ms\": 3000,"
        "  \"boot_banner\": \"login:\","
        "  \"commands\": [\"uname -a\", \"uptime\"],"
        "  \"custom_anomaly_patterns\": ["
        "    {\"name\": \"my_err\", \"pattern\": \"ERROR_[0-9]+\", \"severity\": \"warning\"}"
        "  ]"
        "}";

    int rc = sm_profile_from_json(&p, json);
    ASSERT_INT_EQ(rc, 0);
    ASSERT_STR_EQ(p.name, "test-device");
    ASSERT_STR_EQ(p.device_type, "linux");
    ASSERT_INT_EQ(p.default_timeout_ms, 3000);
    ASSERT_STR_EQ(p.boot_banner, "login:");
    ASSERT_INT_EQ((int)p.command_count, 2);
    ASSERT_STR_EQ(p.commands[0].cmd, "uname -a");
    ASSERT_STR_EQ(p.commands[1].cmd, "uptime");
    ASSERT_INT_EQ((int)p.anomaly_count, 1);
    ASSERT_STR_EQ(p.anomaly_patterns[0].name, "my_err");
    ASSERT_STR_EQ(p.anomaly_patterns[0].pattern, "ERROR_[0-9]+");
    ASSERT_STR_EQ(p.anomaly_patterns[0].severity, "warning");
}

static void test_from_json_object_commands(void)
{
    sm_device_profile_t p;
    const char *json =
        "{"
        "  \"name\": \"pump\","
        "  \"command_prefix\": \"\\r\","
        "  \"response_mode\": \"timeout\","
        "  \"commands\": ["
        "    {\"cmd\": \"help\", \"description\": \"Show help\"},"
        "    {\"cmd\": \"status\", \"description\": \"Show status\"}"
        "  ]"
        "}";

    int rc = sm_profile_from_json(&p, json);
    ASSERT_INT_EQ(rc, 0);
    ASSERT_STR_EQ(p.name, "pump");
    /* JSON "\r" is parsed as carriage return by cJSON */
    ASSERT_STR_EQ(p.command_prefix, "\r");
    ASSERT_STR_EQ(p.response_mode, "timeout");
    ASSERT_INT_EQ((int)p.command_count, 2);
    ASSERT_STR_EQ(p.commands[0].cmd, "help");
    ASSERT_STR_EQ(p.commands[0].description, "Show help");
}

static void test_apply_anomaly(void)
{
    sm_device_profile_t p;
    const char *json =
        "{"
        "  \"custom_anomaly_patterns\": ["
        "    {\"name\": \"test_err\", \"pattern\": \"TEST_FAIL\", \"severity\": \"critical\"}"
        "  ]"
        "}";

    sm_profile_from_json(&p, json);

    sm_anomaly_detector_t det;
    sm_anomaly_init(&det);

    sm_profile_apply_anomaly(&p, &det);
    ASSERT(det.pattern_count >= 1, "pattern added");

    /* Feed data that matches the pattern */
    size_t n = sm_anomaly_feed(&det, (const uint8_t *)"got TEST_FAIL here", 18, 1000.0);
    ASSERT(n >= 1, "anomaly detected");

    sm_anomaly_destroy(&det);
    sm_profile_destroy(&p);
}

static void test_apply_boot(void)
{
    sm_device_profile_t p;
    const char *json =
        "{"
        "  \"boot_stall_timeout_ms\": 5000,"
        "  \"boot_stages\": ["
        "    {\"name\": \"uboot\", \"pattern\": \"U-Boot 20\"},"
        "    {\"name\": \"login\", \"pattern\": \"login:\"}"
        "  ]"
        "}";

    sm_profile_from_json(&p, json);
    ASSERT_INT_EQ((int)p.boot_stage_count, 2);
    ASSERT_INT_EQ(p.boot_stall_timeout_ms, 5000);
    ASSERT_STR_EQ(p.boot_stages[0].name, "uboot");

    sm_boot_tracker_t t;
    sm_boot_init(&t);
    sm_profile_apply_boot(&p, &t);
    ASSERT_INT_EQ((int)t.stage_count, 2);

    sm_boot_feed(&t, (const uint8_t *)"U-Boot 2024.01\n", 15, 1.0);
    ASSERT_INT_EQ(t.furthest, 0);
    sm_boot_feed(&t, (const uint8_t *)"board login:", 12, 2.0);
    ASSERT(sm_boot_terminal_reached(&t), "reached login via applied profile");

    sm_boot_destroy(&t);
    sm_profile_destroy(&p);
}

static void test_invalid_json(void)
{
    sm_device_profile_t p;
    int rc = sm_profile_from_json(&p, "not valid json");
    ASSERT(rc != 0, "invalid JSON rejected");
}

/* T4: Load a profile from an actual file on disk */
static void test_load_from_file(void)
{
    const char *tmp = getenv("TMPDIR");
    char path[512];
    snprintf(path, sizeof(path), "%s/smolmux-test-profile.json",
             tmp && tmp[0] ? tmp : "/tmp");
    const char *json =
        "{\n"
        "  \"name\": \"gdb-stub\",\n"
        "  \"device_type\": \"gdb\",\n"
        "  \"description\": \"GDB remote stub\",\n"
        "  \"prompt_pattern\": \"\\\\(gdb\\\\)\",\n"
        "  \"default_timeout_ms\": 10000,\n"
        "  \"commands\": [\n"
        "    {\"cmd\": \"info registers\", \"description\": \"Show registers\"},\n"
        "    {\"cmd\": \"backtrace\", \"description\": \"Show stack trace\"},\n"
        "    \"continue\"\n"
        "  ],\n"
        "  \"custom_anomaly_patterns\": [\n"
        "    {\"name\": \"segfault\", \"pattern\": \"SIGSEGV\", \"severity\": \"critical\"},\n"
        "    {\"name\": \"abort\", \"pattern\": \"SIGABRT\"}\n"
        "  ]\n"
        "}";

    /* Write test file */
    FILE *f = fopen(path, "w");
    ASSERT(f != NULL, "created temp profile file");
    if (!f) return;  /* nothing else is meaningful without the file */
    fputs(json, f);
    fclose(f);

    sm_device_profile_t p;
    int rc = sm_profile_load(&p, path);
    ASSERT_INT_EQ(rc, 0);

    ASSERT_STR_EQ(p.name, "gdb-stub");
    ASSERT_STR_EQ(p.device_type, "gdb");
    ASSERT_STR_EQ(p.description, "GDB remote stub");
    ASSERT_INT_EQ(p.default_timeout_ms, 10000);

    /* Verify commands parsed (mix of string and object entries) */
    ASSERT_INT_EQ((int)p.command_count, 3);
    ASSERT_STR_EQ(p.commands[0].cmd, "info registers");
    ASSERT_STR_EQ(p.commands[0].description, "Show registers");
    ASSERT_STR_EQ(p.commands[2].cmd, "continue");

    /* Verify anomaly patterns */
    ASSERT_INT_EQ((int)p.anomaly_count, 2);
    ASSERT_STR_EQ(p.anomaly_patterns[0].name, "segfault");
    ASSERT_STR_EQ(p.anomaly_patterns[0].severity, "critical");
    /* Second pattern has no explicit severity — should default to "warning" */
    ASSERT_STR_EQ(p.anomaly_patterns[1].name, "abort");
    ASSERT_STR_EQ(p.anomaly_patterns[1].severity, "warning");

    sm_profile_destroy(&p);
    unlink(path);
}

/* T4 bonus: loading non-existent file should fail */
static void test_load_missing_file(void)
{
    const char *tmp = getenv("TMPDIR");
    char path[512];
    snprintf(path, sizeof(path), "%s/smolmux-nonexistent-profile.json",
             tmp && tmp[0] ? tmp : "/tmp");
    sm_device_profile_t p;
    int rc = sm_profile_load(&p, path);
    ASSERT(rc != 0, "missing file rejected");
}

/* Short-name resolve: prefer path, then config dir by stem; missing => -1 */
static void test_profile_resolve_short_name(void)
{
    const char *base = getenv("TMPDIR");
    if (!base || !base[0]) base = "/tmp";
    char dir[512], path_a[576], path_b[576], home_saved[512];
    snprintf(dir, sizeof(dir), "%s/smolmux-resolve-test-%d", base, (int)getpid());
    snprintf(path_a, sizeof(path_a), "%s/uboot%s", dir, SM_PROFILE_FILE_SUFFIX);
    snprintf(path_b, sizeof(path_b), "%s/esp-idf-uart%s", dir, SM_PROFILE_FILE_SUFFIX);

    ASSERT(mkdir(dir, 0700) == 0 || errno == EEXIST, "mkdir test config dir");

    const char *json_u =
        "{\"name\":\"uboot\",\"device_type\":\"bootloader\",\"description\":\"u\"}";
    const char *json_e =
        "{\"name\":\"esp-idf-uart\",\"device_type\":\"mcu\",\"description\":\"e\"}";
    FILE *f = fopen(path_a, "w");
    ASSERT(f != NULL, "write uboot profile");
    if (f) { fputs(json_u, f); fclose(f); }
    f = fopen(path_b, "w");
    ASSERT(f != NULL, "write esp profile");
    if (f) { fputs(json_e, f); fclose(f); }

    /* Point HOME at a tree with .config/smolmux */
    char fake_home[512], conf[576], cfg_parent[576];
    snprintf(fake_home, sizeof(fake_home), "%s/home", dir);
    snprintf(cfg_parent, sizeof(cfg_parent), "%s/.config", fake_home);
    snprintf(conf, sizeof(conf), "%s/.config/smolmux", fake_home);
    mkdir(fake_home, 0700);
    mkdir(cfg_parent, 0700);
    mkdir(conf, 0700);

    char conf_u[640], conf_e[640];
    snprintf(conf_u, sizeof(conf_u), "%s/uboot%s", conf, SM_PROFILE_FILE_SUFFIX);
    snprintf(conf_e, sizeof(conf_e), "%s/esp-idf-uart%s", conf, SM_PROFILE_FILE_SUFFIX);
    f = fopen(conf_u, "w");
    if (f) { fputs(json_u, f); fclose(f); }
    f = fopen(conf_e, "w");
    if (f) { fputs(json_e, f); fclose(f); }

    const char *old_home = getenv("HOME");
    if (old_home)
        snprintf(home_saved, sizeof(home_saved), "%s", old_home);
    else
        home_saved[0] = '\0';
    setenv("HOME", fake_home, 1);

    char out[512];
    ASSERT_INT_EQ(sm_profile_resolve_path("uboot", SM_PROFILE_FILE_SUFFIX,
                                          out, sizeof(out)), 0);
    ASSERT(strstr(out, "uboot") != NULL, "resolved uboot path contains uboot");
    ASSERT(strstr(out, "esp-idf") == NULL, "did not pick esp first alphabetically");

    ASSERT_INT_EQ(sm_profile_resolve_path("nope-missing", SM_PROFILE_FILE_SUFFIX,
                                          out, sizeof(out)), -1);

    ASSERT_INT_EQ(sm_profile_resolve_path(conf_u, SM_PROFILE_FILE_SUFFIX,
                                          out, sizeof(out)), 0);
    ASSERT_STR_EQ(out, conf_u);

    if (home_saved[0])
        setenv("HOME", home_saved, 1);
    else
        unsetenv("HOME");

    unlink(conf_u);
    unlink(conf_e);
    unlink(path_a);
    unlink(path_b);
    rmdir(conf);
    rmdir(cfg_parent);
    rmdir(fake_home);
    rmdir(dir);
}

/* Ship config: empty-password guidance + shell stage + BusyBox-friendly cmds. */
static void test_load_shipped_linux_shell_profile(void)
{
    const char *candidates[] = {
        "configs/linux-shell.smolmux-profile.json",
        "../configs/linux-shell.smolmux-profile.json",
        NULL
    };
    const char *path = NULL;
    for (int i = 0; candidates[i]; i++) {
        if (access(candidates[i], R_OK) == 0) {
            path = candidates[i];
            break;
        }
    }
    ASSERT_NOT_NULL(path); /* run from build/ or repo root */

    sm_device_profile_t p;
    ASSERT_INT_EQ(sm_profile_load(&p, path), 0);
    ASSERT_STR_EQ(p.name, "linux-shell");
    ASSERT_STR_EQ(p.device_type, "linux");
    ASSERT(strstr(p.description, "Password:") != NULL,
           "description mentions Password: trap");
    ASSERT(strlen(p.description) < sizeof(p.description),
           "description fits field (no silent loss of NULs)");

    int have_shell = 0, have_login = 0;
    for (size_t i = 0; i < p.boot_stage_count; i++) {
        if (strcmp(p.boot_stages[i].name, "shell") == 0)
            have_shell = 1;
        if (strcmp(p.boot_stages[i].name, "login") == 0)
            have_login = 1;
    }
    ASSERT(have_login, "login boot stage present");
    ASSERT(have_shell, "shell boot stage present");

    int have_logread = 0, have_ps = 0;
    for (size_t i = 0; i < p.command_count; i++) {
        if (strstr(p.commands[i].cmd, "logread") != NULL)
            have_logread = 1;
        if (strcmp(p.commands[i].cmd, "ps") == 0)
            have_ps = 1;
    }
    ASSERT(have_logread, "logread command for BusyBox images");
    ASSERT(have_ps, "plain ps command present");

    sm_profile_destroy(&p);
}

int main(void)
{
    printf("test_device_profile\n");

    RUN_TEST(test_defaults);
    RUN_TEST(test_from_json_basic);
    RUN_TEST(test_from_json_object_commands);
    RUN_TEST(test_apply_anomaly);
    RUN_TEST(test_apply_boot);
    RUN_TEST(test_invalid_json);
    RUN_TEST(test_load_from_file);
    RUN_TEST(test_load_missing_file);
    RUN_TEST(test_profile_resolve_short_name);
    RUN_TEST(test_load_shipped_linux_shell_profile);

    TEST_REPORT();
}
