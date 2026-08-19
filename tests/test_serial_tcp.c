/*
 * test_serial_tcp — serial-over-TCP device link.
 *
 * Drives the link against a local ephemeral TCP listener (single-threaded: a
 * non-blocking connect to a listening socket completes in the kernel, so
 * accept() dequeues it without a second thread). Exercises bidirectional bytes,
 * the telnet IAC parser (strip/refuse/unescape/subneg/split), outbound escaping,
 * and connect failure.
 */

#include "test_main.h"
#include "links/serial_tcp.h"
#include "links/link.h"

#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* Create a loopback listener; return its fd and write the chosen port. */
static int make_listener(int *port_out)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;   /* ephemeral */
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    if (listen(fd, 1) < 0) { close(fd); return -1; }

    socklen_t alen = sizeof(addr);
    getsockname(fd, (struct sockaddr *)&addr, &alen);
    *port_out = ntohs(addr.sin_port);
    return fd;
}

/* Read exactly `want` bytes (or as many as arrive before timeout). */
static int drain_n(int fd, uint8_t *buf, size_t want, int timeout_ms)
{
    size_t got = 0;
    while (got < want) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(&p, 1, timeout_ms) <= 0) break;
        ssize_t n = read(fd, buf + got, want - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    return (int)got;
}

/* Open the link to the listener and accept the server side. */
static int connect_pair(int listen_fd, int port, sm_link_t **link_out)
{
    sm_link_t *link = sm_serial_tcp_new("127.0.0.1", port);
    ASSERT_NOT_NULL(link);
    if (link->open(link) != 0) { link->destroy(link); return -1; }

    struct pollfd p = { .fd = listen_fd, .events = POLLIN };
    poll(&p, 1, 1000);
    int server_fd = accept(listen_fd, NULL, NULL);
    if (server_fd < 0) { link->destroy(link); return -1; }

    *link_out = link;
    return server_fd;
}

static int link_rfc2217_flag(sm_link_t *link)
{
    cJSON *st = cJSON_CreateObject();
    link->get_status(link, st);
    int active = cJSON_IsTrue(cJSON_GetObjectItem(st, "rfc2217"));
    cJSON_Delete(st);
    return active;
}

static void test_connect_and_bidirectional(void)
{
    int port, lfd = make_listener(&port);
    ASSERT(lfd >= 0, "listener created");

    sm_link_t *link = NULL;
    int sfd = connect_pair(lfd, port, &link);
    ASSERT(sfd >= 0, "connected + accepted");

    /* Device -> broker: bytes read from the link fd pass through the filter. */
    ASSERT(write(sfd, "hello", 5) == 5, "server wrote");
    uint8_t raw[64];
    int n = drain_n(link->read_fd(link), raw, 5, 1000);
    uint8_t out[64];
    size_t fn = link->filter_rx(link, raw, (size_t)n, out);
    ASSERT_INT_EQ((int)fn, 5);
    ASSERT(memcmp(out, "hello", 5) == 0, "device data delivered verbatim");

    /* Broker -> device: write_data reaches the server. */
    ASSERT_INT_EQ(link->write_data(link, (const uint8_t *)"world", 5), 0);
    uint8_t sbuf[64];
    ASSERT_INT_EQ(drain_n(sfd, sbuf, 5, 1000), 5);
    ASSERT(memcmp(sbuf, "world", 5) == 0, "keystrokes reached server");

    close(sfd);
    close(lfd);
    link->destroy(link);
}

