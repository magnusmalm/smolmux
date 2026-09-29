/*
 * smolmux-monitor — interactive raw-mode terminal client
 *
 * Connects to a running smolmux broker over its Unix socket and provides
 * an interactive terminal for serial device access.
 *
 * Escape sequence: the prefix key (Ctrl-] by default, -e to change) then:
 *   q / prefix  quit
 *   h / ?       help
 *   s           status request
 *   t           takeover
 *   r           release
 *   b           send break
 *   z / Z       suspend / resume (release / re-acquire the port)
 */

#include "constants.h"
#include "protocol.h"
#include "broker_info.h"
#include "util/base64.h"
#include "util/json_helpers.h"
#include "util/keyspec.h"
#include "util/sock_util.h"
#include "util/auth_token.h"
#include "util/timeutil.h"
#include "monitor_esc.h"
#include "monitor_crlf.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/* --- Global state --- */

/* Read buffer grows on demand: the broker coalesces a slow observer's output
 * into a single line that can exceed SM_CLIENT_COALESCE_MAX_BYTES (16 KB), so a
 * fixed buffer would fill without a newline, read 0 bytes, and be misread as a
 * disconnect. */
#define MON_READ_BUF_INIT SM_MONITOR_READ_BUF_SIZE
#define MON_READ_BUF_MAX  (4 * 1024 * 1024)
#define MON_EVENT_HOLD_S  0.2

static struct {
    int sock_fd;
    volatile sig_atomic_t running;
    int escape_mode;
    uint8_t escape_char;     /* prefix key; default Ctrl-] (SM_MONITOR_ESCAPE_CHAR) */
    char role[16];
    char name[64];
    struct termios orig_termios;
    int raw_mode;
    int raw_output;          /* --raw: write device bytes unchanged */
    int map_lf;              /* bare LF -> CRLF on a raw-mode terminal */
    int prev_cr;             /* sm_mon_map_lf state across output chunks */
    int at_bol;              /* last device byte written ended a line */
    char pending[4096];      /* event lines held until the device line ends */
    size_t pending_len;
    double pending_since;    /* monotonic time the first held line arrived */
    char *read_buf;
    size_t read_len;
    size_t read_cap;
} mon;

/* --- Terminal raw mode --- */

static void exit_raw_mode(void)
{
    if (mon.raw_mode) {
        tcsetattr(STDIN_FILENO, TCSADRAIN, &mon.orig_termios);
        mon.raw_mode = 0;
    }
}

static void enter_raw_mode(void)
{
    if (!isatty(STDIN_FILENO))
        return;

    tcgetattr(STDIN_FILENO, &mon.orig_termios);

    struct termios raw = mon.orig_termios;
    cfmakeraw(&raw);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    mon.raw_mode = 1;
}

static void cleanup(void)
{
    exit_raw_mode();
}

static void signal_handler(int sig)
{
    (void)sig;
    mon.running = 0;
}

/* --- Socket helpers --- */

static int send_msg(cJSON *msg)
{
    size_t len;
    char *line = sm_msg_encode(msg, &len);
    if (!line) {
        cJSON_Delete(msg);
        return -1;
    }

    int rc = sm_write_all(mon.sock_fd, line, len);
    free(line);
    cJSON_Delete(msg);
    return rc;
}

/* --- Event lines --- */

/* Broker events (suspend, anomaly, boot stage, ...) print on a line of their
 * own. While the device is mid-line they are held until the device ends the
 * line, or MON_EVENT_HOLD_S passes, so they do not split device output. */
static void flush_events(void)
{
    if (mon.pending_len == 0)
        return;
    if (!mon.at_bol)
        write(STDERR_FILENO, "\r\n", 2);
    write(STDERR_FILENO, mon.pending, mon.pending_len);
    mon.pending_len = 0;
    mon.at_bol = 1;
}

