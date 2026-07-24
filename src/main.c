#include "constants.h"
#include "broker.h"
#include "broker_info.h"
#include "device_profile.h"
#include "links/uart.h"
#include "logger.h"
#include "util/sock_util.h"
#include "util/profile_resolve.h"
#include "sm_features.h"

#if SM_ENABLE_GDB
#include "links/gdb.h"
#endif

#if SM_ENABLE_LINK_SERIAL_TCP
#include "links/serial_tcp.h"
#endif

#if SM_ENABLE_SINK_MCP
#include "sinks/mcp.h"
#endif

#if SM_ENABLE_SINK_TCP
#include "sinks/tcp.h"
#endif

#if SM_ENABLE_SINK_WS
#include "sinks/ws.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <glob.h>
#include <unistd.h>

static sm_broker_t broker;

static void signal_handler(int sig)
{
    (void)sig;
    sm_broker_stop(&broker);
}

/* After a startup failure on a UART port, say WHO holds it instead of leaving
 * the operator to fuser/lsof archaeology. Re-opens the port briefly to get a
 * fresh errno; only speaks when that confirms a busy/permission failure. */
static void diagnose_busy_port(const char *port)
{
    int fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) {
        close(fd);
        return;                     /* port opens fine — failure was elsewhere */
    }
    if (errno != EBUSY && errno != EACCES && errno != EPERM)
        return;
    int open_errno = errno;

    sm_broker_info_t holder;
    if (sm_find_broker_for_endpoint(port, &holder, 800) == 0) {
        fprintf(stderr,
                "port %s is held by smolmux pid=%d socket=%s%s%s%s\n"
                "  stop it with: %s-cli -s %s shutdown\n",
                port, holder.pid, holder.socket,
                holder.board[0] ? " board=" : "",
                holder.board[0] ? holder.board : "",
                holder.suspended ? " (suspended)" : "",
                SM_NAME, holder.socket);
    } else if (open_errno == EBUSY) {
        fprintf(stderr,
                "port %s is held by another process (try: fuser -v %s)\n",
                port, port);
    }
}

#if SM_ENABLE_GDB
/* Reap exited GDB children so repeated crashes don't accumulate zombies
 * (M8). gdb_close()'s waitpid tolerates ECHILD when we win the race. */
static void sigchld_handler(int sig)
{
    (void)sig;
    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
    errno = saved_errno;
}
#endif