static void test_telnet_strip_and_refuse(void)
{
    int port, lfd = make_listener(&port);
    ASSERT(lfd >= 0, "listener created");
    sm_link_t *link = NULL;
    int sfd = connect_pair(lfd, port, &link);
    ASSERT(sfd >= 0, "connected");

    /* 'A' IAC DO ECHO(1) 'B' IAC WILL SGA(3) 'C' */
    uint8_t in[] = { 'A', 255,253,1, 'B', 255,251,3, 'C' };
    ASSERT(write(sfd, in, sizeof(in)) == (ssize_t)sizeof(in), "server wrote negotiation");

    uint8_t raw[64];
    int n = drain_n(link->read_fd(link), raw, sizeof(in), 1000);
    uint8_t out[64];
    size_t fn = link->filter_rx(link, raw, (size_t)n, out);
    ASSERT_INT_EQ((int)fn, 3);
    ASSERT(memcmp(out, "ABC", 3) == 0, "IAC sequences stripped, data kept");

    /* The link refuses: IAC WONT ECHO (fc 01) and IAC DONT SGA (fe 03). */
    uint8_t resp[16];
    int rn = drain_n(sfd, resp, 6, 1000);
    ASSERT_INT_EQ(rn, 6);
    uint8_t want[] = { 255,252,1, 255,254,3 };
    ASSERT(memcmp(resp, want, 6) == 0, "refused DO->WONT and WILL->DONT");

    close(sfd);
    close(lfd);
    link->destroy(link);
}

/* The exact 21-byte negotiation burst a real ser2net 4.3.11 telnet(rfc2217)
 * accepter sends on connect (captured 2026-07-06). Must strip to zero data
 * bytes and refuse each request; ser2net offering COM-PORT (opt 44 = RFC2217)
 * is refused with WONT since the link does not speak RFC2217. */
static void test_telnet_real_ser2net_burst(void)
{
    int port, lfd = make_listener(&port);
    sm_link_t *link = NULL;
    int sfd = connect_pair(lfd, port, &link);
    ASSERT(sfd >= 0, "connected");

    uint8_t burst[] = {
        255,251,3,   /* WILL SGA   */  255,253,3,   /* DO SGA     */
        255,251,1,   /* WILL ECHO  */  255,254,1,   /* DONT ECHO  */
        255,253,0,   /* DO BINARY  */  255,251,0,   /* WILL BINARY*/
        255,253,44,  /* DO COM-PORT (RFC2217) */
    };
    ASSERT(write(sfd, burst, sizeof(burst)) == (ssize_t)sizeof(burst), "wrote burst");

    uint8_t raw[64];
    int n = drain_n(link->read_fd(link), raw, sizeof(burst), 1000);
    uint8_t out[64];
    size_t fn = link->filter_rx(link, raw, (size_t)n, out);
    ASSERT_INT_EQ((int)fn, 0);   /* pure negotiation -> no device data */

    /* Ordinary options refused (DO->WONT, WILL->DONT; DONT/WONT no reply), but
     * COM-PORT (RFC 2217) is *accepted* (DO -> WILL) so device control works. */
    uint8_t want[] = {
        255,254,3,   /* WILL SGA    -> DONT SGA     */
        255,252,3,   /* DO SGA      -> WONT SGA     */
        255,254,1,   /* WILL ECHO   -> DONT ECHO    */
        /* DONT ECHO -> (no reply) */
        255,252,0,   /* DO BINARY   -> WONT BINARY  */
        255,254,0,   /* WILL BINARY -> DONT BINARY  */
        255,251,44,  /* DO COM-PORT -> WILL COM-PORT (RFC 2217 accepted) */
    };
    uint8_t resp[64];
    int rn = drain_n(sfd, resp, sizeof(want), 1000);
    ASSERT_INT_EQ(rn, (int)sizeof(want));
    ASSERT(memcmp(resp, want, sizeof(want)) == 0, "refused others, accepted COM-PORT");
    ASSERT_INT_EQ(link_rfc2217_flag(link), 1);   /* the real burst enables RFC 2217 */

    close(sfd);
    close(lfd);
    link->destroy(link);
}

