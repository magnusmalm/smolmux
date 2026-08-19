#include "links/uart.h"
#include "links/link_wq.h"
#include "constants.h"
#include "logger.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <termios.h>
#include <sys/ioctl.h>

#define LOG_TAG "uart"

typedef struct uart_data {
    char port[256];
    int baud;
    int exclusive;
    int fd;
    struct termios orig_termios;
    int orig_saved;
    int data_bits;      /* 5, 6, 7, 8 (default 8) */
    int parity;         /* 0=none, 1=odd, 2=even */
    int stop_bits;      /* 1 or 2 (default 1) */
    int flow_control;   /* 0=none, 1=rtscts, 2=xonxoff */
    sm_link_wq_t wq;
} uart_data_t;

static speed_t baud_to_speed(int baud, int *supported)
{
    *supported = 1;
    switch (baud) {
    case 300:     return B300;
    case 600:     return B600;
    case 1200:    return B1200;
    case 2400:    return B2400;
    case 4800:    return B4800;
    case 9600:    return B9600;
    case 19200:   return B19200;
    case 38400:   return B38400;
    case 57600:   return B57600;
    case 115200:  return B115200;
    case 230400:  return B230400;
    case 460800:  return B460800;
    case 500000:  return B500000;
    case 576000:  return B576000;
    case 921600:  return B921600;
    case 1000000: return B1000000;
    case 1500000: return B1500000;
    case 2000000: return B2000000;
    case 3000000: return B3000000;
    case 4000000: return B4000000;
    default:
        SM_LOG_WARN(LOG_TAG, "unsupported baud rate %d", baud);
        *supported = 0;
        return B115200;
    }
}

static void apply_serial_config(struct termios *tty, uart_data_t *ud)
{
    /* Data bits */
    tty->c_cflag &= ~CSIZE;
    switch (ud->data_bits) {
    case 5: tty->c_cflag |= CS5; break;
    case 6: tty->c_cflag |= CS6; break;
    case 7: tty->c_cflag |= CS7; break;
    default: tty->c_cflag |= CS8; break;
    }

    /* Parity */
    tty->c_cflag &= ~(PARENB | PARODD);
    if (ud->parity == 1) {
        tty->c_cflag |= PARENB | PARODD;  /* odd */
    } else if (ud->parity == 2) {
        tty->c_cflag |= PARENB;            /* even */
    }

    /* Stop bits */
    if (ud->stop_bits == 2)
        tty->c_cflag |= CSTOPB;
    else
        tty->c_cflag &= ~CSTOPB;

    /* Flow control */
    tty->c_cflag &= ~CRTSCTS;
    tty->c_iflag &= ~(IXON | IXOFF | IXANY);
    if (ud->flow_control == 1) {
        tty->c_cflag |= CRTSCTS;
    } else if (ud->flow_control == 2) {
        tty->c_iflag |= IXON | IXOFF;
    }
}

static int uart_open(sm_link_t *self)
{
    uart_data_t *ud = self->data;

    int fd = open(ud->port, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        SM_LOG_ERROR(LOG_TAG, "open %s: %s", ud->port, strerror(errno));
        return -1;
    }

    /* Claim exclusivity before configuring so a second opener can't race
     * in and reconfigure the port between our setup steps (L5) */
    if (ud->exclusive) {
        if (ioctl(fd, TIOCEXCL) < 0)
            SM_LOG_WARN(LOG_TAG, "TIOCEXCL %s: %s", ud->port, strerror(errno));
    }

    /* Save original termios */
    if (tcgetattr(fd, &ud->orig_termios) == 0)
        ud->orig_saved = 1;

    /* Configure raw mode */
    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(fd, &tty) < 0) {
        SM_LOG_ERROR(LOG_TAG, "tcgetattr %s: %s", ud->port, strerror(errno));
        close(fd);
        return -1;
    }
    cfmakeraw(&tty);
    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    apply_serial_config(&tty, ud);

    int baud_ok;
    speed_t speed = baud_to_speed(ud->baud, &baud_ok);
    if (!baud_ok) {
        SM_LOG_ERROR(LOG_TAG, "unsupported baud rate %d for %s", ud->baud, ud->port);
        close(fd);
        return -1;
    }
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);

    if (tcsetattr(fd, TCSANOW, &tty) < 0) {
        SM_LOG_ERROR(LOG_TAG, "tcsetattr %s: %s", ud->port, strerror(errno));
        close(fd);
        return -1;
    }

    /* Flush stale data from kernel buffers */
    tcflush(fd, TCIOFLUSH);

    ud->fd = fd;
    SM_LOG_INFO(LOG_TAG, "opened %s at %d baud", ud->port, ud->baud);
    return 0;
}