static void event_line(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n > sizeof(line) - 3)
        n = (int)(sizeof(line) - 3);
    memcpy(line + n, "\r\n", 2);
    n += 2;

    if (mon.at_bol && mon.pending_len == 0) {
        write(STDERR_FILENO, line, (size_t)n);
        return;
    }
    if (mon.pending_len + (size_t)n > sizeof(mon.pending))
        flush_events();
    if (mon.pending_len == 0)
        mon.pending_since = sm_now_monotonic();
    memcpy(mon.pending + mon.pending_len, line, (size_t)n);
    mon.pending_len += (size_t)n;
    if (mon.at_bol)
        flush_events();
}

/* Write device bytes; release held events at the first line end. */
static void write_device(const uint8_t *p, size_t len)
{
    if (len == 0)
        return;
    if (mon.pending_len > 0) {
        const uint8_t *nl = memchr(p, '\n', len);
        if (nl) {
            size_t head = (size_t)(nl - p) + 1;
            write(STDOUT_FILENO, p, head);
            mon.at_bol = 1;
            flush_events();
            p += head;
            len -= head;
            if (len == 0)
                return;
        }
    }
    write(STDOUT_FILENO, p, len);
    mon.at_bol = (p[len - 1] == '\n');
}

/* --- Broker message handling --- */

static void handle_output(cJSON *root)
{
    const char *b64 = sm_json_get_string(root, "data");
    if (!b64) return;

    size_t dec_len;
    uint8_t *data = sm_base64_decode(b64, strlen(b64), &dec_len);
    if (data && dec_len > 0) {
        uint8_t *mapped = mon.map_lf ? malloc(2 * dec_len) : NULL;
        if (mapped) {
            size_t n = sm_mon_map_lf(data, dec_len, mapped, &mon.prev_cr);
            write_device(mapped, n);
            free(mapped);
        } else {
            write_device(data, dec_len);
        }
    }
    free(data);
}

static void handle_input_echo(cJSON *root)
{
    const char *sender = sm_json_get_string(root, "sender");
    const char *b64 = sm_json_get_string(root, "data");
    if (!b64) return;

    size_t dec_len;
    uint8_t *data = sm_base64_decode(b64, strlen(b64), &dec_len);
    if (!data) return;

    /* Cyan-colored echo on stderr: [sender] text */
    dprintf(STDERR_FILENO, "\033[36m[%s] ", sender ? sender : "?");
    write(STDERR_FILENO, data, dec_len);
    dprintf(STDERR_FILENO, "\033[0m\r\n");
    free(data);
}

static void handle_status_response(cJSON *root)
{
    const char *port = sm_json_get_string(root, "port");
    int baud = sm_json_get_int(root, "baud", 0);
    int connected = sm_json_get_bool(root, "connected", 0);
    int suspended = sm_json_get_bool(root, "suspended", 0);

    dprintf(STDERR_FILENO, "\r\n--- Status ---\r\n");
    dprintf(STDERR_FILENO, "  Port:      %s\r\n", port ? port : "?");
    dprintf(STDERR_FILENO, "  Baud:      %d\r\n", baud);
    dprintf(STDERR_FILENO, "  Connected: %s\r\n", connected ? "yes" : "no");
    dprintf(STDERR_FILENO, "  Suspended: %s\r\n", suspended ? "yes" : "no");
    dprintf(STDERR_FILENO, "--------------\r\n");
}

static void handle_error(cJSON *root)
{
    const char *msg = sm_json_get_string(root, "message");
    dprintf(STDERR_FILENO, "\r\n\033[31m[error] %s\033[0m\r\n",
            msg ? msg : "unknown error");
}

static void handle_suspended(cJSON *root)
{
    const char *port = sm_json_get_string(root, "port");
    event_line("\033[33m[suspended] %s\033[0m", port ? port : "serial");
}

static void handle_resumed(cJSON *root)
{
    const char *port = sm_json_get_string(root, "port");
    event_line("\033[32m[resumed] %s\033[0m", port ? port : "serial");
}

