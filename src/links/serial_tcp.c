#include "links/serial_tcp.h"
#include "links/link_wq.h"
#include "constants.h"
#include "logger.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>

#define LOG_TAG "serial-tcp"

/* Telnet protocol bytes (RFC 854). */
enum {
    TN_SE = 240, TN_SB = 250,
    TN_WILL = 251, TN_WONT = 252, TN_DO = 253, TN_DONT = 254,
    TN_IAC = 255,
    TN_COMPORT = 44,   /* RFC 2217 COM-PORT-OPTION */
};

/* RFC 2217 client->server subnegotiation commands (SB COM-PORT <cmd> <val>). */
enum {
    CPC_SET_BAUDRATE = 1,
    CPC_SET_DATASIZE = 2,
    CPC_SET_PARITY   = 3,
    CPC_SET_STOPSIZE = 4,
    CPC_SET_CONTROL  = 5,
};

/* SET-CONTROL values we use (flow control + line signals). */
enum {
    CPCTL_FLOW_NONE = 1, CPCTL_FLOW_XONXOFF = 2, CPCTL_FLOW_HARDWARE = 3,
    CPCTL_BREAK_ON = 4, CPCTL_BREAK_OFF = 5,
    CPCTL_DTR_ON = 6, CPCTL_DTR_OFF = 7,
    CPCTL_RTS_ON = 8, CPCTL_RTS_OFF = 9,
};

/* Inbound telnet parser state. */
enum { TS_DATA, TS_IAC, TS_OPT, TS_SB, TS_SB_IAC };

typedef struct serial_tcp_data {
    char host[256];
    int port;
    int fd;
    sm_link_wq_t wq;
    int tstate;         /* telnet rx parser state */
    uint8_t topt_cmd;   /* saved WILL/WONT/DO/DONT awaiting its option byte */
    int rfc2217_active; /* server negotiated COM-PORT -> baud/pin/break work */
    struct sockaddr_storage caddr;  /* cached resolved address */
    socklen_t caddr_len;
    int caddr_valid;    /* 1 once resolved; cleared on connect failure */
} serial_tcp_data_t;

/* Resolve host:port once and cache the address, so reconnects skip the
 * blocking getaddrinfo. Caches the first result; cleared on a connect failure
 * to pick up IP changes. Returns 0 on success, -1 on resolve failure. */
static int st_resolve(serial_tcp_data_t *d)
{
    if (d->caddr_valid) return 0;

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", d->port);
    if (getaddrinfo(d->host, port_str, &hints, &res) != 0)
        return -1;

    memcpy(&d->caddr, res->ai_addr, res->ai_addrlen);
    d->caddr_len = res->ai_addrlen;
    d->caddr_valid = 1;
    freeaddrinfo(res);
    return 0;
}

/* Create a non-blocking socket and start connecting to the cached address.
 * Sets d->fd and resets per-connection telnet/RFC2217 state. Returns 0 =
 * connected, 1 = in progress (EINPROGRESS), -1 = error (d->fd left -1, errno
 * preserved). */