static void usage(const char *prog)
{
    fprintf(stderr,
        "%s — serial device multiplexer\n"
        "\n"
        "Holds a serial port open and multiplexes access to multiple clients\n"
        "over a Unix socket. Clients connect with smolmux-cli or smolmux-monitor.\n"
        "\n"
        "USAGE:\n"
        "  %s <port> [options]\n"
        "  %s --gdb --gdb-target localhost:3333\n"
        "  %s --list-ports\n"
        "  %s --list-profiles\n"
        "\n"
        "OPTIONS:\n"
        "  -b, --baud <rate>           Baud rate (default: %d)\n"
        "  -s, --socket <path>         Unix socket path\n"
        "  -l, --log-dir <dir>         I/O log directory (default: %s)\n"
        "  -t, --text-log-dir <dir>    Text log directory\n"
        "  -p, --profile <path>        Device profile JSON file\n"
        "  --board <name>              Group this wire under a board (for discovery)\n"
        "  --role <label>              This wire's role on the board (console, swd, ...)\n"
#if SM_ENABLE_GDB
        "  --gdb                       Use GDB MI link instead of UART\n"
        "  --gdb-path <path>           Path to gdb binary (default: gdb)\n"
        "  --gdb-target <spec>         GDB target (e.g., localhost:3333)\n"
#endif
#if SM_ENABLE_LINK_SERIAL_TCP
        "  --serial-tcp <host:port>    Connect to a serial-over-TCP device server\n"
        "                              (ser2net, socat, terminal server; telnet-aware)\n"
#endif
#if SM_ENABLE_SINK_MCP
        "  --mcp                       Enable MCP stdio sink\n"
#endif
#if SM_ENABLE_SINK_TCP
        "  --tcp-port <port>           Enable TCP sink on port (default: %d)\n"
        "  --tcp-bind <addr>           TCP bind address (default: 127.0.0.1)\n"
        "  --auth-token <token>        Require this token in hello from TCP clients\n"
        "                              (prefer env SMOLMUX_AUTH_TOKEN — hidden from ps)\n"
#endif
#if SM_ENABLE_SINK_WS
        "  --ws-port <port>            Enable WebSocket sink on port (default: %d)\n"
#endif
        "  --no-text-log               Disable text log\n"
        "  --no-reconnect              Don't auto-reconnect on disconnect\n"
        "  --list-ports                List available serial ports and exit\n"
        "  --list-profiles             List available device profiles and exit\n"
        "  --help-protocol             Show wire protocol documentation\n"
        "  -v, --verbose               Enable debug logging\n"
        "  -V, --version               Show version\n"
        "  -h, --help                  Show this help\n"
        "\n"
        "EXAMPLES:\n"
        "  %s /dev/ttyUSB0                          # Default 115200 baud\n"
        "  %s /dev/ttyUSB0 -b 9600                  # Custom baud rate\n"
        "  %s /dev/ttyACM0 -p profiles/nrf9151.json # With device profile\n"
        "  %s /dev/ttyUSB0 --tcp-port 5555          # Enable remote TCP access\n"
        "  %s --list-ports                           # Discover serial ports\n"
        "\n"
        "SOCKET PATH:\n"
        "  Auto-derived from the device/label basename:\n"
        "    $XDG_RUNTIME_DIR/smolmux-ttyUSB0.sock  (preferred, secure)\n"
        "    /tmp/smolmux-ttyUSB0.sock              (fallback)\n"
        "  Long by-id basenames are shortened to a stable form that always\n"
        "  fits Unix domain sockets (clients still auto-discover by glob).\n"
        "  Override with -s <path>.\n"
        "  Clients find it via $SMOLMUX_SOCKET or auto-discovery.\n"
        "\n"
        "ENVIRONMENT:\n"
        "  SMOLMUX_SOCKET           Override socket path for clients\n"
        "  SMOLMUX_DEVICE_PROFILE   Path to device profile JSON\n"
        "  XDG_RUNTIME_DIR          Preferred directory for socket files\n"
        "\n"
        "COMPANION TOOLS:\n"
        "  smolmux-cli              Command-line client (send commands, read output)\n"
        "  smolmux-monitor          Interactive terminal client (Ctrl-] to escape)\n"
        "  smolmux-watcher          Daemon that saves anomaly incident reports\n",
        prog, prog, prog, prog, prog, SM_DEFAULT_BAUD, SM_LOG_DIR
#if SM_ENABLE_SINK_TCP
        , SM_TCP_DEFAULT_PORT
#endif
#if SM_ENABLE_SINK_WS
        , SM_WS_DEFAULT_PORT
#endif
        , prog, prog, prog, prog, prog
    );
}