static void handle_anomaly(cJSON *root)
{
    const char *severity = sm_json_get_string(root, "severity");
    const char *pattern = sm_json_get_string(root, "pattern_name");
    const char *match = sm_json_get_string(root, "match_text");

    /* Color by severity: critical=red, warning=yellow, info=cyan */
    const char *color = "\033[36m";
    if (severity) {
        if (strcmp(severity, "critical") == 0)
            color = "\033[31m";
        else if (strcmp(severity, "warning") == 0)
            color = "\033[33m";
    }

    /* First line of the match only: the rest is device output already on
     * screen. */
    if (!match)
        match = "";
    int match_len = (int)strcspn(match, "\r\n");

    event_line("%s[anomaly:%s] %s: %.*s\033[0m",
               color,
               severity ? severity : "?",
               pattern ? pattern : "?",
               match_len, match);
}

static void handle_boot_stage(cJSON *root)
{
    const char *name = sm_json_get_string(root, "name");
    int index = sm_json_get_int(root, "index", -1);
    int total = sm_json_get_int(root, "total", 0);

    event_line("\033[32m[boot] reached %s (%d/%d)\033[0m",
               name ? name : "?", index + 1, total);
}

static void handle_boot_stall(cJSON *root)
{
    const char *name = sm_json_get_string(root, "name");
    int index = sm_json_get_int(root, "index", -1);
    int total = sm_json_get_int(root, "total", 0);
    int ms = sm_json_get_int(root, "stalled_ms", 0);

    event_line("\033[31m[boot STALLED] at %s (%d/%d) after %dms\033[0m",
               name ? name : "?", index + 1, total, ms);
}

static void handle_autoresponder_fired(cJSON *root)
{
    const char *name = sm_json_get_string(root, "name");
    int sent = sm_json_get_int(root, "sent", 0);

    event_line("\033[36m[autorespond] %s fired (%d bytes)\033[0m",
               name ? name : "?", sent);
}

static void dispatch_message(sm_msg_t *msg)
{
    switch (msg->type) {
    case SM_MSG_OUTPUT:          handle_output(msg->root); break;
    case SM_MSG_INPUT_ECHO:      handle_input_echo(msg->root); break;
    case SM_MSG_STATUS_RESPONSE: handle_status_response(msg->root); break;
    case SM_MSG_ERROR:           handle_error(msg->root); break;
    case SM_MSG_SUSPENDED:       handle_suspended(msg->root); break;
    case SM_MSG_RESUMED:         handle_resumed(msg->root); break;
    case SM_MSG_ANOMALY:         handle_anomaly(msg->root); break;
    case SM_MSG_BOOT_STAGE:      handle_boot_stage(msg->root); break;
    case SM_MSG_BOOT_STALL:      handle_boot_stall(msg->root); break;
    case SM_MSG_AUTORESPONDER_FIRED: handle_autoresponder_fired(msg->root); break;
    case SM_MSG_WELCOME:
        /* Update role from welcome */
        {
            const char *r = sm_json_get_string(msg->root, "your_role");
            if (r)
                snprintf(mon.role, sizeof(mon.role), "%s", r);
        }
        break;
    default:
        break;
    }
}

static void handle_broker_data(void)
{
    /* Grow rather than stall at 0 read space (which looks like a disconnect)
     * when the buffer is full of a newline-less line, bounded by MON_READ_BUF_MAX. */
    if (mon.read_len + 1 >= mon.read_cap) {
        size_t new_cap = mon.read_cap ? mon.read_cap * 2 : MON_READ_BUF_INIT;
        if (new_cap > MON_READ_BUF_MAX) {
            if (mon.read_cap >= MON_READ_BUF_MAX) {
                fprintf(stderr, "smolmux-monitor: response line exceeds %d bytes\n",
                        MON_READ_BUF_MAX);
                mon.running = 0;
                return;
            }
            new_cap = MON_READ_BUF_MAX;
        }
        char *nb = realloc(mon.read_buf, new_cap);
        if (!nb) { mon.running = 0; return; }
        mon.read_buf = nb;
        mon.read_cap = new_cap;
    }

    ssize_t n = read(mon.sock_fd,
                     mon.read_buf + mon.read_len,
                     mon.read_cap - mon.read_len - 1);
    if (n <= 0) {
        mon.running = 0;
        return;
    }
    mon.read_len += (size_t)n;
    mon.read_buf[mon.read_len] = '\0';

    /* Process complete lines */
    char *start = mon.read_buf;
    char *nl;
    while ((nl = memchr(start, '\n', mon.read_len - (size_t)(start - mon.read_buf))) != NULL) {
        size_t line_len = (size_t)(nl - start);
        sm_msg_t msg = sm_msg_decode(start, line_len);
        if (msg.root)
            dispatch_message(&msg);
        sm_msg_free(&msg);
        start = nl + 1;
    }

    /* Shift remaining data to front */
    size_t remaining = mon.read_len - (size_t)(start - mon.read_buf);
    if (remaining > 0 && start != mon.read_buf)
        memmove(mon.read_buf, start, remaining);
    mon.read_len = remaining;
}

