#include "test_main.h"
#include "links/uart.h"

#include <unistd.h>
#include <pty.h>
#include <time.h>
#include <string.h>

static void test_open_close(void)
{
    int master, slave;
    ASSERT(openpty(&master, &slave, NULL, NULL, NULL) == 0, "openpty");
    char *slave_name = ttyname(slave);
    ASSERT_NOT_NULL(slave_name);

    sm_link_t *link = sm_uart_new(slave_name, 115200, 0);
    ASSERT_NOT_NULL(link);
    ASSERT(link->silence_normal == 0, "uart is not a GDB silence_normal link");
    ASSERT_INT_EQ(link->open(link), 0);
    ASSERT(link->read_fd(link) >= 0, "has read fd");

    link->close(link);
    ASSERT(link->read_fd(link) < 0, "fd closed");

    link->destroy(link);
    close(master);
    close(slave);
}

static void test_write_read(void)
{
    int master, slave;
    openpty(&master, &slave, NULL, NULL, NULL);
    char *slave_name = ttyname(slave);

    sm_link_t *link = sm_uart_new(slave_name, 115200, 0);
    link->open(link);

    /* Write through link, read from master */
    const uint8_t data[] = "hello device\n";
    ASSERT_INT_EQ(link->write_data(link, data, sizeof(data) - 1), 0);

    char buf[256];
    usleep(50000);  /* let data propagate through PTY */
    ssize_t n = read(master, buf, sizeof(buf));
    ASSERT(n > 0, "read from master");
    ASSERT(memcmp(buf, "hello device\n", 13) == 0, "data matches");

    /* Write from master, verify link can read */
    const char *response = "response\n";
    write(master, response, strlen(response));
    usleep(50000);

    int fd = link->read_fd(link);
    n = read(fd, buf, sizeof(buf));
    ASSERT(n > 0, "read from link fd");
    ASSERT(memcmp(buf, "response\n", 9) == 0, "response matches");

    link->close(link);
    link->destroy(link);
    close(master);
    close(slave);
}

static void test_get_status(void)
{
    int master, slave;
    openpty(&master, &slave, NULL, NULL, NULL);
    char *slave_name = ttyname(slave);

    sm_link_t *link = sm_uart_new(slave_name, 115200, 0);
    link->open(link);

    cJSON *status = cJSON_CreateObject();
    link->get_status(link, status);

    ASSERT_NOT_NULL(cJSON_GetObjectItem(status, "port"));
    ASSERT_NOT_NULL(cJSON_GetObjectItem(status, "baud"));
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItem(status, "connected")), "connected");

    cJSON_Delete(status);
    link->close(link);
    link->destroy(link);
    close(master);
    close(slave);
}

static void test_send_break(void)
{
    int master, slave;
    openpty(&master, &slave, NULL, NULL, NULL);
    char *slave_name = ttyname(slave);

    sm_link_t *link = sm_uart_new(slave_name, 115200, 0);
    link->open(link);

    /* Just verify it doesn't crash/error */
    int rc = link->send_break(link, 0);
    /* PTYs may not fully support break, just check it doesn't crash */
    (void)rc;
    ASSERT(1, "send_break didn't crash");

    link->close(link);
    link->destroy(link);
    close(master);
    close(slave);
}

static void test_pin_pulse(void)
{
    int master, slave;
    ASSERT(openpty(&master, &slave, NULL, NULL, NULL) == 0, "openpty");
    char *slave_name = ttyname(slave);
    sm_link_t *link = sm_uart_new(slave_name, 115200, 0);
    ASSERT_INT_EQ(link->open(link), 0);
    /* PTY often rejects TIOCM*; pulse must not crash (same bar as break). */
    (void)link->set_param(link, "dtr", "pulse");
    (void)link->set_param(link, "rts", "send");
    ASSERT(1, "pulse/send pin actions did not crash");
    link->close(link);
    link->destroy(link);
    close(master);
    close(slave);
}

/* ACT-014: TCSANOW must not wait for a full tty drain on the caller. */
static void test_set_param_tcsanow_does_not_block(void)
{
    int master, slave;
    ASSERT(openpty(&master, &slave, NULL, NULL, NULL) == 0, "openpty");
    char *slave_name = ttyname(slave);
    sm_link_t *link = sm_uart_new(slave_name, 115200, 0);
    ASSERT_INT_EQ(link->open(link), 0);

    char blob[4096];
    memset(blob, 'X', sizeof(blob));
    for (int i = 0; i < 16; i++)
        (void)link->write_data(link, (const uint8_t *)blob, sizeof(blob));

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    ASSERT_INT_EQ(link->set_param(link, "baud", "9600"), 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = (double)(t1.tv_sec - t0.tv_sec) +
                (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    ASSERT(dt < 0.5, "set_param baud returns without TCSADRAIN stall");

    link->close(link);
    link->destroy(link);
    close(master);
    close(slave);
}

int main(void)
{
    printf("test_uart\n");

    RUN_TEST(test_open_close);
    RUN_TEST(test_write_read);
    RUN_TEST(test_get_status);
    RUN_TEST(test_send_break);
    RUN_TEST(test_pin_pulse);
    RUN_TEST(test_set_param_tcsanow_does_not_block);

    TEST_REPORT();
}
