#include "test_main.h"
#include "gdb_profile.h"

#include <stdio.h>
#include <unistd.h>

static void test_defaults(void)
{
    sm_gdb_profile_t p;
    sm_gdb_profile_init_default(&p);

    ASSERT_STR_EQ(p.name, "generic");
    ASSERT_STR_EQ(p.arch, "arm");
    ASSERT_STR_EQ(p.gdb_path, "arm-none-eabi-gdb");
    ASSERT_INT_EQ((int)p.register_count, 9);
    ASSERT_STR_EQ(p.important_registers[0], "r0");
    ASSERT_STR_EQ(p.important_registers[7], "pc");

    ASSERT_INT_EQ((int)p.fault_register_count, 4);
    ASSERT_STR_EQ(p.fault_registers[0].name, "CFSR");
    ASSERT_STR_EQ(p.fault_registers[0].address, "0xE000ED28");
    ASSERT_STR_EQ(p.fault_registers[3].name, "BFAR");

    ASSERT_INT_EQ((int)p.peripheral_count, 0);
    ASSERT_NULL(sm_gdb_profile_peripheral_addr(&p, "UARTE0"));
}

static void test_full_profile(void)
{
    const char *json =
        "{"
        "\"name\":\"nrf9151-zephyr\","
        "\"description\":\"Nordic nRF9151 SiP with Zephyr RTOS\","
        "\"arch\":\"arm\","
        "\"gdb_path\":\"/opt/zephyr-sdk/arm-zephyr-eabi/bin/arm-zephyr-eabi-gdb\","
        "\"gdb_init_commands\":[\"set pagination off\",\"set print pretty on\"],"
        "\"important_registers\":[\"r0\",\"r1\",\"msp\",\"psp\",\"control\"],"
        "\"fault_registers\":["
        "  {\"name\":\"CFSR\",\"address\":\"0xE000ED28\"},"
        "  {\"name\":\"HFSR\",\"address\":\"0xE000ED2C\"},"
        "  {\"name\":\"DFSR\",\"address\":\"0xE000ED30\"}"
        "],"
        "\"peripheral_map\":{"
        "  \"UARTE0\":\"0x40008000\","
        "  \"GPIO0\":\"0x40842500\","
        "  \"WDT\":\"0x40018000\""
        "},"
        "\"rtos\":\"zephyr\","
        "\"rtos_commands\":[\"info threads\",\"monitor zephyr threads\"]"
        "}";

    sm_gdb_profile_t p;
    sm_gdb_profile_init_default(&p);
    ASSERT_INT_EQ(sm_gdb_profile_from_json(&p, json), 0);

    ASSERT_STR_EQ(p.name, "nrf9151-zephyr");
    ASSERT_STR_EQ(p.gdb_path,
                  "/opt/zephyr-sdk/arm-zephyr-eabi/bin/arm-zephyr-eabi-gdb");
    ASSERT_STR_EQ(p.rtos, "zephyr");

    ASSERT_INT_EQ((int)p.init_command_count, 2);
    ASSERT_STR_EQ(p.init_commands[0], "set pagination off");

    /* important_registers replaced the defaults, not appended */
    ASSERT_INT_EQ((int)p.register_count, 5);
    ASSERT_STR_EQ(p.important_registers[2], "msp");

    /* fault_registers replaced (3, not the 4 defaults) */
    ASSERT_INT_EQ((int)p.fault_register_count, 3);
    ASSERT_STR_EQ(p.fault_registers[2].name, "DFSR");
    ASSERT_STR_EQ(p.fault_registers[2].address, "0xE000ED30");

    ASSERT_INT_EQ((int)p.peripheral_count, 3);
    ASSERT_STR_EQ(sm_gdb_profile_peripheral_addr(&p, "UARTE0"), "0x40008000");
    ASSERT_STR_EQ(sm_gdb_profile_peripheral_addr(&p, "GPIO0"), "0x40842500");
    ASSERT_NULL(sm_gdb_profile_peripheral_addr(&p, "NONEXISTENT"));

    ASSERT_INT_EQ((int)p.rtos_command_count, 2);
    ASSERT_STR_EQ(p.rtos_commands[1], "monitor zephyr threads");
}

static void test_partial_keeps_defaults(void)
{
    /* Only name + gdb_path present; register/fault defaults must survive. */
    const char *json = "{\"name\":\"custom\",\"gdb_path\":\"gdb-multiarch\"}";

    sm_gdb_profile_t p;
    sm_gdb_profile_init_default(&p);
    ASSERT_INT_EQ(sm_gdb_profile_from_json(&p, json), 0);

    ASSERT_STR_EQ(p.name, "custom");
    ASSERT_STR_EQ(p.gdb_path, "gdb-multiarch");
    /* defaults intact */
    ASSERT_INT_EQ((int)p.register_count, 9);
    ASSERT_INT_EQ((int)p.fault_register_count, 4);
    ASSERT_STR_EQ(p.arch, "arm");
}

static void test_bad_json(void)
{
    sm_gdb_profile_t p;
    sm_gdb_profile_init_default(&p);
    ASSERT_INT_EQ(sm_gdb_profile_from_json(&p, "{not valid json"), -1);
    /* defaults preserved after a failed parse */
    ASSERT_STR_EQ(p.name, "generic");
}

static void test_load_from_file(void)
{
    const char *tmp = getenv("TMPDIR");
    char path[512];
    snprintf(path, sizeof(path), "%s/smolmux-test-gdb-profile.json",
             tmp && tmp[0] ? tmp : "/tmp");

    FILE *fp = fopen(path, "w");
    ASSERT_NOT_NULL(fp);
    if (fp) {
        fputs("{\"name\":\"from-file\",\"peripheral_map\":{\"SPI1\":\"0x40013000\"}}",
              fp);
        fclose(fp);
    }

    sm_gdb_profile_t p;
    sm_gdb_profile_init_default(&p);
    ASSERT_INT_EQ(sm_gdb_profile_load(&p, path), 0);
    ASSERT_STR_EQ(p.name, "from-file");
    ASSERT_STR_EQ(sm_gdb_profile_peripheral_addr(&p, "SPI1"), "0x40013000");
    unlink(path);

    /* Missing file → -1, defaults preserved */
    sm_gdb_profile_init_default(&p);
    ASSERT_INT_EQ(sm_gdb_profile_load(&p, "/nonexistent/path/profile.json"), -1);
    ASSERT_STR_EQ(p.name, "generic");
}

int main(void)
{
    printf("test_gdb_profile\n");

    RUN_TEST(test_defaults);
    RUN_TEST(test_full_profile);
    RUN_TEST(test_partial_keeps_defaults);
    RUN_TEST(test_bad_json);
    RUN_TEST(test_load_from_file);

    TEST_REPORT();
}