/* --- Escape commands --- */

static void show_help(void)
{
    char key[16];
    sm_format_escape_key(mon.escape_char, key, sizeof(key));
    dprintf(STDERR_FILENO,
        "\r\n--- Escape commands (%s then key) ---\r\n"
        "  q    quit\r\n", key);
    dprintf(STDERR_FILENO,
        "  h/?  this help\r\n"
        "  s    status\r\n"
        "  t    takeover (exclusive send)\r\n"
        "  r    release takeover\r\n"
        "  b    send break\r\n"
        "  z    suspend (release port for external tools)\r\n"
        "  Z    resume (re-acquire port)\r\n"
        "--------------------------------------------\r\n");
}

static void observer_restart_hint(void)
{
    dprintf(STDERR_FILENO,
            "\r\n[keystrokes not sent; restart with -c]\r\n");
}

static void handle_escape(uint8_t ch)
{
    /* Prefix-twice quits (the prefix key is runtime-configurable, so it
     * cannot be a switch case label). */
    if (ch == mon.escape_char) {
        mon.running = 0;
        return;
    }

    int kind = sm_mon_esc_kind(ch);
    switch (kind) {
    case SM_MON_ESC_QUIT:
        mon.running = 0;
        break;
    case SM_MON_ESC_HELP:
        show_help();
        break;
    case SM_MON_ESC_RESTART_C:
        observer_restart_hint();
        break;
    default:
        break;
    }
    /* Broker JSON only via sm_mon_esc_wire — RESTART_C is NONE (no hello). */
    switch (sm_mon_esc_wire(kind)) {
    case SM_MON_WIRE_STATUS:
        send_msg(sm_msg_status("mon-status"));
        break;
    case SM_MON_WIRE_TAKEOVER:
        send_msg(sm_msg_takeover("mon-takeover"));
        dprintf(STDERR_FILENO, "\r\n[requesting takeover]\r\n");
        break;
    case SM_MON_WIRE_RELEASE:
        send_msg(sm_msg_release("mon-release"));
        dprintf(STDERR_FILENO, "\r\n[releasing takeover]\r\n");
        break;
    case SM_MON_WIRE_BREAK:
        send_msg(sm_msg_pin_control("mon-brk", "break", "pulse", 250));
        dprintf(STDERR_FILENO, "\r\n[sending break]\r\n");
        break;
    case SM_MON_WIRE_SUSPEND:
        send_msg(sm_msg_suspend("mon-suspend"));
        dprintf(STDERR_FILENO, "\r\n[requesting suspend — releasing port]\r\n");
        break;
    case SM_MON_WIRE_RESUME:
        send_msg(sm_msg_resume("mon-resume"));
        dprintf(STDERR_FILENO, "\r\n[requesting resume — re-acquiring port]\r\n");
        break;
    case SM_MON_WIRE_NONE:
        if (kind == SM_MON_ESC_FORWARD) {
            if (strcmp(mon.role, "controller") == 0) {
                uint8_t pair[2] = { mon.escape_char, ch };
                send_msg(sm_msg_send("mon", pair, 2));
            } else {
                observer_restart_hint();
            }
        }
        break;
    }
}

/* --- Stdin handling --- */

