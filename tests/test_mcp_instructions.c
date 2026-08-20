/* Unit tests for serial MCP instructions + prompts. Pure API — no broker. */
#include "test_main.h"
#include "mcp_instructions.h"
#include "constants.h"
#include "cJSON.h"

#include <string.h>

static void test_instructions_content(void)
{
    const char *ins = sm_mcp_serial_instructions();
    ASSERT_NOT_NULL(ins);
    size_t len = strlen(ins);
    ASSERT(len >= 200, "instructions min length >= 200");
    ASSERT(strstr(ins, "serial_suspend") != NULL,
           "instructions mention serial_suspend");
    ASSERT(strstr(ins, "serial_get_incidents") != NULL,
           "instructions mention serial_get_incidents");
    /* Negative: not just the server name */
    ASSERT(strcmp(ins, SM_NAME) != 0, "not server name alone");
    ASSERT(strcmp(ins, SM_NAME "-mcp") != 0, "not serverInfo.name alone");
}

static void test_prompt_names_exact_set(void)
{
    size_t count = 0;
    const char *const *names = sm_mcp_serial_prompt_names(&count);
    ASSERT_NOT_NULL(names);
    ASSERT_INT_EQ((int)count, SM_MCP_SERIAL_PROMPT_COUNT);
    ASSERT_INT_EQ((int)count, 3);

    int saw_bringup = 0, saw_debug = 0, saw_flash = 0;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(names[i], "bringup") == 0) saw_bringup = 1;
        else if (strcmp(names[i], "debug_serial") == 0) saw_debug = 1;
        else if (strcmp(names[i], "flash_safe") == 0) saw_flash = 1;
        else ASSERT(0, "unexpected prompt name in canonical list");
    }
    ASSERT(saw_bringup && saw_debug && saw_flash,
           "exact set: bringup, debug_serial, flash_safe");
}

static void test_prompts_list_json_exact(void)
{
    cJSON *result = sm_mcp_serial_prompts_list_result();
    ASSERT_NOT_NULL(result);
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(result, "prompts");
    ASSERT(cJSON_IsArray(arr), "prompts is array");
    ASSERT_INT_EQ(cJSON_GetArraySize(arr), 3);

    int saw_bringup = 0, saw_debug = 0, saw_flash = 0;
    cJSON *p;
    cJSON_ArrayForEach(p, arr) {
        const char *n = NULL;
        cJSON *name = cJSON_GetObjectItemCaseSensitive(p, "name");
        if (cJSON_IsString(name))
            n = name->valuestring;
        ASSERT_NOT_NULL(n);
        if (strcmp(n, "bringup") == 0) saw_bringup = 1;
        else if (strcmp(n, "debug_serial") == 0) saw_debug = 1;
        else if (strcmp(n, "flash_safe") == 0) saw_flash = 1;
        else ASSERT(0, "unexpected name in prompts/list");
    }
    ASSERT(saw_bringup && saw_debug && saw_flash,
           "list exact set of three prompts");
    cJSON_Delete(result);
}

static void assert_prompt_body(const char *name, const char *must1,
                               const char *must2)
{
    char *text = NULL;
    const char *desc = NULL;
    int rc = sm_mcp_serial_prompt_get(name, NULL, &text, &desc);
    ASSERT_INT_EQ(rc, 0);
    ASSERT_NOT_NULL(text);
    ASSERT_NOT_NULL(desc);
    ASSERT(strstr(text, must1) != NULL, "prompt body cites tool");
    ASSERT(strstr(text, must2) != NULL, "prompt body cites second tool");
    free(text);
}

static void test_prompt_bodies_cite_tools(void)
{
    assert_prompt_body("bringup", "serial_boot_status",
                       "serial_get_incidents");
    assert_prompt_body("debug_serial", "serial_get_incidents",
                       "serial_output_history");
    assert_prompt_body("flash_safe", "serial_suspend", "serial_resume");
}

static void test_prompt_unknown(void)
{
    char *text = NULL;
    const char *desc = NULL;
    int rc = sm_mcp_serial_prompt_get("nope", NULL, &text, &desc);
    ASSERT_INT_EQ(rc, -1);
    ASSERT(text == NULL, "no text on unknown");
}

static void test_prompt_bringup_port_arg(void)
{
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "port", "/dev/ttyUSB0");
    char *text = NULL;
    const char *desc = NULL;
    ASSERT_INT_EQ(sm_mcp_serial_prompt_get("bringup", args, &text, &desc), 0);
    ASSERT(text && strstr(text, "/dev/ttyUSB0") != NULL,
           "port hint appears in bringup text");
    free(text);
    cJSON_Delete(args);
}

int main(void)
{
    printf("test_mcp_instructions\n");
    RUN_TEST(test_instructions_content);
    RUN_TEST(test_prompt_names_exact_set);
    RUN_TEST(test_prompts_list_json_exact);
    RUN_TEST(test_prompt_bodies_cite_tools);
    RUN_TEST(test_prompt_unknown);
    RUN_TEST(test_prompt_bringup_port_arg);
    TEST_REPORT();
}