static int st_socket_connect(serial_tcp_data_t *d)
{
    int fd = socket(d->caddr.ss_family,
                    SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    d->fd = fd;
    d->tstate = TS_DATA;      /* fresh telnet negotiation each connection */
    d->rfc2217_active = 0;    /* renegotiated per connection */

    int rc = connect(fd, (struct sockaddr *)&d->caddr, d->caddr_len);
    if (rc == 0) return 0;
    if (errno == EINPROGRESS) return 1;

    int saved = errno;
    close(fd);
    d->fd = -1;
    errno = saved;
    return -1;
}

/* Blocking connect used at startup/resume (no clients to stall there): begin
 * the connect, then wait up to the timeout for it to complete. */
static int st_open(sm_link_t *self)
{
    serial_tcp_data_t *d = self->data;

    if (st_resolve(d) != 0) {
        SM_LOG_ERROR(LOG_TAG, "resolve %s:%d failed", d->host, d->port);
        return -1;
    }

    int rc = st_socket_connect(d);
    if (rc < 0) {
        SM_LOG_ERROR(LOG_TAG, "connect %s:%d: %s", d->host, d->port, strerror(errno));
        d->caddr_valid = 0;
        return -1;
    }
    if (rc == 0) {
        SM_LOG_INFO(LOG_TAG, "connected to %s:%d", d->host, d->port);
        return 0;
    }

    /* rc == 1: wait for the non-blocking connect to complete. */
    struct pollfd pfd = { .fd = d->fd, .events = POLLOUT };
    int pr = poll(&pfd, 1, SM_SERIAL_TCP_CONNECT_TIMEOUT_MS);
    int err = ETIMEDOUT;
    if (pr > 0 && (pfd.revents & POLLOUT)) {
        socklen_t elen = sizeof(err);
        if (getsockopt(d->fd, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err == 0) {
            SM_LOG_INFO(LOG_TAG, "connected to %s:%d", d->host, d->port);
            return 0;
        }
        if (err == 0) err = ETIMEDOUT;
    }
    errno = err;
    SM_LOG_ERROR(LOG_TAG, "connect %s:%d: %s", d->host, d->port, strerror(errno));
    close(d->fd);
    d->fd = -1;
    d->caddr_valid = 0;
    return -1;
}

/* Async connect for the broker's reconnect path: begin and return immediately.
 * Returns 0 = connected, 1 = in progress, -1 = failed (see connect_poll). */
static int st_connect_begin(sm_link_t *self)
{
    serial_tcp_data_t *d = self->data;
    if (st_resolve(d) != 0) {
        SM_LOG_DEBUG(LOG_TAG, "resolve %s:%d failed", d->host, d->port);
        return -1;
    }
    int rc = st_socket_connect(d);
    if (rc < 0) {
        SM_LOG_DEBUG(LOG_TAG, "connect %s:%d: %s", d->host, d->port, strerror(errno));
        d->caddr_valid = 0;
    }
    return rc;
}

/* Poll an in-progress async connect: 1 = connected, 0 = pending, -1 = failed. */
static int st_connect_poll(sm_link_t *self)
{
    serial_tcp_data_t *d = self->data;
    if (d->fd < 0) return -1;

    struct pollfd pfd = { .fd = d->fd, .events = POLLOUT };
    int pr = poll(&pfd, 1, 0);
    if (pr <= 0) return 0;   /* still pending (or transient EINTR) */

    int err = 0;
    socklen_t elen = sizeof(err);
    if ((pfd.revents & (POLLERR | POLLHUP)) ||
        getsockopt(d->fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) {
        d->caddr_valid = 0;   /* re-resolve next attempt (handles IP changes) */
        return -1;
    }
    SM_LOG_INFO(LOG_TAG, "connected to %s:%d", d->host, d->port);
    return 1;
}

static void st_close(sm_link_t *self)
{
    serial_tcp_data_t *d = self->data;
    if (d->fd < 0) return;

    sm_link_wq_clear(&d->wq);
    close(d->fd);
    d->fd = -1;
    SM_LOG_INFO(LOG_TAG, "disconnected from %s:%d", d->host, d->port);
}

static int st_read_fd(sm_link_t *self)  { return ((serial_tcp_data_t *)self->data)->fd; }
static int st_write_fd(sm_link_t *self) { return ((serial_tcp_data_t *)self->data)->fd; }

static int st_has_write_pending(sm_link_t *self)
{
    return sm_link_wq_has_pending(&((serial_tcp_data_t *)self->data)->wq);
}

static int st_flush_write_queue(sm_link_t *self)
{
    serial_tcp_data_t *d = self->data;
    if (d->fd < 0) return -1;
    return sm_link_wq_flush(d->fd, &d->wq);
}

/* Send raw bytes (no telnet escaping): write what we can, queue the rest. */
static int raw_send(serial_tcp_data_t *d, const uint8_t *data, size_t len)
{
    size_t written = 0;
    while (written < len) {
        ssize_t n = send(d->fd, data + written, len - written, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (sm_link_wq_enqueue(&d->wq, data + written, len - written) != 0)
                    return -1;
                return 0;
            }
            return -1;
        }
        if (n == 0)
            return -1;
        written += (size_t)n;
    }
    return 0;
}

/* Outbound: telnet-escape a literal 0xFF as IAC IAC so the server sees data,
 * not a command. Keystrokes almost never contain 0xFF, so this is a no-op path
 * in practice. */
static int st_write(sm_link_t *self, const uint8_t *data, size_t len)
{
    serial_tcp_data_t *d = self->data;
    if (d->fd < 0) return -1;

    if (!memchr(data, TN_IAC, len))
        return raw_send(d, data, len);

    uint8_t *esc = malloc(len * 2);
    if (!esc) return -1;
    size_t e = 0;
    for (size_t i = 0; i < len; i++) {
        esc[e++] = data[i];
        if (data[i] == TN_IAC)
            esc[e++] = TN_IAC;
    }
    int rc = raw_send(d, esc, e);
    free(esc);
    return rc;
}

/* Refuse a spontaneous telnet request so the server stops waiting. We only
 * answer requests (DO/WILL), never acknowledgements (DONT/WONT), to avoid
 * negotiation loops (RFC 854). Best-effort: at connect time the send buffer is
 * empty; a dropped refusal just leaves the option disabled, which is fine. */
static void telnet_refuse(serial_tcp_data_t *d, uint8_t cmd, uint8_t opt)
{
    uint8_t resp[3] = { TN_IAC, 0, opt };
    if (cmd == TN_DO)
        resp[1] = TN_WONT;
    else if (cmd == TN_WILL)
        resp[1] = TN_DONT;
    else
        return;   /* DONT/WONT: no reply */

    if (d->fd >= 0) {
        ssize_t r = send(d->fd, resp, sizeof(resp), MSG_NOSIGNAL);
        (void)r;
    }
}

/* Handle the server's COM-PORT (RFC 2217) negotiation. Reactive only: when the
 * server offers control (DO COM-PORT), accept it (WILL COM-PORT) and mark the
 * link RFC2217-capable so set_param can drive baud/pins/break. We never
 * *initiate* COM-PORT, so a raw (non-telnet) server never sees telnet bytes. */
static void telnet_comport(serial_tcp_data_t *d, uint8_t cmd)
{
    if (cmd == TN_DO) {
        d->rfc2217_active = 1;
        uint8_t resp[3] = { TN_IAC, TN_WILL, TN_COMPORT };
        if (d->fd >= 0) {
            ssize_t r = send(d->fd, resp, sizeof(resp), MSG_NOSIGNAL);
            (void)r;
        }
    } else if (cmd == TN_DONT) {
        d->rfc2217_active = 0;
    }
    /* WILL/WONT COM-PORT from the server is not expected in this direction. */
}

/* Send an RFC 2217 SB COM-PORT <cmd> <value...> SE, IAC-escaping value bytes. */
static int comport_send(serial_tcp_data_t *d, uint8_t cmd,
                        const uint8_t *val, size_t vlen)
{
    if (d->fd < 0 || !d->rfc2217_active) return -1;
    uint8_t frame[32];
    size_t n = 0;
    frame[n++] = TN_IAC; frame[n++] = TN_SB;
    frame[n++] = TN_COMPORT; frame[n++] = cmd;
    for (size_t i = 0; i < vlen; i++) {
        frame[n++] = val[i];
        if (val[i] == TN_IAC)   /* escape a literal 0xFF in the value */
            frame[n++] = TN_IAC;
    }
    frame[n++] = TN_IAC; frame[n++] = TN_SE;
    return raw_send(d, frame, n);
}

/* Strip inbound telnet IAC negotiation; pass data bytes through. Returns the
 * number of data bytes written to out (<= in_len). Stateful across chunks. */
static size_t st_filter_rx(sm_link_t *self, const uint8_t *in, size_t in_len,
                           uint8_t *out)
{
    serial_tcp_data_t *d = self->data;
    size_t o = 0;

    for (size_t i = 0; i < in_len; i++) {
        uint8_t c = in[i];
        switch (d->tstate) {
        case TS_DATA:
            if (c == TN_IAC)
                d->tstate = TS_IAC;
            else
                out[o++] = c;
            break;
        case TS_IAC:
            if (c == TN_IAC) {          /* IAC IAC -> literal 0xFF data byte */
                out[o++] = TN_IAC;
                d->tstate = TS_DATA;
            } else if (c == TN_WILL || c == TN_WONT ||
                       c == TN_DO   || c == TN_DONT) {
                d->topt_cmd = c;
                d->tstate = TS_OPT;
            } else if (c == TN_SB) {
                d->tstate = TS_SB;
            } else {                    /* SE, GA, NOP, ... — 2-byte command */
                d->tstate = TS_DATA;
            }
            break;
        case TS_OPT:
            if (c == TN_COMPORT)
                telnet_comport(d, d->topt_cmd);   /* accept RFC 2217 control */
            else
                telnet_refuse(d, d->topt_cmd, c);
            d->tstate = TS_DATA;
            break;
        case TS_SB:                     /* consume subnegotiation payload */
            if (c == TN_IAC)
                d->tstate = TS_SB_IAC;
            break;
        case TS_SB_IAC:
            if (c == TN_SE)
                d->tstate = TS_DATA;    /* end of subnegotiation */
            else
                d->tstate = TS_SB;      /* IAC IAC inside SB: ignore, keep going */
            break;
        }
    }
    return o;
}

static int st_send_break(sm_link_t *self, int duration_ms)
{
    (void)self; (void)duration_ms;
    return -1;   /* no break signal on a raw TCP stream */
}

/* Drive the remote serial port via RFC 2217. Returns -1 (unsupported) when the
 * server has not negotiated COM-PORT, matching a raw byte-pipe. */
static int st_set_param(sm_link_t *self, const char *key, const char *value)
{
    serial_tcp_data_t *d = self->data;
    if (d->fd < 0 || !d->rfc2217_active)
        return -1;

    if (strcmp(key, "baud") == 0) {
        char *endp;
        long b = strtol(value, &endp, 10);
        if (*endp || b <= 0 || b > 4000000) return -1;
        uint8_t v[4] = { (uint8_t)(b >> 24), (uint8_t)(b >> 16),
                         (uint8_t)(b >> 8),  (uint8_t)b };
        return comport_send(d, CPC_SET_BAUDRATE, v, 4);
    }
    if (strcmp(key, "data_bits") == 0) {
        int db = atoi(value);
        if (db < 5 || db > 8) return -1;
        uint8_t v = (uint8_t)db;
        return comport_send(d, CPC_SET_DATASIZE, &v, 1);
    }
    if (strcmp(key, "parity") == 0) {
        uint8_t v;
        if (strcmp(value, "none") == 0)      v = 1;
        else if (strcmp(value, "odd") == 0)  v = 2;
        else if (strcmp(value, "even") == 0) v = 3;
        else return -1;
        return comport_send(d, CPC_SET_PARITY, &v, 1);
    }
    if (strcmp(key, "stop_bits") == 0) {
        int sb = atoi(value);
        if (sb != 1 && sb != 2) return -1;
        uint8_t v = (uint8_t)sb;   /* RFC 2217: 1 -> 1 stop, 2 -> 2 stop */
        return comport_send(d, CPC_SET_STOPSIZE, &v, 1);
    }
    if (strcmp(key, "flow_control") == 0) {
        uint8_t v;
        if (strcmp(value, "none") == 0)         v = CPCTL_FLOW_NONE;
        else if (strcmp(value, "xonxoff") == 0) v = CPCTL_FLOW_XONXOFF;
        else if (strcmp(value, "rtscts") == 0)  v = CPCTL_FLOW_HARDWARE;
        else return -1;
        return comport_send(d, CPC_SET_CONTROL, &v, 1);
    }

    /* Line signals: set/clear (or 1/0). No toggle — RFC 2217 has no synchronous
     * read of the current DTR/RTS state to flip from. */
    int on;
    if (strcmp(value, "set") == 0 || strcmp(value, "1") == 0)        on = 1;
    else if (strcmp(value, "clear") == 0 || strcmp(value, "0") == 0) on = 0;
    else return -1;

    uint8_t v;
    if (strcmp(key, "dtr") == 0)        v = on ? CPCTL_DTR_ON   : CPCTL_DTR_OFF;
    else if (strcmp(key, "rts") == 0)   v = on ? CPCTL_RTS_ON   : CPCTL_RTS_OFF;
    else if (strcmp(key, "break") == 0) v = on ? CPCTL_BREAK_ON : CPCTL_BREAK_OFF;
    else return -1;
    return comport_send(d, CPC_SET_CONTROL, &v, 1);
}

static int st_get_status(sm_link_t *self, cJSON *out)
{
    serial_tcp_data_t *d = self->data;
    cJSON_AddStringToObject(out, "link_type", "serial-tcp");
    cJSON_AddStringToObject(out, "host", d->host);
    cJSON_AddNumberToObject(out, "port", d->port);
    cJSON_AddBoolToObject(out, "connected", d->fd >= 0);
    /* Whether the server negotiated RFC 2217 -> baud/pin/break control works. */
    cJSON_AddBoolToObject(out, "rfc2217", d->rfc2217_active);
    return 0;
}

static void st_destroy(sm_link_t *self)
{
    serial_tcp_data_t *d = self->data;
    if (d->fd >= 0) st_close(self);
    free(d);
    free(self);
}

sm_link_t *sm_serial_tcp_new(const char *host, int port)
{
    sm_link_t *link = calloc(1, sizeof(*link));
    serial_tcp_data_t *d = calloc(1, sizeof(*d));
    if (!link || !d) { free(link); free(d); return NULL; }

    snprintf(d->host, sizeof(d->host), "%s", host);
    d->port = port;
    d->fd = -1;
    d->tstate = TS_DATA;

    link->name = "serial-tcp";
    link->open = st_open;
    link->close = st_close;
    link->read_fd = st_read_fd;
    link->write_fd = st_write_fd;
    link->write_data = st_write;
    link->has_write_pending = st_has_write_pending;
    link->flush_write_queue = st_flush_write_queue;
    link->send_break = st_send_break;
    link->set_param = st_set_param;
    link->get_status = st_get_status;
    link->connect_begin = st_connect_begin;
    link->connect_poll = st_connect_poll;
    link->destroy = st_destroy;
    link->filter_rx = st_filter_rx;
    link->data = d;

    return link;
}
