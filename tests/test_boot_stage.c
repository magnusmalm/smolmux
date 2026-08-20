#include "test_main.h"
#include "boot_stage.h"

/* A representative four-stage cold-boot pipeline (FSBL -> DDR -> U-Boot -> login). */
static void add_stages(sm_boot_tracker_t *t)
{
    sm_boot_add_stage(t, "fsbl",  "BL2");
    sm_boot_add_stage(t, "ddr",   "DDR.*OK");
    sm_boot_add_stage(t, "uboot", "U-Boot 20");
    sm_boot_add_stage(t, "login", "login:");
}

static size_t feed(sm_boot_tracker_t *t, const char *s, double ts)
{
    return sm_boot_feed(t, (const uint8_t *)s, strlen(s), ts);
}

/* ISSUE-DF-ESP-3: Arduino-ESP32 cold boots may never print "ESP-ROM:" - the
 * shipped ESP profiles' first stage matches the "rst:0x.." reset-reason line
 * as an alternative. Mirror of configs/esp32-arduino-lvgl stages, fed a
 * captured Arduino-style boot (no ESP-ROM) and then a classic IDF boot. */
static void add_esp_stages(sm_boot_tracker_t *t)
{
    sm_boot_add_stage(t, "reset",         "ESP-ROM:|rst:0x[0-9a-fA-F]+");
    sm_boot_add_stage(t, "flash_boot",    "SPI_FAST_FLASH_BOOT");
    sm_boot_add_stage(t, "app_entry",     "entry 0x");
    sm_boot_add_stage(t, "arduino_hello", "Hello Arduino!");
    sm_boot_add_stage(t, "lvgl_ready",    "Setup done");
}

static void test_esp_reset_alternation(void)
{
    sm_boot_tracker_t t;
    sm_boot_init(&t);
    add_esp_stages(&t);

    /* Arduino-style cold boot: reset reason line, no ESP-ROM banner. */
    size_t n = feed(&t,
        "Build:Mar 27 2021\n"
        "rst:0x1 (POWERON),boot:0x18 (SPI_FAST_FLASH_BOOT)\n"
        "SPIWP:0xee\n"
        "mode:DIO, clock div:1\n"
        "load:0x3fce3808,len:0x44c\n"
        "entry 0x403c98d0\n"
        "Hello Arduino! V8.3.10\n"
        "I am LVGL_Arduino\n"
        "Setup done\n", 1.0);
    ASSERT_INT_EQ((int)n, 5);
    ASSERT_INT_EQ(t.furthest, 4);
    ASSERT(t.stages[0].reached, "reset stage reached without ESP-ROM");
    ASSERT(sm_boot_terminal_reached(&t), "full Arduino boot tracked");

    /* Classic IDF boot: the ESP-ROM: alternative must still fire (stage-0
     * re-arrival is treated as a reboot and restarts the pipeline). */
    ASSERT_INT_EQ((int)feed(&t, "ESP-ROM:esp32s3-20210327\n", 10.0), 1);
    ASSERT(t.stages[0].reached, "reset stage reached via ESP-ROM");
    ASSERT_INT_EQ(t.furthest, 0);

    sm_boot_destroy(&t);
}

static void test_sequential_advance(void)
{
    sm_boot_tracker_t t;
    sm_boot_init(&t);
    add_stages(&t);

    ASSERT_INT_EQ(t.furthest, -1);

    ASSERT_INT_EQ((int)feed(&t, "BL2: booting\n", 1.0), 1);
    ASSERT_INT_EQ(t.furthest, 0);
    ASSERT(t.stages[0].reached, "fsbl reached");
    ASSERT(t.stages[0].reached_ts == 1.0, "fsbl ts recorded");

    ASSERT_INT_EQ((int)feed(&t, "DDR training OK\n", 2.0), 1);
    ASSERT_INT_EQ(t.furthest, 1);

    ASSERT_INT_EQ((int)feed(&t, "U-Boot 2024.01\n", 3.0), 1);
    ASSERT_INT_EQ(t.furthest, 2);
    ASSERT(!sm_boot_terminal_reached(&t), "not terminal at uboot");

    ASSERT_INT_EQ((int)feed(&t, "buildroot login:", 4.0), 1);
    ASSERT_INT_EQ(t.furthest, 3);
    ASSERT(sm_boot_terminal_reached(&t), "terminal reached at login");

    sm_boot_destroy(&t);
}

static void test_all_in_one_chunk(void)
{
    sm_boot_tracker_t t;
    sm_boot_init(&t);
    add_stages(&t);

    size_t n = feed(&t,
        "BL2: start\nDDR init OK\nU-Boot 2023.10\nmyboard login:", 5.0);
    ASSERT_INT_EQ((int)n, 4);
    ASSERT_INT_EQ(t.furthest, 3);
    ASSERT(sm_boot_terminal_reached(&t), "terminal reached");

    sm_boot_destroy(&t);
}