static void handle_stdin_data(void)
{
    uint8_t buf[256];
    ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0) {
        mon.running = 0;
        return;
    }

    int is_controller = (strcmp(mon.role, "controller") == 0);

    /* Split the input into forwardable data and escape sequences. A byte is
     * forwarded to the device only if it is neither the prefix key nor the
     * command that follows it — the escaped command byte is consumed by
     * handle_escape and must never leak onto the wire. Pending data is flushed
     * before each escape so ordering (data, then command) is preserved. */
    uint8_t out[sizeof(buf)];
    size_t out_len = 0;

    for (ssize_t i = 0; i < n; i++) {
        uint8_t ch = buf[i];

        if (mon.escape_mode) {
            handle_escape(ch);       /* consumes ch (may emit its own message) */
            mon.escape_mode = 0;
            continue;
        }

        if (ch == mon.escape_char) {
            if (out_len > 0 && is_controller) {
                send_msg(sm_msg_send("mon", out, out_len));
                out_len = 0;
            }
            mon.escape_mode = 1;
            continue;
        }

        out[out_len++] = ch;
    }

    if (out_len > 0 && is_controller)
        send_msg(sm_msg_send("mon", out, out_len));
    else if (out_len > 0)
        observer_restart_hint();
}

/* --- Main --- */

static void usage(FILE *out, const char *prog)
{
    fprintf(out,
        "%s-monitor — interactive terminal client for smolmux\n"
        "\n"
        "Connects to a running smolmux broker and provides raw terminal\n"
        "access to the serial device. Press the prefix key (Ctrl-] by\n"
        "default) then 'h' for escape commands.\n"
        "\n"
        "USAGE:\n"
        "  %s [options] [socket_path]\n"
        "\n"
        "OPTIONS:\n"
        "  -c, --controller        Connect as controller (default: observer)\n"
        "  -n, --name <name>       Client name (default: monitor)\n"
        "  -e, --escape <key>      Prefix key: caret notation (^], ^A, ^?) or\n"
        "                          'esc' (default: ^] = Ctrl-])\n"
        "  -L, --list              List active brokers and what each holds, then exit\n"
        "  -s, --socket <path>     Broker socket (not the TTY; a char device\n"
        "                          is derived to the broker sock)\n"
        "  --tcp <host:port>       Connect via TCP instead of Unix socket\n"
        "  --raw                   Write device bytes unchanged (default: a bare\n"
        "                          LF from the device is shown as CRLF)\n"
        "  -V, --version           Show version\n"
        "  -h, --help              Show help\n"
        "\n"
        "EXAMPLES:\n"
        "  %s -L                                # List active brokers\n"
        "  %s                                   # Attach (auto-discover; lists if >1)\n"
        "  %s -c                                # Connect as controller\n"
        "  %s /tmp/smolmux-ttyUSB0.sock         # Explicit socket path\n"
        "  %s --tcp 192.168.1.100:5555           # Connect via TCP\n"
        "\n"
        "ESCAPE COMMANDS (prefix key then key; prefix defaults to Ctrl-]):\n"
        "  q    quit\n"
        "  h/?  help\n"
        "  s    status request\n"
        "  t    takeover (exclusive send)\n"
        "  r    release takeover\n"
        "  b    send break\n"
        "  z    suspend (release port for external tools)\n"
        "  Z    resume (re-acquire port)\n"
        "\n"
        "SOCKET DISCOVERY:\n"
        "  1. -s / --socket or positional argument\n"
        "  2. $SMOLMUX_SOCKET environment variable\n"
        "  3. Glob $XDG_RUNTIME_DIR/smolmux-*.sock\n"
        "  4. Glob /tmp/smolmux-*.sock\n"
        "\n"
        "ENVIRONMENT:\n"
        "  SMOLMUX_SOCKET    Override broker socket path\n",
        SM_NAME, prog, prog, prog, prog, prog, prog);
}