static void print_protocol_help(void)
{
    printf(
        "smolmux Wire Protocol Reference\n"
        "================================\n"
        "\n"
        "TRANSPORT:\n"
        "  Unix domain socket (also TCP/WS sinks), newline-delimited JSON.\n"
        "  Binary data is base64-encoded in all messages.\n"
        "  Core protocol messages plus boot / autoboot / autoresponder extensions.\n"
        "\n"
        "HANDSHAKE:\n"
        "  Client sends 'hello', broker responds with 'welcome'.\n"
        "  No other messages accepted before hello. Hello cannot be re-sent.\n"
        "  TCP clients may require token when --auth-token / SMOLMUX_AUTH_TOKEN is set.\n"
        "\n"
        "  -> {\"type\":\"hello\",\"name\":\"my-tool\",\"role\":\"controller\",\"protocol_version\":1}\n"
        "  <- {\"type\":\"welcome\",\"broker_version\":\"0.1.0\",\"protocol_version\":1,\n"
        "      \"port\":\"/dev/ttyUSB0\",\"baud\":115200,\"your_role\":\"controller\"}\n"
        "\n"
        "ROLES:\n"
        "  observer    — read-only: receives output, status, anomaly/boot events\n"
        "  controller  — read-write: send data, pins, suspend/resume, configure rules\n"
        "\n"
        "  A controller can request exclusive access with 'takeover'.\n"
        "\n"    );
    printf(
        "CLIENT -> BROKER MESSAGES:\n"
        "\n"
        "  hello             Connect and declare role.\n"
        "    {\"type\":\"hello\",\"name\":\"<name>\",\"role\":\"<observer|controller>\",\n"
        "     \"protocol_version\":1[,\"token\":\"<auth>\"]}\n"
        "\n"
        "  send              Write data to device (controller only).\n"
        "    {\"type\":\"send\",\"id\":\"<id>\",\"data\":\"<base64>\"}\n"
        "\n"
        "  send_expect       Write data + wait for regex match with timeout.\n"
        "    {\"type\":\"send_expect\",\"id\":\"<id>\",\"data\":\"<base64>\",\n"
        "     \"pattern\":\"<regex>\",\"timeout_ms\":<int>}\n"
        "\n"
        "  takeover          Request exclusive send access.\n"
        "    {\"type\":\"takeover\",\"id\":\"<id>\"}\n"
        "\n"
        "  release           Release exclusive access.\n"
        "    {\"type\":\"release\",\"id\":\"<id>\"}\n"
        "\n"
        "  status            Query broker state (includes boot progress when configured).\n"
        "    {\"type\":\"status\",\"id\":\"<id>\"}\n"
        "\n"
        "  pin_control       Control DTR/RTS or send break.\n"
        "    {\"type\":\"pin_control\",\"id\":\"<id>\",\"pin\":\"<dtr|rts|break>\",\n"
        "     \"action\":\"<set|clear|toggle|pulse>\",\"duration_ms\":<int>}\n"
        "\n"
        "  set_baud          Change baud rate at runtime.\n"
        "    {\"type\":\"set_baud\",\"id\":\"<id>\",\"baud\":<int>}\n"
        "\n"
        "  suspend           Close serial port for external tool access.\n"
        "    {\"type\":\"suspend\",\"id\":\"<id>\"}\n"
        "\n"
        "  resume            Re-open serial port after suspend.\n"
        "    {\"type\":\"resume\",\"id\":\"<id>\"}\n"
        "\n"    );
    printf(
        "  history_request   Retrieve output history.\n"
        "    {\"type\":\"history_request\",\"id\":\"<id>\",\"since_ts\":<double>,\n"
        "     \"last_bytes\":<int>}\n"
        "\n"
        "  incidents_request Retrieve anomaly incidents.\n"
        "    {\"type\":\"incidents_request\",\"id\":\"<id>\",\"since_ts\":<double>}\n"
        "\n"
        "  configure_anomaly Add anomaly detection patterns.\n"
        "    {\"type\":\"configure_anomaly\",\"id\":\"<id>\",\"patterns\":[\n"
        "     {\"name\":\"<name>\",\"pattern\":\"<regex>\",\"severity\":\"<level>\"}]}\n"
        "\n"
        "  interrupt_autoboot  Flood a key to break into a bootloader (controller).\n"
        "    {\"type\":\"interrupt_autoboot\",\"id\":\"<id>\",\"key_b64\":\"<base64>\",\n"
        "     \"interval_ms\":<int>,\"duration_ms\":<int>,\"stop_pattern\":\"<regex>\",\n"
        "     \"reset_pin\":\"dtr|rts\",\"reset_assert\":\"set|clear\",\"reset_hold_ms\":<int>}\n"
        "\n"
        "  configure_autoresponder  Add/remove/clear standing expect->send rules.\n"
        "    {\"type\":\"configure_autoresponder\",\"id\":\"<id>\",\"action\":\"add|remove|clear\",\n"
        "     \"name\":\"<name>\",\"pattern\":\"<regex>\",\"response_b64\":\"<base64>\",\n"
        "     \"once\":<bool>,\"cooldown_ms\":<int>}\n"
        "\n"
        "  autoresponders_request  List configured autoresponder rules.\n"
        "    {\"type\":\"autoresponders_request\",\"id\":\"<id>\"}\n"
        "\n"    );
    printf(
        "BROKER -> CLIENT MESSAGES:\n"
        "\n"
        "  welcome           Handshake response (after hello).\n"
        "  output            Device data (base64-encoded, with timestamp).\n"
        "  input_echo        Echo of data sent by another client.\n"
        "  expect_result     Result of send_expect (matched bool + captured data).\n"
        "  status_response   Response to status request.\n"
        "  history_response  Response with output history chunks.\n"
        "  incidents_response Response with anomaly incidents.\n"
        "  anomaly           Anomaly detected (pattern name, severity, match).\n"
        "  autoboot_result   interrupt_autoboot finished (prompt match or timeout).\n"
        "  boot_stage        A configured boot stage was reached.\n"
        "  boot_stall        Boot progress stalled (no advance before terminal stage).\n"
        "  autoresponders_response  List of autoresponder rules.\n"
        "  autoresponder_fired      A standing rule matched and sent its response.\n"
        "  suspended         Serial port has been suspended.\n"
        "  resumed           Serial port has been resumed.\n"
        "  link_down         Device disconnected.\n"
        "  link_up           Device reconnected.\n"
        "  error             Error response (with id matching the request).\n"
        "\n"
        "EXAMPLE SESSION (with socat):\n"
        "\n"
        "  socat - UNIX-CONNECT:/tmp/smolmux-ttyUSB0.sock\n"
        "  {\"type\":\"hello\",\"name\":\"test\",\"role\":\"controller\",\"protocol_version\":1}\n"
        "  # broker responds with welcome\n"
        "  {\"type\":\"send_expect\",\"id\":\"1\",\"data\":\"dW5hbWUgLWEK\",\n"
        "   \"pattern\":\"\\\\$\\\\s*$\",\"timeout_ms\":5000}\n"
        "  # broker sends output messages, then expect_result\n"
        "\n"
        "  Data encoding: echo -n 'uname -a\\n' | base64  ->  dW5hbWUgLWEK\n"    );
}