static void test_telnet_escaped_ff(void)
{
    int port, lfd = make_listener(&port);
    sm_link_t *link = NULL;
    int sfd = connect_pair(lfd, port, &link);
    ASSERT(sfd >= 0, "connected");

    /* 'X' IAC IAC 'Y' -> a literal 0xFF data byte survives. */
    uint8_t in[] = { 'X', 255,255, 'Y' };
    write(sfd, in, sizeof(in));
    uint8_t raw[32];
    int n = drain_n(link->read_fd(link), raw, sizeof(in), 1000);
    uint8_t out[32];
    size_t fn = link->filter_rx(link, raw, (size_t)n, out);
    ASSERT_INT_EQ((int)fn, 3);
    ASSERT(out[0] == 'X' && out[1] == 0xFF && out[2] == 'Y', "IAC IAC -> 0xFF");

    close(sfd);
    close(lfd);
    link->destroy(link);
}

static void test_telnet_subnegotiation(void)
{
    int port, lfd = make_listener(&port);
    sm_link_t *link = NULL;
    int sfd = connect_pair(lfd, port, &link);
    ASSERT(sfd >= 0, "connected");

    /* 'A' IAC SB <opt 3, payload 1 2> IAC SE 'B' -> subneg consumed. */
    uint8_t in[] = { 'A', 255,250, 3,1,2, 255,240, 'B' };
    write(sfd, in, sizeof(in));
    uint8_t raw[32];
    int n = drain_n(link->read_fd(link), raw, sizeof(in), 1000);
    uint8_t out[32];
    size_t fn = link->filter_rx(link, raw, (size_t)n, out);
    ASSERT_INT_EQ((int)fn, 2);
    ASSERT(out[0] == 'A' && out[1] == 'B', "subnegotiation stripped");

    close(sfd);
    close(lfd);
    link->destroy(link);
}

static void test_telnet_split_across_reads(void)
{
    /* The parser is stateful: an IAC sequence split across two filter_rx calls
     * must still be recognized. */
    int port, lfd = make_listener(&port);
    sm_link_t *link = NULL;
    int sfd = connect_pair(lfd, port, &link);
    ASSERT(sfd >= 0, "connected");

    uint8_t out[32];
    uint8_t part1[] = { 'A', 255 };          /* data + IAC (dangling) */
    uint8_t part2[] = { 253, 1, 'B' };        /* DO ECHO + data */
    size_t f1 = link->filter_rx(link, part1, sizeof(part1), out);
    ASSERT_INT_EQ((int)f1, 1);
    ASSERT(out[0] == 'A', "first chunk yields 'A'");
    size_t f2 = link->filter_rx(link, part2, sizeof(part2), out);
    ASSERT_INT_EQ((int)f2, 1);
    ASSERT(out[0] == 'B', "split IAC DO ECHO consumed, 'B' kept");

    close(sfd);
    close(lfd);
    link->destroy(link);
}

static void test_outbound_ff_escaped(void)
{
    int port, lfd = make_listener(&port);
    sm_link_t *link = NULL;
    int sfd = connect_pair(lfd, port, &link);
    ASSERT(sfd >= 0, "connected");

    /* A literal 0xFF in outbound data is telnet-escaped to IAC IAC. */
    uint8_t data[] = { 'a', 0xFF, 'b' };
    ASSERT_INT_EQ(link->write_data(link, data, sizeof(data)), 0);
    uint8_t sbuf[16];
    int n = drain_n(sfd, sbuf, 4, 1000);
    ASSERT_INT_EQ(n, 4);
    uint8_t want[] = { 'a', 255, 255, 'b' };
    ASSERT(memcmp(sbuf, want, 4) == 0, "outbound 0xFF -> IAC IAC");

    close(sfd);
    close(lfd);
    link->destroy(link);
}

/* --- RFC 2217 --- */

/* Server offers COM-PORT (IAC DO 44); driving filter_rx makes the link reply
 * IAC WILL 44 and go active. Returns after consuming the WILL from the socket. */