/* List all active brokers and what each holds. Returns the number found. */
static size_t list_brokers(void)
{
    sm_broker_info_t infos[64];
    size_t n = sm_broker_discover(infos, 64, 1000);
    if (n == 0) {
        fprintf(stderr, "No active smolmux brokers found.\n");
        return 0;
    }
    size_t shown = n < 64 ? n : 64;
    fprintf(stderr, "Active smolmux brokers (%zu):\n", n);
    for (size_t i = 0; i < shown; i++) {
        char line[480];
        sm_broker_info_format(&infos[i], line, sizeof(line));
        fprintf(stderr, "  %s\n", line);
    }
    if (n > shown)
        fprintf(stderr, "  ... and %zu more\n", n - shown);
    return n;
}

int main(int argc, char *argv[])
{
    const char *socket_path = NULL;
    const char *tcp_target = NULL;
    int controller = 0;
    int do_list = 0;

    memset(&mon, 0, sizeof(mon));
    mon.sock_fd = -1;
    mon.running = 1;
    mon.at_bol = 1;
    mon.escape_char = SM_MONITOR_ESCAPE_CHAR;
    snprintf(mon.name, sizeof(mon.name), "monitor");
    snprintf(mon.role, sizeof(mon.role), "observer");

    static const struct option long_opts[] = {
        {"controller", no_argument,       NULL, 'c'},
        {"name",       required_argument, NULL, 'n'},
        {"escape",     required_argument, NULL, 'e'},
        {"socket",     required_argument, NULL, 's'},
        {"tcp",        required_argument, NULL, 'T'},
        {"raw",        no_argument,       NULL, 'R'},
        {"list",       no_argument,       NULL, 'L'},
        {"version",    no_argument,       NULL, 'V'},
        {"help",       no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "cn:e:s:LVh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'c':
            controller = 1;
            break;
        case 's':
            socket_path = optarg;
            break;
        case 'L':
            do_list = 1;
            break;
        case 'n':
            snprintf(mon.name, sizeof(mon.name), "%s", optarg);
            break;
        case 'e':
            if (sm_parse_escape_key(optarg, &mon.escape_char) != 0) {
                fprintf(stderr, "Error: invalid escape key '%s'\n"
                        "  Use caret notation (^], ^A, ^?) or 'esc'\n", optarg);
                return 1;
            }
            break;
        case 'T':
            tcp_target = optarg;
            break;
        case 'R':
            mon.raw_output = 1;
            break;
        case 'V':
            printf("%s-monitor %s\n", SM_NAME, SM_VERSION);
            return 0;
        case 'h':
            usage(stdout, argv[0]);
            return 0;
        default:
            usage(stderr, argv[0]);
            return 1;
        }
    }

    /* --list: enumerate brokers and exit (no connection). */
    if (do_list) {
        list_brokers();
        return 0;
    }

    if (controller)
        snprintf(mon.role, sizeof(mon.role), "controller");

    /* Connect via TCP or Unix socket */
    char connect_desc[256];
    if (tcp_target) {
        /* Parse host:port */
        char host[240];
        int port = SM_TCP_DEFAULT_PORT;
        sm_parse_host_port(tcp_target, host, sizeof(host), &port);

        sm_auth_token_autoload(host, port);
        mon.sock_fd = sm_connect_tcp(host, port);
        if (mon.sock_fd < 0) {
            fprintf(stderr, "Error: cannot connect to %s:%d: %s\n",
                    host, port, strerror(errno));
            return 1;
        }
        snprintf(connect_desc, sizeof(connect_desc), "tcp://%s:%d", host, port);
    } else {
        /* Socket path: positional arg > $SMOLMUX_SOCKET > auto-discovery.
         * With multiple brokers, auto-discovery lists them instead of
         * attaching to an arbitrary one (todo #1). */
        char discovered_path[SM_SOCK_PATH_MAX];
        const char *env = getenv(SM_SOCKET_ENV);
        if (socket_path) {
            /* -s / --socket already set */
        } else if (optind < argc) {
            socket_path = argv[optind];
        } else if (env && env[0]) {
            socket_path = env;
        } else {
            char socks[64][SM_SOCK_PATH_MAX];
            size_t n = sm_discover_reachable_sockets(socks, 64);
            if (n == 0) {
                char leftover[64][SM_SOCK_PATH_MAX];
                size_t nall = sm_discover_all_sockets(leftover, 64);
                if (nall > 0)
                    fprintf(stderr,
                            "Error: no live broker socket found\n"
                            "  %zu leftover socket(s); smolmux-cli brokers lists them\n"
                            "  Specify path, set %s, or start a broker\n",
                            nall, SM_SOCKET_ENV);
                else
                    fprintf(stderr, "Error: no broker socket found\n"
                            "  Specify path, set %s, or start a broker\n",
                            SM_SOCKET_ENV);
                return 1;
            } else if (n == 1) {
                snprintf(discovered_path, sizeof(discovered_path), "%s", socks[0]);
                socket_path = discovered_path;
            } else {
                fprintf(stderr, "Multiple brokers active — pick one:\n");
                list_brokers();
                fprintf(stderr, "\nThen: %s <socket>\n", argv[0]);
                return 1;
            }
        }

        {
            char resolved[SM_SOCK_PATH_MAX];
            int rrs = sm_resolve_client_socket(resolved, sizeof(resolved),
                                               socket_path);
            if (rrs < 0) {
                fprintf(stderr, "Error: cannot resolve socket from '%s'\n"
                                "  smolmux-cli brokers\n", socket_path);
                return 1;
            }
            if (rrs == 1) {
                fprintf(stderr,
                        "note: '%s' is a device node; using socket %s\n"
                        "  smolmux-cli brokers lists live brokers\n",
                        socket_path, resolved);
            }
            snprintf(discovered_path, sizeof(discovered_path), "%s", resolved);
            socket_path = discovered_path;
        }
        mon.sock_fd = sm_connect_unix(socket_path);
        if (mon.sock_fd < 0) {
            fprintf(stderr, "Error: cannot connect to %s: %s\n",
                    socket_path, strerror(errno));
            return 1;
        }
        snprintf(connect_desc, sizeof(connect_desc), "%s", socket_path);
    }

    /* Send hello */
    send_msg(sm_msg_hello(mon.name, mon.role));

    /* Read welcome */
    handle_broker_data();

    /* Print banner */
    char key_label[16];
    sm_format_escape_key(mon.escape_char, key_label, sizeof(key_label));
    dprintf(STDERR_FILENO,
        "Connected to %s as %s (%s)\r\n"
        "Escape: %s then h for help, q to quit\r\n",
        connect_desc, mon.name, mon.role, key_label);

    /* Set up signal handling */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* Enter raw mode and register cleanup */
    atexit(cleanup);
    enter_raw_mode();
    /* Only a raw-mode terminal loses its own LF -> CRLF; a pipe or file
     * gets the device bytes as sent. */
    mon.map_lf = mon.raw_mode && !mon.raw_output && isatty(STDOUT_FILENO);

    /* Event loop */
    struct pollfd fds[2] = {
        { .fd = STDIN_FILENO,  .events = POLLIN },
        { .fd = mon.sock_fd,   .events = POLLIN },
    };

    while (mon.running) {
        int timeout_ms = -1;
        if (mon.pending_len > 0) {
            double left = mon.pending_since + MON_EVENT_HOLD_S - sm_now_monotonic();
            timeout_ms = left > 0 ? (int)(left * 1000.0) + 1 : 0;
        }
        int ret = poll(fds, 2, timeout_ms);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (fds[1].revents & POLLIN)
            handle_broker_data();
        if (mon.pending_len > 0 &&
            sm_now_monotonic() - mon.pending_since >= MON_EVENT_HOLD_S)
            flush_events();
        if (fds[1].revents & (POLLHUP | POLLERR)) {
            flush_events();
            dprintf(STDERR_FILENO, "\r\n[broker disconnected]\r\n");
            break;
        }
        if (fds[0].revents & POLLIN)
            handle_stdin_data();
    }

    /* Clean up */
    flush_events();
    close(mon.sock_fd);
    dprintf(STDERR_FILENO, "\r\n[disconnected]\r\n");

    return 0;
}