static void uart_close(sm_link_t *self)
{
    uart_data_t *ud = self->data;
    if (ud->fd < 0) return;

    sm_link_wq_clear(&ud->wq);
    tcflush(ud->fd, TCIOFLUSH);
    if (ud->orig_saved)
        tcsetattr(ud->fd, TCSANOW, &ud->orig_termios);
    close(ud->fd);
    ud->fd = -1;
    SM_LOG_INFO(LOG_TAG, "closed %s", ud->port);
}

static int uart_read_fd(sm_link_t *self)
{
    uart_data_t *ud = self->data;
    return ud->fd;
}

static int uart_write_fd(sm_link_t *self)
{
    uart_data_t *ud = self->data;
    return ud->fd;
}

static int uart_has_write_pending(sm_link_t *self)
{
    uart_data_t *ud = self->data;
    return sm_link_wq_has_pending(&ud->wq);
}

static int uart_flush_write_queue(sm_link_t *self)
{
    uart_data_t *ud = self->data;
    if (ud->fd < 0) return -1;
    return sm_link_wq_flush(ud->fd, &ud->wq);
}

static int uart_write(sm_link_t *self, const uint8_t *data, size_t len)
{
    uart_data_t *ud = self->data;
    if (ud->fd < 0) return -1;

    size_t written = 0;
    while (written < len) {
        ssize_t n = write(ud->fd, data + written, len - written);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (sm_link_wq_enqueue(&ud->wq, data + written,
                                       len - written) != 0)
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

/* Synchronous break — fallback only; not reachable from sm_broker_run
 * handlers (UART break uses set_param + timerfd). Direct callers with
 * duration_ms > 0 block via nanosleep — use sm_broker_schedule_break(). */
static int uart_send_break(sm_link_t *self, int duration_ms)
{
    uart_data_t *ud = self->data;
    if (ud->fd < 0) return -1;

    if (duration_ms <= 0) {
        return tcsendbreak(ud->fd, 0);
    }

    if (duration_ms > SM_MAX_BREAK_DURATION_MS) duration_ms = SM_MAX_BREAK_DURATION_MS;
    if (ioctl(ud->fd, TIOCSBRK) < 0) return -1;
    struct timespec ts = {.tv_sec = duration_ms / 1000,
                          .tv_nsec = (duration_ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
    if (ioctl(ud->fd, TIOCCBRK) < 0) return -1;
    return 0;
}

/* Control a modem pin (DTR or RTS) by action string. */
static int uart_pin_control(int fd, int tiocm_bit, const char *action)
{
    int bits = tiocm_bit;
    if (strcmp(action, "set") == 0 || strcmp(action, "1") == 0)
        return ioctl(fd, TIOCMBIS, &bits);
    if (strcmp(action, "toggle") == 0) {
        int modem = 0;
        if (ioctl(fd, TIOCMGET, &modem) < 0) return -1;
        modem ^= tiocm_bit;
        return ioctl(fd, TIOCMSET, &modem);
    }
    return ioctl(fd, TIOCMBIC, &bits);  /* clear */
}

/* Reapply serial config after changing a line parameter. */
static int uart_reapply_config(uart_data_t *ud)
{
    struct termios tty;
    if (tcgetattr(ud->fd, &tty) < 0) return -1;
    apply_serial_config(&tty, ud);
    return tcsetattr(ud->fd, TCSADRAIN, &tty);
}

static int uart_set_param(sm_link_t *self, const char *key, const char *value)
{
    uart_data_t *ud = self->data;
    if (ud->fd < 0) return -1;

    if (strcmp(key, "baud") == 0) {
        char *endp;
        long baud_l = strtol(value, &endp, 10);
        if (*endp || baud_l <= 0 || baud_l > 4000000) return -1;
        int baud = (int)baud_l;
        int baud_ok;
        speed_t speed = baud_to_speed(baud, &baud_ok);
        if (!baud_ok) return -1;
        struct termios tty;
        if (tcgetattr(ud->fd, &tty) < 0) return -1;
        cfsetispeed(&tty, speed);
        cfsetospeed(&tty, speed);
        if (tcsetattr(ud->fd, TCSADRAIN, &tty) < 0) return -1;
        ud->baud = baud;
        SM_LOG_INFO(LOG_TAG, "baud changed to %d", baud);
        return 0;
    }

    if (strcmp(key, "dtr") == 0)
        return uart_pin_control(ud->fd, TIOCM_DTR, value);

    if (strcmp(key, "rts") == 0)
        return uart_pin_control(ud->fd, TIOCM_RTS, value);

    if (strcmp(key, "break") == 0) {
        if (strcmp(value, "set") == 0 || strcmp(value, "1") == 0)
            return ioctl(ud->fd, TIOCSBRK);
        return ioctl(ud->fd, TIOCCBRK);
    }

    if (strcmp(key, "data_bits") == 0) {
        int db = atoi(value);
        if (db < 5 || db > 8) return -1;
        ud->data_bits = db;
        return uart_reapply_config(ud);
    }

    if (strcmp(key, "parity") == 0) {
        if (strcmp(value, "none") == 0) ud->parity = 0;
        else if (strcmp(value, "odd") == 0) ud->parity = 1;
        else if (strcmp(value, "even") == 0) ud->parity = 2;
        else return -1;
        return uart_reapply_config(ud);
    }

    if (strcmp(key, "stop_bits") == 0) {
        int sb = atoi(value);
        if (sb != 1 && sb != 2) return -1;
        ud->stop_bits = sb;
        return uart_reapply_config(ud);
    }

    if (strcmp(key, "flow_control") == 0) {
        if (strcmp(value, "none") == 0) ud->flow_control = 0;
        else if (strcmp(value, "rtscts") == 0) ud->flow_control = 1;
        else if (strcmp(value, "xonxoff") == 0) ud->flow_control = 2;
        else return -1;
        return uart_reapply_config(ud);
    }

    return -1;
}

static int uart_get_status(sm_link_t *self, cJSON *out)
{
    uart_data_t *ud = self->data;
    cJSON_AddStringToObject(out, "link_type", "uart");
    cJSON_AddStringToObject(out, "port", ud->port);
    cJSON_AddNumberToObject(out, "baud", ud->baud);
    cJSON_AddNumberToObject(out, "data_bits", ud->data_bits);
    const char *parity_str = ud->parity == 1 ? "odd" : ud->parity == 2 ? "even" : "none";
    cJSON_AddStringToObject(out, "parity", parity_str);
    cJSON_AddNumberToObject(out, "stop_bits", ud->stop_bits);
    const char *fc_str = ud->flow_control == 1 ? "rtscts" : ud->flow_control == 2 ? "xonxoff" : "none";
    cJSON_AddStringToObject(out, "flow_control", fc_str);
    cJSON_AddBoolToObject(out, "connected", ud->fd >= 0);

    if (ud->fd >= 0) {
        int modem_bits = 0;
        if (ioctl(ud->fd, TIOCMGET, &modem_bits) == 0) {
            cJSON *pins = cJSON_CreateObject();
            cJSON_AddBoolToObject(pins, "dtr", (modem_bits & TIOCM_DTR) != 0);
            cJSON_AddBoolToObject(pins, "rts", (modem_bits & TIOCM_RTS) != 0);
            cJSON_AddBoolToObject(pins, "cts", (modem_bits & TIOCM_CTS) != 0);
            cJSON_AddBoolToObject(pins, "dsr", (modem_bits & TIOCM_DSR) != 0);
            cJSON_AddBoolToObject(pins, "cd",  (modem_bits & TIOCM_CD) != 0);
            cJSON_AddBoolToObject(pins, "ri",  (modem_bits & TIOCM_RI) != 0);
            cJSON_AddItemToObject(out, "pin_states", pins);
        }
    }
    return 0;
}

static void uart_destroy(sm_link_t *self)
{
    uart_data_t *ud = self->data;
    if (ud->fd >= 0) uart_close(self);
    free(ud);
    free(self);
}

sm_link_t *sm_uart_new(const char *port, int baud, int exclusive)
{
    sm_link_t *link = calloc(1, sizeof(*link));
    uart_data_t *ud = calloc(1, sizeof(*ud));
    if (!link || !ud) { free(link); free(ud); return NULL; }

    snprintf(ud->port, sizeof(ud->port), "%s", port);
    ud->baud = baud;
    ud->exclusive = exclusive;
    ud->fd = -1;
    ud->data_bits = 8;
    ud->parity = 0;
    ud->stop_bits = 1;
    ud->flow_control = 0;

    link->name = "uart";
    link->open = uart_open;
    link->close = uart_close;
    link->read_fd = uart_read_fd;
    link->write_fd = uart_write_fd;
    link->write_data = uart_write;
    link->has_write_pending = uart_has_write_pending;
    link->flush_write_queue = uart_flush_write_queue;
    link->send_break = uart_send_break;
    link->set_param = uart_set_param;
    link->get_status = uart_get_status;
    link->destroy = uart_destroy;
    link->data = ud;

    return link;
}