static void do_list_ports(void)
{
    glob_t g;
    size_t total = sm_glob_serial_ports(&g);
    if (total == 0) {
        printf("No serial ports found.\n");
    } else {
        for (size_t i = 0; i < total; i++)
            printf("%s\n", g.gl_pathv[i]);
    }
    globfree(&g);
}

static void do_list_profiles(void)
{
    const char *home = getenv("HOME");
    int found = 0;

    /* Check env var */
    const char *env_path = getenv(SM_PROFILE_ENV);
    if (env_path && env_path[0]) {
        sm_device_profile_t p;
        sm_profile_init_default(&p);
        if (sm_profile_load(&p, env_path) == 0) {
            printf("%-20s %s  ($%s)\n", p.name, p.description, SM_PROFILE_ENV);
            found++;
        }
        sm_profile_destroy(&p);
    }

    /* Scan config dir */
    if (home) {
        char pattern[512];
        snprintf(pattern, sizeof(pattern), SM_PROFILE_CONFIG_DIR_FMT, home);
        size_t dir_len = strlen(pattern);
        snprintf(pattern + dir_len, sizeof(pattern) - dir_len,
                 "/*%s", SM_PROFILE_FILE_SUFFIX);

        glob_t g;
        memset(&g, 0, sizeof(g));
        if (glob(pattern, 0, NULL, &g) == 0) {
            for (size_t i = 0; i < g.gl_pathc; i++) {
                sm_device_profile_t p;
                sm_profile_init_default(&p);
                if (sm_profile_load(&p, g.gl_pathv[i]) == 0) {
                    printf("%-20s %s\n", p.name, p.description);
                    found++;
                }
                sm_profile_destroy(&p);
            }
        }
        globfree(&g);
    }

    /* Scan bundled profiles in configs/ */
    {
        glob_t g;
        memset(&g, 0, sizeof(g));
        if (glob("configs/*" SM_PROFILE_FILE_SUFFIX, 0, NULL, &g) == 0) {
            for (size_t i = 0; i < g.gl_pathc; i++) {
                sm_device_profile_t p;
                sm_profile_init_default(&p);
                if (sm_profile_load(&p, g.gl_pathv[i]) == 0) {
                    printf("%-20s %s  (bundled)\n", p.name, p.description);
                    found++;
                }
                sm_profile_destroy(&p);
            }
        }
        globfree(&g);
    }

    if (!found)
        printf("No device profiles found.\n"
               "Place profiles in ~/.config/smolmux/ with suffix %s\n",
               SM_PROFILE_FILE_SUFFIX);
}


/* Load serial profile: explicit -p and $SMOLMUX_DEVICE_PROFILE hard-fail if
 * unresolvable. No -p means built-in default only (never auto-pick first file). */