static int rfc2217_activate(sm_link_t *link, int sfd)
{
    uint8_t doopt[] = { 255, 253, 44 };   /* IAC DO COM-PORT */
    if (write(sfd, doopt, 3) != 3) return -1;
    uint8_t raw[8];
    int n = drain_n(link->read_fd(link), raw, 3, 1000);
    uint8_t out[8];
    link->filter_rx(link, raw, (size_t)n, out);   /* triggers the WILL reply */
    uint8_t resp[8];
    if (drain_n(sfd, resp, 3, 1000) != 3) return -1;
    if (!(resp[0] == 255 && resp[1] == 251 && resp[2] == 44)) return -1;  /* WILL */
    return 0;
}

static void test_rfc2217_negotiation(void)
{
    int port, lfd = make_listener(&port);
    sm_link_t *link = NULL;
    int sfd = connect_pair(lfd, port, &link);
    ASSERT(sfd >= 0, "connected");

    ASSERT_INT_EQ(link_rfc2217_flag(link), 0);       /* off until negotiated */
    ASSERT_INT_EQ(link->set_param(link, "baud", "115200"), -1);  /* unsupported */

    ASSERT_INT_EQ(rfc2217_activate(link, sfd), 0);   /* DO -> WILL, active */
    ASSERT_INT_EQ(link_rfc2217_flag(link), 1);

    close(sfd); close(lfd); link->destroy(link);
}

static void test_rfc2217_set_baud(void)
{
    int port, lfd = make_listener(&port);
    sm_link_t *link = NULL;
    int sfd = connect_pair(lfd, port, &link);
    ASSERT(rfc2217_activate(link, sfd) == 0, "negotiated");

    /* 115200 = 0x0001C200 -> SB COM-PORT SET-BAUDRATE 00 01 C2 00 SE */
    ASSERT_INT_EQ(link->set_param(link, "baud", "115200"), 0);
    uint8_t got[16];
    int n = drain_n(sfd, got, 10, 1000);
    uint8_t want[] = { 255,250, 44, 1, 0x00,0x01,0xC2,0x00, 255,240 };
    ASSERT_INT_EQ(n, (int)sizeof(want));
    ASSERT(memcmp(got, want, sizeof(want)) == 0, "SET-BAUDRATE subneg framed");

    close(sfd); close(lfd); link->destroy(link);
}

static void test_rfc2217_baud_iac_escaped(void)
{
    int port, lfd = make_listener(&port);
    sm_link_t *link = NULL;
    int sfd = connect_pair(lfd, port, &link);
    ASSERT(rfc2217_activate(link, sfd) == 0, "negotiated");

    /* 65280 = 0x0000FF00: the 0xFF value byte must be escaped to FF FF. */
    ASSERT_INT_EQ(link->set_param(link, "baud", "65280"), 0);
    uint8_t got[16];
    int n = drain_n(sfd, got, 11, 1000);
    uint8_t want[] = { 255,250, 44, 1, 0x00,0x00,0xFF,0xFF,0x00, 255,240 };
    ASSERT_INT_EQ(n, (int)sizeof(want));
    ASSERT(memcmp(got, want, sizeof(want)) == 0, "0xFF baud byte IAC-escaped");

    close(sfd); close(lfd); link->destroy(link);
}

static void test_rfc2217_line_controls(void)
{
    int port, lfd = make_listener(&port);
    sm_link_t *link = NULL;
    int sfd = connect_pair(lfd, port, &link);
    ASSERT(rfc2217_activate(link, sfd) == 0, "negotiated");

    struct { const char *k, *v; uint8_t ctl; } cases[] = {
        {"dtr", "set",   6}, {"dtr", "clear", 7},
        {"rts", "set",   8}, {"rts", "clear", 9},
        {"break", "set", 4}, {"break", "clear", 5},
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        ASSERT_INT_EQ(link->set_param(link, cases[i].k, cases[i].v), 0);
        uint8_t got[8];
        int n = drain_n(sfd, got, 7, 1000);
        uint8_t want[] = { 255,250, 44, 5, cases[i].ctl, 255,240 };  /* SET-CONTROL */
        ASSERT_INT_EQ(n, 7);
        ASSERT(memcmp(got, want, 7) == 0, "SET-CONTROL value");
    }

    /* toggle has no RFC 2217 mapping. */
    ASSERT_INT_EQ(link->set_param(link, "dtr", "toggle"), -1);

    close(sfd); close(lfd); link->destroy(link);
}

