/* Q-SMOLMUX-03 A: mutate tools are opt-in. Pure schema test, no broker. */
#include "test_main.h"
#include "sinks/mcp_schemas.h"
#include "cJSON.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static int list_has(cJSON *tools, const char *name)
{
    int n = cJSON_GetArraySize(tools);
    for (int i = 0; i < n; i++) {
        cJSON *t = cJSON_GetArrayItem(tools, i);
        cJSON *nm = cJSON_GetObjectItem(t, "name");
        if (cJSON_IsString(nm) && nm->valuestring &&
            strcmp(nm->valuestring, name) == 0)
            return 1;
    }
    return 0;
}

static const char *const k_ro_tools[] = {
    "serial_read", "serial_wait_for", "serial_monitor",
    "serial_output_history", "serial_port_status", "serial_boot_status",
    "serial_list_ports", "serial_get_incidents", "serial_generate_report",
};

static const char *const k_mutate_tools[] = {
    "serial_write", "serial_send_command", "serial_sysrq",
    "serial_pin_control", "serial_add_autoresponder", "serial_suspend",
    "serial_resume", "serial_add_watchdog",
};

static void test_default_hides_sysrq(void)
{
    unsetenv("SMOLMUX_MCP_MUTATE");
    ASSERT(!sm_mcp_mutate_enabled(), "mutate off by default");
    ASSERT(sm_mcp_tool_is_mutate("serial_sysrq"), "sysrq is mutate");
    ASSERT(!sm_mcp_tool_is_mutate("serial_read"), "read is not mutate");

    cJSON *tools = sm_mcp_build_tools_list();
    ASSERT_NOT_NULL(tools);
    ASSERT_INT_EQ(cJSON_GetArraySize(tools), 9);
    ASSERT(list_has(tools, "serial_read"), "read-only listed");
    for (size_t i = 0; i < sizeof(k_ro_tools) / sizeof(k_ro_tools[0]); i++)
        ASSERT(list_has(tools, k_ro_tools[i]), "default lists read tool");
    for (size_t i = 0; i < sizeof(k_mutate_tools) / sizeof(k_mutate_tools[0]); i++)
        ASSERT(!list_has(tools, k_mutate_tools[i]),
               "mutate tool hidden by default");
    cJSON_Delete(tools);
}

static void test_opt_in_lists_sysrq(void)
{
    setenv("SMOLMUX_MCP_MUTATE", "1", 1);
    ASSERT(sm_mcp_mutate_enabled(), "mutate on");
    cJSON *tools = sm_mcp_build_tools_list();
    ASSERT_NOT_NULL(tools);
    ASSERT_INT_EQ(cJSON_GetArraySize(tools), 17);
    ASSERT(list_has(tools, "serial_sysrq"), "sysrq listed when opted in");
    ASSERT(list_has(tools, "serial_write"), "write listed when opted in");
    ASSERT(list_has(tools, "serial_read"), "read still listed");
    cJSON_Delete(tools);
    unsetenv("SMOLMUX_MCP_MUTATE");
}

int main(void)
{
    printf("test_mcp_mutate\n");
    RUN_TEST(test_default_hides_sysrq);
    RUN_TEST(test_opt_in_lists_sysrq);
    TEST_REPORT();
}