static int discover_profile(sm_device_profile_t *profile, const char *explicit_path)
{
    char resolved[512];

    if (explicit_path && explicit_path[0]) {
        if (sm_profile_resolve_path(explicit_path, SM_PROFILE_FILE_SUFFIX,
                                    resolved, sizeof(resolved)) != 0) {
            SM_LOG_ERROR("main", "failed to resolve profile '%s' "
                         "(path or short name under ~/.config/smolmux, "
                         "profiles/, configs/)", explicit_path);
            return -1;
        }
        if (sm_profile_load(profile, resolved) != 0) {
            SM_LOG_ERROR("main", "failed to load profile from %s", resolved);
            return -1;
        }
        SM_LOG_INFO("main", "loaded profile from %s", resolved);
        return 0;
    }

    const char *env_path = getenv(SM_PROFILE_ENV);
    if (env_path && env_path[0]) {
        if (sm_profile_resolve_path(env_path, SM_PROFILE_FILE_SUFFIX,
                                    resolved, sizeof(resolved)) != 0) {
            SM_LOG_ERROR("main", "failed to resolve $%s=%s", SM_PROFILE_ENV,
                         env_path);
            return -1;
        }
        if (sm_profile_load(profile, resolved) != 0) {
            SM_LOG_ERROR("main", "failed to load profile from $%s (%s)",
                         SM_PROFILE_ENV, resolved);
            return -1;
        }
        SM_LOG_INFO("main", "loaded profile from $%s=%s", SM_PROFILE_ENV,
                    resolved);
        return 0;
    }

    sm_profile_init_default(profile);
    SM_LOG_INFO("main", "using default device profile");
    return 0;
}