/* --- async connect (reconnect path) --- */

static void test_async_connect_success(void)
{
    int port, lfd = make_listener(&port);
    ASSERT(lfd >= 0, "listener created");
    sm_link_t *link = sm_serial_tcp_new("127.0.0.1", port);
    ASSERT_NOT_NULL(link);

    int rc = link->connect_begin(link);
    ASSERT(rc >= 0, "connect_begin did not fail");
    if (rc == 1) {                       /* in progress: poll to completion */
        int done = 0;
        for (int i = 0; i < 100 && !done; i++) {
            int p = link->connect_poll(link);
            ASSERT(p >= 0, "connect_poll not failed");
            if (p == 1) done = 1; else usleep(5000);
        }
        ASSERT(done, "async connect completed");
    }

    int sfd = accept(lfd, NULL, NULL);
    ASSERT(sfd >= 0, "server accepted the async connection");
    cJSON *st = cJSON_CreateObject();
    link->get_status(link, st);
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItem(st, "connected")), "reports connected");
    cJSON_Delete(st);

    close(sfd); close(lfd); link->destroy(link);
}

static void test_async_connect_fails(void)
{
    /* Grab a port then close it so the async connect is refused. */
    int port, lfd = make_listener(&port);
    close(lfd);
    sm_link_t *link = sm_serial_tcp_new("127.0.0.1", port);
    ASSERT_NOT_NULL(link);

    int rc = link->connect_begin(link);
    if (rc == 1) {                       /* pending -> must resolve to failure */
        int failed = 0;
        for (int i = 0; i < 100 && !failed; i++) {
            int p = link->connect_poll(link);
            if (p == -1) failed = 1; else { ASSERT(p == 0, "pending"); usleep(5000); }
        }
        ASSERT(failed, "async connect reported failure");
    } else {
        ASSERT_INT_EQ(rc, -1);           /* immediate refuse (loopback RST) */
    }
    link->close(link);   /* broker would close a failed connect */
    link->destroy(link);
}

static void test_connect_refused(void)
{
    /* Bind+listen to grab a port, then close it so nothing listens there. */
    int port, lfd = make_listener(&port);
    ASSERT(lfd >= 0, "listener created");
    close(lfd);

    sm_link_t *link = sm_serial_tcp_new("127.0.0.1", port);
    ASSERT_NOT_NULL(link);
    ASSERT_INT_EQ(link->open(link), -1);   /* connection refused -> open fails */

    cJSON *st = cJSON_CreateObject();
    link->get_status(link, st);
    ASSERT(cJSON_IsFalse(cJSON_GetObjectItem(st, "connected")), "reports disconnected");
    ASSERT_STR_EQ(cJSON_GetStringValue(cJSON_GetObjectItem(st, "link_type")), "serial-tcp");
    cJSON_Delete(st);

    link->destroy(link);
}

int main(void)
{
    printf("test_serial_tcp\n");
    RUN_TEST(test_connect_and_bidirectional);
    RUN_TEST(test_telnet_strip_and_refuse);
    RUN_TEST(test_telnet_real_ser2net_burst);
    RUN_TEST(test_telnet_escaped_ff);
    RUN_TEST(test_telnet_subnegotiation);
    RUN_TEST(test_telnet_split_across_reads);
    RUN_TEST(test_outbound_ff_escaped);
    RUN_TEST(test_rfc2217_negotiation);
    RUN_TEST(test_rfc2217_set_baud);
    RUN_TEST(test_rfc2217_baud_iac_escaped);
    RUN_TEST(test_rfc2217_line_controls);
    RUN_TEST(test_async_connect_success);
    RUN_TEST(test_async_connect_fails);
    RUN_TEST(test_connect_refused);
    TEST_REPORT();
}