static void test_out_of_order_tolerance(void)
{
    /* A skipped intermediate marker must not block later stages: jumping
     * straight to U-Boot advances furthest to 2 with ddr(1) unreached. When
     * the skipped ddr marker shows up afterward it records its own reached
     * flag without pulling furthest backwards. (Stage 0 re-arriving is a
     * reboot, covered separately — an intermediate marker is not.) */
    sm_boot_tracker_t t;
    sm_boot_init(&t);
    add_stages(&t);

    ASSERT_INT_EQ((int)feed(&t, "BL2\n", 1.0), 1);
    ASSERT_INT_EQ((int)feed(&t, "U-Boot 2024.01\n", 2.0), 1);
    ASSERT_INT_EQ(t.furthest, 2);
    ASSERT(!t.stages[1].reached, "ddr skipped, not reached");

    ASSERT_INT_EQ((int)feed(&t, "DDR OK (late)\n", 3.0), 1);
    ASSERT(t.stages[1].reached, "ddr now reached");
    ASSERT_INT_EQ(t.furthest, 2);  /* unchanged */

    sm_boot_destroy(&t);
}

static void test_no_double_fire(void)
{
    sm_boot_tracker_t t;
    sm_boot_init(&t);
    add_stages(&t);

    ASSERT_INT_EQ((int)feed(&t, "BL2 start\n", 1.0), 1);
    /* Same marker again in fresh output must not re-fire the stage. */
    ASSERT_INT_EQ((int)feed(&t, "BL2 again\n", 2.0), 0);
    ASSERT(t.stages[0].reached_ts == 1.0, "ts stays at first match");

    sm_boot_destroy(&t);
}

static void test_cross_chunk_marker(void)
{
    /* A marker split across two feeds must still match via the overlap. */
    sm_boot_tracker_t t;
    sm_boot_init(&t);
    add_stages(&t);

    ASSERT_INT_EQ((int)feed(&t, "U-Bo", 1.0), 0);
    ASSERT_INT_EQ((int)feed(&t, "ot 2024.01\n", 2.0), 1);
    ASSERT_INT_EQ(t.furthest, 2);

    sm_boot_destroy(&t);
}

static void test_stall_detection(void)
{
    sm_boot_tracker_t t;
    sm_boot_init(&t);
    add_stages(&t);
    sm_boot_set_stall_timeout(&t, 10000);  /* 10s */

    ASSERT(!sm_boot_stalled(&t, 100.0), "not stalled before boot starts");

    feed(&t, "BL2\n", 100.0);
    feed(&t, "DDR OK\n", 101.0);       /* last advance at t=101 */

    ASSERT(!sm_boot_stalled(&t, 105.0), "within timeout: not stalled");
    ASSERT(sm_boot_stalled(&t, 120.0), "past timeout: stalled");

    /* Reaching the terminal stage clears stall. */
    feed(&t, "login:", 121.0);
    ASSERT(!sm_boot_stalled(&t, 200.0), "terminal reached: never stalled");

    sm_boot_destroy(&t);
}

static void test_reboot_reset_on_stage0(void)
{
    /* Stage 0 reappearing after advancing past it restarts progress. */
    sm_boot_tracker_t t;
    sm_boot_init(&t);
    add_stages(&t);

    feed(&t, "BL2\n", 1.0);
    feed(&t, "DDR OK\n", 2.0);
    feed(&t, "U-Boot 2024.01\n", 3.0);
    ASSERT_INT_EQ(t.furthest, 2);

    /* New cold boot begins: stage-0 marker again. */
    size_t n = feed(&t, "\nBL2: restarting\n", 4.0);
    ASSERT(n >= 1, "reboot re-fires stage 0");
    ASSERT_INT_EQ(t.furthest, 0);
    ASSERT(!t.stages[1].reached, "ddr cleared after reboot");
    ASSERT(!t.stages[2].reached, "uboot cleared after reboot");
    ASSERT(t.stages[0].reached_ts == 4.0, "fsbl ts is the new boot's");

    sm_boot_destroy(&t);
}

static void test_explicit_reset(void)
{
    sm_boot_tracker_t t;
    sm_boot_init(&t);
    add_stages(&t);

    feed(&t, "BL2\nDDR OK\n", 1.0);
    ASSERT_INT_EQ(t.furthest, 1);

    sm_boot_reset(&t);
    ASSERT_INT_EQ(t.furthest, -1);
    ASSERT(!t.stages[0].reached, "stage 0 cleared");
    ASSERT(!t.stages[0].announced, "announced cleared");

    sm_boot_destroy(&t);
}

static void test_empty_tracker_is_inert(void)
{
    sm_boot_tracker_t t;
    sm_boot_init(&t);

    ASSERT_INT_EQ((int)feed(&t, "anything at all\n", 1.0), 0);
    ASSERT(!sm_boot_stalled(&t, 1000.0), "empty tracker never stalls");
    ASSERT(!sm_boot_terminal_reached(&t), "empty tracker never terminal");

    sm_boot_destroy(&t);
}

int main(void)
{
    printf("test_boot_stage\n");

    RUN_TEST(test_sequential_advance);
    RUN_TEST(test_all_in_one_chunk);
    RUN_TEST(test_out_of_order_tolerance);
    RUN_TEST(test_no_double_fire);
    RUN_TEST(test_cross_chunk_marker);
    RUN_TEST(test_stall_detection);
    RUN_TEST(test_reboot_reset_on_stage0);
    RUN_TEST(test_explicit_reset);
    RUN_TEST(test_empty_tracker_is_inert);
    RUN_TEST(test_esp_reset_alternation);

    TEST_REPORT();
}