int main(int argc, char *argv[])
{
    int baud = SM_DEFAULT_BAUD;
    const char *port = NULL;
    char socket_path[108] = {0};
    char log_dir[256] = SM_LOG_DIR;
    char text_log_dir[256] = {0};
    const char *profile_path = NULL;
    int no_text_log = 0;
    int no_reconnect = 0;
    int verbose = 0;
    int enable_mcp = 0;
    int enable_gdb = 0;
    const char *gdb_path = NULL;
    const char *gdb_target = NULL;
    const char *serial_tcp_target = NULL;
    int tcp_port = 0;
    const char *tcp_bind = NULL;
    const char *auth_token = getenv("SMOLMUX_AUTH_TOKEN");
    const char *board = NULL;
    const char *role = NULL;
    int ws_port = 0;
    int do_list_ports_flag = 0;
    int do_list_profiles_flag = 0;
    int do_help_protocol = 0;

    /* Default text log dir: ~/.local/share/smolmux/logs */
    const char *home = getenv("HOME");
    if (home)
        snprintf(text_log_dir, sizeof(text_log_dir), SM_TEXT_LOG_DIR_FMT, home);

    enum {
        OPT_LIST_PORTS = 256,
        OPT_LIST_PROFILES,
        OPT_HELP_PROTOCOL,
        OPT_AUTH_TOKEN,
        OPT_BOARD,
        OPT_ROLE,
        OPT_SERIAL_TCP,
    };

    static const struct option long_opts[] = {
        {"baud",           required_argument, NULL, 'b'},
        {"socket",         required_argument, NULL, 's'},
        {"log-dir",        required_argument, NULL, 'l'},
        {"text-log-dir",   required_argument, NULL, 't'},
        {"profile",        required_argument, NULL, 'p'},
        {"gdb",            no_argument,       NULL, 'G'},
        {"gdb-path",       required_argument, NULL, 'g'},
        {"gdb-target",     required_argument, NULL, 'r'},
        {"serial-tcp",     required_argument, NULL, OPT_SERIAL_TCP},
        {"mcp",            no_argument,       NULL, 'M'},
        {"tcp-port",       required_argument, NULL, 'T'},
        {"tcp-bind",       required_argument, NULL, 'B'},
        {"auth-token",     required_argument, NULL, OPT_AUTH_TOKEN},
        {"board",          required_argument, NULL, OPT_BOARD},
        {"role",           required_argument, NULL, OPT_ROLE},
        {"ws-port",        required_argument, NULL, 'W'},
        {"no-text-log",    no_argument,       NULL, 'N'},
        {"no-reconnect",   no_argument,       NULL, 'R'},
        {"list-ports",     no_argument,       NULL, OPT_LIST_PORTS},
        {"list-profiles",  no_argument,       NULL, OPT_LIST_PROFILES},
        {"help-protocol",  no_argument,       NULL, OPT_HELP_PROTOCOL},
        {"verbose",        no_argument,       NULL, 'v'},
        {"version",        no_argument,       NULL, 'V'},
        {"help",           no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "b:s:l:t:p:vVh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'b': {
            char *endp;
            long val = strtol(optarg, &endp, 10);
            if (*endp || val <= 0 || val > 4000000) {
                fprintf(stderr, "Error: invalid baud rate '%s'\n", optarg);
                return 1;
            }
            baud = (int)val;
            break;
        }
        case 's': snprintf(socket_path, sizeof(socket_path), "%s", optarg); break;
        case 'l': snprintf(log_dir, sizeof(log_dir), "%s", optarg); break;
        case 't': snprintf(text_log_dir, sizeof(text_log_dir), "%s", optarg); break;
        case 'p': profile_path = optarg; break;
        case 'G': enable_gdb = 1; break;
        case 'g': gdb_path = optarg; break;
        case 'r': gdb_target = optarg; break;
        case OPT_SERIAL_TCP: serial_tcp_target = optarg; break;
        case 'M': enable_mcp = 1; break;
        case 'T': {
            char *endp;
            long val = strtol(optarg, &endp, 10);
            if (*endp || val <= 0 || val > 65535) {
                fprintf(stderr, "Error: invalid TCP port '%s'\n", optarg);
                return 1;
            }
            tcp_port = (int)val;
            break;
        }
        case 'B': tcp_bind = optarg; break;
        case OPT_BOARD: board = optarg; break;
        case OPT_ROLE:  role = optarg; break;
        case 'W': {
            char *endp;
            long val = strtol(optarg, &endp, 10);
            if (*endp || val <= 0 || val > 65535) {
                fprintf(stderr, "Error: invalid WebSocket port '%s'\n", optarg);
                return 1;
            }
            ws_port = (int)val;
            break;
        }
        case 'N': no_text_log = 1; break;
        case 'R': no_reconnect = 1; break;
        case OPT_LIST_PORTS: do_list_ports_flag = 1; break;
        case OPT_LIST_PROFILES: do_list_profiles_flag = 1; break;
        case OPT_HELP_PROTOCOL: do_help_protocol = 1; break;
        case OPT_AUTH_TOKEN: auth_token = optarg; break;
        case 'v': verbose = 1; break;
        case 'V':
            printf("%s %s\n", SM_NAME, SM_VERSION);
            return 0;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    /* Handle informational flags before requiring a port */
    if (do_list_ports_flag) {
        do_list_ports();
        return 0;
    }
    if (do_list_profiles_flag) {
        do_list_profiles();
        return 0;
    }
    if (do_help_protocol) {
        print_protocol_help();
        return 0;
    }

    /* Port is the first non-option argument */
    if (optind < argc)
        port = argv[optind];

    if (!port && !enable_gdb && !serial_tcp_target) {
        fprintf(stderr, "Error: serial port required (or use --gdb / --serial-tcp)\n"
                "  Use --list-ports to see available ports.\n\n");
        usage(argv[0]);
        return 1;
    }

    if (verbose)
        sm_logger_set_level(SM_LOG_DEBUG);

    /* Derive socket path if not specified (fits AF_UNIX + bind rename). */
    if (!socket_path[0]) {
        const char *label = port ? port
            : (serial_tcp_target ? serial_tcp_target : "gdb");
        if (sm_derive_socket_path(socket_path, sizeof(socket_path), label) != 0) {
            SM_LOG_ERROR("main", "cannot derive socket path for '%s'", label);
            return 1;
        }
    }

    SM_LOG_INFO("main", "%s %s starting", SM_NAME, SM_VERSION);

    /* Create link */
    sm_link_t *link = NULL;

#if SM_ENABLE_GDB
    if (enable_gdb) {
        link = sm_gdb_new(gdb_path, gdb_target);
        if (!link) {
            SM_LOG_ERROR("main", "failed to create GDB link");
            return 1;
        }
        SM_LOG_INFO("main", "gdb=%s target=%s socket=%s",
                    gdb_path ? gdb_path : "gdb",
                    gdb_target ? gdb_target : "(none)", socket_path);
    }
#else
    (void)enable_gdb;
    (void)gdb_path;
    (void)gdb_target;
#endif

#if SM_ENABLE_LINK_SERIAL_TCP
    if (!link && serial_tcp_target) {
        char host[240];
        int tport = SM_SERIAL_TCP_DEFAULT_PORT;
        sm_parse_host_port(serial_tcp_target, host, sizeof(host), &tport);
        link = sm_serial_tcp_new(host, tport);
        if (!link) {
            SM_LOG_ERROR("main", "failed to create serial-tcp link");
            return 1;
        }
        SM_LOG_INFO("main", "serial-tcp=%s:%d socket=%s", host, tport, socket_path);
    }
#else
    if (serial_tcp_target) {
        fprintf(stderr, "Error: --serial-tcp support not built in "
                "(enable SM_ENABLE_LINK_SERIAL_TCP)\n");
        return 1;
    }
#endif

    if (!link) {
        if (!port) {
            fprintf(stderr, "Error: serial port required\n\n");
            usage(argv[0]);
            return 1;
        }
        link = sm_uart_new(port, baud, 1);
        if (!link) {
            SM_LOG_ERROR("main", "failed to create UART link");
            return 1;
        }
        SM_LOG_INFO("main", "port=%s baud=%d socket=%s", port, baud,
                    socket_path);
    }

    /* Initialize broker */
    sm_broker_init(&broker, link, socket_path);
    snprintf(broker.port, sizeof(broker.port), "%s",
             port ? port : (serial_tcp_target ? serial_tcp_target : "gdb"));
    broker.baudrate = baud;
    if (board) snprintf(broker.board, sizeof(broker.board), "%s", board);
    if (role)  snprintf(broker.role, sizeof(broker.role), "%s", role);
    snprintf(broker.log_dir, sizeof(broker.log_dir), "%s", log_dir);
    snprintf(broker.text_log_dir, sizeof(broker.text_log_dir), "%s", text_log_dir);
    broker.no_text_log = no_text_log;
    broker.reconnect = !no_reconnect;
    if (auth_token)
        snprintf(broker.auth_token, sizeof(broker.auth_token), "%s", auth_token);

    /* Load device profile */
    if (discover_profile(&broker.profile, profile_path) != 0)
        return 1;
    sm_profile_apply_anomaly(&broker.profile, &broker.anomaly);
    sm_profile_apply_boot(&broker.profile, &broker.boot);
    SM_LOG_INFO("main", "profile: %s (%s)", broker.profile.name,
                broker.profile.device_type);

#if SM_ENABLE_SINK_MCP
    /* Enable MCP sink if requested */
    if (enable_mcp) {
        /* MCP speaks JSON-RPC on stdout; logging already goes to stderr. */
        sm_sink_t *mcp = sm_mcp_sink_new(&broker);
        if (!mcp) {
            SM_LOG_ERROR("main", "failed to create MCP sink");
            return 1;
        }
        sm_broker_add_sink(&broker, mcp);
        SM_LOG_INFO("main", "MCP stdio sink enabled");
    }
#else
    (void)enable_mcp;
#endif

#if SM_ENABLE_SINK_TCP
    if (tcp_port > 0) {
        sm_sink_t *tcp = sm_tcp_sink_new(tcp_port, tcp_bind);
        sm_broker_add_sink(&broker, tcp);
        SM_LOG_INFO("main", "TCP sink enabled on port %d", tcp_port);
        if (!broker.auth_token[0])
            SM_LOG_WARN("main",
                        "TCP sink has no auth token — any peer that can "
                        "reach the port gets full access. Set "
                        "SMOLMUX_AUTH_TOKEN or --auth-token.");
    }
#else
    (void)tcp_port;
    (void)tcp_bind;
#endif

#if SM_ENABLE_SINK_WS
    if (ws_port > 0) {
        sm_sink_t *wss = sm_ws_sink_new(ws_port);
        sm_broker_add_sink(&broker, wss);
        SM_LOG_INFO("main", "WebSocket sink enabled on port %d", ws_port);
    }
#else
    (void)ws_port;
#endif

    /* Set up signal handling */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

#if SM_ENABLE_GDB
    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = sigchld_handler;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa_chld, NULL);
#endif

    /* Ignore SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    /* Run */
    int rc = sm_broker_run(&broker);
    /* Setup failures (rc<0) on a UART port: name the holder if one exists. */
    if (rc < 0 && port && !enable_gdb && !serial_tcp_target)
        diagnose_busy_port(port);
    sm_broker_destroy(&broker);

    SM_LOG_INFO("main", "exiting (rc=%d)", rc);
    return rc < 0 ? 1 : 0;
}
