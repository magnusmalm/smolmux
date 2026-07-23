/*
 * smolmux-watcher — autonomous anomaly incident reporter
 *
 * Connects to a running smolmux broker as an observer, watches for anomaly
 * messages, fetches output history, and writes markdown incident reports
 * to disk.
 *
 * State machine (one pending anomaly at a time):
 *   1. Receive anomaly → store in pending, send history_request
 *   2. Receive history_response with matching ID → generate report, clear
 *   3. Timeout (5s) → generate report without history, clear
 */

#include "constants.h"
#include "protocol.h"
#include "util/base64.h"
#include "util/json_helpers.h"
#include "util/sock_util.h"

#include <errno.h>
#include <getopt.h>
#include <stdarg.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* --- Global state --- */

/* Read buffer grows on demand: a history_response can reach ~700 KB base64
 * (large --context-bytes) and the broker can coalesce output past 16 KB, so a
 * fixed buffer would fill without a newline, read 0 bytes, and be misread as a
 * disconnect. */
#define WATCHER_READ_BUF_INIT (64 * 1024)
#define WATCHER_READ_BUF_MAX  (4 * 1024 * 1024)

static struct {
    int sock_fd;
    volatile sig_atomic_t running;
    int verbose;
    char output_dir[256];
    int context_bytes;
    char *read_buf;
    size_t read_len;
    size_t read_cap;
    int incident_count;
    /* Pending anomaly state */
    int pending;
    cJSON *pending_anomaly;   /* owned — cJSON_Delete when done */
    char pending_id[64];      /* history request ID to correlate */
    time_t pending_time;      /* monotonic time of request */
} watcher;

/* --- Logging --- */

static void log_info(const char *fmt, ...)
{
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);

    fprintf(stderr, "%s [INFO] ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void log_debug(const char *fmt, ...)
{
    if (!watcher.verbose)
        return;

    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);

    fprintf(stderr, "%s [DEBUG] ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void log_warn(const char *fmt, ...)
{
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);

    fprintf(stderr, "%s [WARN] ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
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

    int rc = sm_write_all(watcher.sock_fd, line, len);
    free(line);
    cJSON_Delete(msg);
    return rc;
}

/* --- Report generation --- */

/* Sanitize a string for use as a filename component.
 * Replaces non-alphanumeric chars (except .-_) with underscore.
 * Collapses consecutive dots to prevent ".." path traversal. */
void watcher_sanitize_filename(const char *src, char *dst, size_t dst_len)
{
    size_t j = 0;
    size_t limit = dst_len - 1;
    if (limit > 128)
        limit = 128;

    for (size_t i = 0; src[i] && j < limit; i++) {
        char c = src[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_') {
            dst[j++] = c;
        } else if (c == '.') {
            /* Allow dots but collapse consecutive dots to prevent ".." */
            if (j > 0 && dst[j - 1] == '.')
                dst[j - 1] = '_';  /* replace previous dot */
            dst[j++] = '.';
        } else {
            dst[j++] = '_';
        }
    }
    /* Don't end with a dot (could combine with extension dot) */
    if (j > 0 && dst[j - 1] == '.')
        dst[j - 1] = '_';
    dst[j] = '\0';
}

/* Generate a sortable filename for an incident report.
 * Format: YYYYMMDD-HHMMSS-pattern-incident_id.md */
void watcher_make_report_filename(const cJSON *anomaly, char *out, size_t out_len)
{
    double ts = sm_json_get_double(anomaly, "timestamp", 0.0);
    const char *pattern = sm_json_get_string(anomaly, "pattern_name");
    const char *incident_id = sm_json_get_string(anomaly, "incident_id");

    char ts_part[32];
    if (ts > 0) {
        time_t t = (time_t)ts;
        struct tm tm;
        gmtime_r(&t, &tm);
        strftime(ts_part, sizeof(ts_part), "%Y%m%d-%H%M%S", &tm);
    } else {
        snprintf(ts_part, sizeof(ts_part), "00000000-000000");
    }

    char safe_pattern[130];
    watcher_sanitize_filename(pattern ? pattern : "unknown", safe_pattern, sizeof(safe_pattern));

    char safe_id[130];
    watcher_sanitize_filename(incident_id ? incident_id : "unknown", safe_id, sizeof(safe_id));

    snprintf(out, out_len, "%s-%s-%s.md", ts_part, safe_pattern, safe_id);
}

/* Format a timestamp as ISO 8601 UTC. */
static void format_timestamp(double ts, char *out, size_t out_len)
{
    if (ts <= 0) {
        snprintf(out, out_len, "unknown");
        return;
    }
    time_t t = (time_t)ts;
    struct tm tm;
    gmtime_r(&t, &tm);
    int usec = (int)((ts - (double)t) * 1000000.0);
    snprintf(out, out_len, "%04d-%02d-%02dT%02d:%02d:%02d.%06dZ",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, usec);
}

/* Format a short timestamp (HH:MM:SS.mmm) for history chunks. */
static void format_chunk_timestamp(double ts, char *out, size_t out_len)
{
    if (ts <= 0) {
        snprintf(out, out_len, "?");
        return;
    }
    time_t t = (time_t)ts;
    struct tm tm;
    gmtime_r(&t, &tm);
    int ms = (int)((ts - (double)t) * 1000.0);
    snprintf(out, out_len, "%02d:%02d:%02d.%03d",
             tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
}

/* Generate a markdown incident report.
 * Returns malloc'd string. Caller frees.
 * anomaly: the anomaly cJSON message
 * chunks: cJSON array of history chunks (may be NULL) */
char *watcher_format_report(const cJSON *anomaly, const cJSON *chunks)
{
    /* Build report into a dynamically grown buffer */
    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf)
        return NULL;

#define APPEND(...) do { \
    int _n; \
    while ((_n = snprintf(buf + len, cap - len, __VA_ARGS__)) >= (int)(cap - len)) { \
        cap *= 2; \
        char *_nb = realloc(buf, cap); \
        if (!_nb) { free(buf); return NULL; } \
        buf = _nb; \
    } \
    len += (size_t)_n; \
} while(0)

    const char *pattern = sm_json_get_string(anomaly, "pattern_name");
    const char *incident_id = sm_json_get_string(anomaly, "incident_id");
    const char *severity = sm_json_get_string(anomaly, "severity");
    const char *match_text = sm_json_get_string(anomaly, "match_text");
    const char *pre_context = sm_json_get_string(anomaly, "pre_context");
    double ts = sm_json_get_double(anomaly, "timestamp", 0.0);

    char ts_str[64];
    format_timestamp(ts, ts_str, sizeof(ts_str));

    /* Header */
    APPEND("# Incident: %s\n\n", pattern ? pattern : "unknown");

    /* Metadata table */
    APPEND("| Field | Value |\n");
    APPEND("|-------|-------|\n");
    APPEND("| Incident ID | `%s` |\n", incident_id ? incident_id : "");
    APPEND("| Pattern | %s |\n", pattern ? pattern : "");
    APPEND("| Severity | %s |\n", severity ? severity : "");
    APPEND("| Timestamp | %s |\n", ts_str);
    APPEND("| Match | %s |\n\n", match_text ? match_text : "");

    /* Pre-context */
    if (pre_context && pre_context[0]) {
        APPEND("## Pre-context\n\n");
        APPEND("```\n%s\n```\n\n", pre_context);
    }

    /* Output history */
    APPEND("## Output History\n\n");

    int chunk_count = chunks ? cJSON_GetArraySize(chunks) : 0;
    if (chunk_count > 0) {
        const cJSON *chunk;
        cJSON_ArrayForEach(chunk, chunks) {
            double chunk_ts = sm_json_get_double(chunk, "timestamp", 0.0);
            char chunk_ts_str[32];
            format_chunk_timestamp(chunk_ts, chunk_ts_str, sizeof(chunk_ts_str));

            const char *b64 = sm_json_get_string(chunk, "data");
            if (b64) {
                size_t dec_len;
                uint8_t *data = sm_base64_decode(b64, strlen(b64), &dec_len);
                if (data) {
                    APPEND("**[%s]**\n```\n", chunk_ts_str);
                    /* Append decoded data, replacing NUL bytes */
                    for (size_t i = 0; i < dec_len; i++) {
                        char c = (char)data[i];
                        if (c == '\0') c = ' ';
                        /* Ensure buffer space */
                        if (len + 2 >= cap) {
                            cap *= 2;
                            char *nb = realloc(buf, cap);
                            if (!nb) { free(data); free(buf); return NULL; }
                            buf = nb;
                        }
                        buf[len++] = c;
                    }
                    APPEND("\n```\n\n");
                    free(data);
                }
            }
        }
    } else {
        APPEND("(no history available)\n\n");
    }

#undef APPEND

    buf[len] = '\0';
    return buf;
}

/* --- Anomaly / history handling --- */

static void clear_pending(void)
{
    if (watcher.pending_anomaly) {
        cJSON_Delete(watcher.pending_anomaly);
        watcher.pending_anomaly = NULL;
    }
    watcher.pending = 0;
    watcher.pending_id[0] = '\0';
    watcher.pending_time = 0;
}

static void write_report(const cJSON *anomaly, const cJSON *chunks)
{
    char *report = watcher_format_report(anomaly, chunks);
    if (!report) {
        log_warn("Failed to format report");
        return;
    }

    char filename[512];
    watcher_make_report_filename(anomaly, filename, sizeof(filename));

    char filepath[768];
    snprintf(filepath, sizeof(filepath), "%s/%s", watcher.output_dir, filename);

    FILE *f = fopen(filepath, "w");
    if (!f) {
        log_warn("Failed to write report %s: %s", filepath, strerror(errno));
        free(report);
        return;
    }
    fputs(report, f);
    fclose(f);
    free(report);

    watcher.incident_count++;
    log_info("Report written: %s (%d total)", filename, watcher.incident_count);
}

/* Generate a simple unique ID for history requests. */
static void make_request_id(char *out, size_t out_len)
{
    static int seq = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    snprintf(out, out_len, "w-%ld-%d", (long)ts.tv_nsec, seq++);
}

static void handle_anomaly(cJSON *root)
{
    const char *pattern = sm_json_get_string(root, "pattern_name");
    const char *severity = sm_json_get_string(root, "severity");
    log_info("Anomaly: %s [%s]", pattern ? pattern : "?", severity ? severity : "?");

    /* If there's already a pending anomaly, flush it without history */
    if (watcher.pending) {
        log_debug("Flushing previous pending anomaly without history");
        write_report(watcher.pending_anomaly, NULL);
        clear_pending();
    }

    /* Store anomaly and send history request */
    watcher.pending_anomaly = cJSON_Duplicate(root, 1);
    watcher.pending = 1;

    make_request_id(watcher.pending_id, sizeof(watcher.pending_id));
    watcher.pending_time = time(NULL);

    log_debug("Requesting history (id=%s, bytes=%d)",
              watcher.pending_id, watcher.context_bytes);
    send_msg(sm_msg_history_request(watcher.pending_id, 0.0, watcher.context_bytes));
}

static void handle_history_response(cJSON *root)
{
    const char *id = sm_json_get_string(root, "id");

    if (!watcher.pending || !id || strcmp(id, watcher.pending_id) != 0) {
        log_debug("Ignoring unmatched history_response (id=%s)", id ? id : "null");
        return;
    }

    cJSON *chunks = cJSON_GetObjectItem(root, "chunks");
    write_report(watcher.pending_anomaly, chunks);
    clear_pending();
}

/* --- Broker message handling --- */

static void dispatch_message(sm_msg_t *msg)
{
    switch (msg->type) {
    case SM_MSG_ANOMALY:
        handle_anomaly(msg->root);
        break;
    case SM_MSG_HISTORY_RESPONSE:
        handle_history_response(msg->root);
        break;
    case SM_MSG_WELCOME: {
        const char *port = sm_json_get_string(msg->root, "port");
        int baud = sm_json_get_int(msg->root, "baud", 0);
        log_info("Connected to broker: %s @ %d baud", port ? port : "?", baud);
        break;
    }
    case SM_MSG_ERROR: {
        const char *errmsg = sm_json_get_string(msg->root, "message");
        log_warn("Broker error: %s", errmsg ? errmsg : "unknown");
        break;
    }
    default:
        break;
    }
}

static int handle_broker_data(void)
{
    /* Grow rather than stall at 0 read space (which looks like a disconnect)
     * when the buffer is full of a newline-less line, bounded by
     * WATCHER_READ_BUF_MAX. */
    if (watcher.read_len + 1 >= watcher.read_cap) {
        size_t new_cap = watcher.read_cap ? watcher.read_cap * 2
                                          : WATCHER_READ_BUF_INIT;
        if (new_cap > WATCHER_READ_BUF_MAX) {
            if (watcher.read_cap >= WATCHER_READ_BUF_MAX) {
                fprintf(stderr, "smolmux-watcher: response line exceeds %d bytes\n",
                        WATCHER_READ_BUF_MAX);
                return -1;
            }
            new_cap = WATCHER_READ_BUF_MAX;
        }
        char *nb = realloc(watcher.read_buf, new_cap);
        if (!nb)
            return -1;
        watcher.read_buf = nb;
        watcher.read_cap = new_cap;
    }

    ssize_t n = read(watcher.sock_fd,
                     watcher.read_buf + watcher.read_len,
                     watcher.read_cap - watcher.read_len - 1);
    if (n <= 0)
        return -1;

    watcher.read_len += (size_t)n;
    watcher.read_buf[watcher.read_len] = '\0';

    char *start = watcher.read_buf;
    char *nl;
    while ((nl = memchr(start, '\n',
                        watcher.read_len - (size_t)(start - watcher.read_buf))) != NULL) {
        size_t line_len = (size_t)(nl - start);
        sm_msg_t msg = sm_msg_decode(start, line_len);
        if (msg.root)
            dispatch_message(&msg);
        sm_msg_free(&msg);
        start = nl + 1;
    }

    size_t remaining = watcher.read_len - (size_t)(start - watcher.read_buf);
    if (remaining > 0 && start != watcher.read_buf)
        memmove(watcher.read_buf, start, remaining);
    watcher.read_len = remaining;

    return 0;
}

/* Test hooks (mirrors cli_test_*): drive handle_broker_data against a
 * caller-supplied fd and inspect the grown buffer capacity. */
int watcher_test_feed(int fd, size_t *cap_out)
{
    watcher.sock_fd = fd;
    int rc = handle_broker_data();
    if (cap_out) *cap_out = watcher.read_cap;
    return rc;
}

void watcher_test_reset(void)
{
    free(watcher.read_buf);
    watcher.read_buf = NULL;
    watcher.read_len = 0;
    watcher.read_cap = 0;
    watcher.sock_fd = -1;
}

/* --- Check for pending anomaly timeout --- */

static void check_pending_timeout(void)
{
    if (!watcher.pending)
        return;

    time_t now = time(NULL);
    if (now - watcher.pending_time >= SM_WATCHER_HISTORY_TIMEOUT_S) {
        log_debug("History request timed out — writing report without history");
        write_report(watcher.pending_anomaly, NULL);
        clear_pending();
    }
}

/* --- Signal handling --- */

static void signal_handler(int sig)
{
    (void)sig;
    watcher.running = 0;
}

/* --- Main --- */

static void usage(const char *prog)
{
    fprintf(stderr,
        "%s-watcher — autonomous anomaly incident reporter\n"
        "\n"
        "Connects to a running smolmux broker as an observer, watches for\n"
        "anomaly events (kernel panics, crashes, OOMs), and saves markdown\n"
        "incident reports with output history to disk.\n"
        "\n"
        "USAGE:\n"
        "  %s [options] [socket_path]\n"
        "\n"
        "OPTIONS:\n"
        "  -s, --socket <path>       Broker socket path (auto-discover if omitted)\n"
        "  -o, --output-dir <dir>    Report directory (default: %s)\n"
        "      --context-bytes <n>   History bytes per incident (default: %d)\n"
        "  -v, --verbose             Debug logging\n"
        "  -V, --version             Show version\n"
        "  -h, --help                Show help\n"
        "\n"
        "EXAMPLES:\n"
        "  %s                                        # Auto-discover, default output dir\n"
        "  %s -o /var/log/smolmux-incidents           # Custom output directory\n"
        "  %s -s /tmp/smolmux-ttyUSB0.sock            # Explicit socket path\n"
        "  %s --context-bytes 16384 -v                 # More context, debug logging\n"
        "\n"
        "BEHAVIOR:\n"
        "  - Auto-reconnects to the broker with exponential backoff\n"
        "  - Fetches output history for each incident (timeout: 5s)\n"
        "  - Reports are saved as YYYYMMDD-HHMMSS-pattern-id.md\n"
        "\n"
        "SOCKET DISCOVERY:\n"
        "  1. -s flag or positional argument\n"
        "  2. $SMOLMUX_SOCKET environment variable\n"
        "  3. Glob $XDG_RUNTIME_DIR/smolmux-*.sock\n"
        "  4. Glob /tmp/smolmux-*.sock\n"
        "\n"
        "ENVIRONMENT:\n"
        "  SMOLMUX_SOCKET    Override broker socket path\n",
        SM_NAME, prog,
        SM_WATCHER_DEFAULT_OUTPUT_DIR,
        SM_WATCHER_DEFAULT_CONTEXT_BYTES,
        prog, prog, prog, prog);
}

int main(int argc, char *argv[])
{
    const char *socket_path = NULL;

    memset(&watcher, 0, sizeof(watcher));
    watcher.sock_fd = -1;
    watcher.running = 1;
    watcher.context_bytes = SM_WATCHER_DEFAULT_CONTEXT_BYTES;
    snprintf(watcher.output_dir, sizeof(watcher.output_dir), "%s",
             SM_WATCHER_DEFAULT_OUTPUT_DIR);

    static const struct option long_opts[] = {
        {"socket",        required_argument, NULL, 's'},
        {"output-dir",    required_argument, NULL, 'o'},
        {"context-bytes", required_argument, NULL, 'B'},
        {"verbose",       no_argument,       NULL, 'v'},
        {"version",       no_argument,       NULL, 'V'},
        {"help",          no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "s:o:vVh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 's':
            socket_path = optarg;
            break;
        case 'o':
            snprintf(watcher.output_dir, sizeof(watcher.output_dir), "%s", optarg);
            break;
        case 'B':
            watcher.context_bytes = atoi(optarg);
            if (watcher.context_bytes <= 0)
                watcher.context_bytes = SM_WATCHER_DEFAULT_CONTEXT_BYTES;
            break;
        case 'v':
            watcher.verbose = 1;
            break;
        case 'V':
            printf("%s-watcher %s\n", SM_NAME, SM_VERSION);
            return 0;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    /* Socket path: -s flag > positional arg > env > glob */
    char discovered_path[108];
    if (!socket_path && optind < argc)
        socket_path = argv[optind];
    if (!socket_path) {
        if (sm_discover_socket(discovered_path, sizeof(discovered_path)) == 0) {
            socket_path = discovered_path;
        } else {
            fprintf(stderr, "Error: no broker socket found\n"
                    "  Specify -s path, set %s, or start a broker\n", SM_SOCKET_ENV);
            return 1;
        }
    }

    /* Create output directory */
    mkdir(watcher.output_dir, 0755);

    /* Set up signal handling */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    log_info("Watcher starting — output: %s, context: %d bytes",
             watcher.output_dir, watcher.context_bytes);

    /* Main loop with auto-reconnect */
    int retry_delay = SM_WATCHER_RECONNECT_BASE_S;

    while (watcher.running) {
        /* Connect */
        watcher.sock_fd = sm_connect_unix(socket_path);
        if (watcher.sock_fd < 0) {
            log_warn("Connection failed: %s — retrying in %ds",
                     strerror(errno), retry_delay);
            for (int ms = 0; ms < retry_delay * 1000 && watcher.running; ms += 100) {
                struct timespec ts = {0, 100000000L};  /* 100ms */
                nanosleep(&ts, NULL);
            }
            retry_delay *= 2;
            if (retry_delay > SM_WATCHER_RECONNECT_MAX_S)
                retry_delay = SM_WATCHER_RECONNECT_MAX_S;
            continue;
        }

        /* Reset retry on successful connect */
        retry_delay = SM_WATCHER_RECONNECT_BASE_S;
        watcher.read_len = 0;
        clear_pending();

        /* Send hello as observer */
        send_msg(sm_msg_hello("watcher", "observer"));

        /* Read welcome */
        handle_broker_data();

        /* Event loop */
        struct pollfd fds[1] = {
            { .fd = watcher.sock_fd, .events = POLLIN },
        };

        while (watcher.running) {
            int ret = poll(fds, 1, 1000);  /* 1s timeout for pending check */
            if (ret < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }

            if (ret > 0 && (fds[0].revents & POLLIN)) {
                if (handle_broker_data() < 0) {
                    log_warn("Broker disconnected");
                    break;
                }
            }
            if (ret > 0 && (fds[0].revents & (POLLHUP | POLLERR))) {
                log_warn("Broker disconnected");
                break;
            }

            check_pending_timeout();
        }

        close(watcher.sock_fd);
        watcher.sock_fd = -1;
        clear_pending();

        if (watcher.running) {
            log_info("Reconnecting in %ds...", retry_delay);
            for (int ms = 0; ms < retry_delay * 1000 && watcher.running; ms += 100) {
                struct timespec ts = {0, 100000000L};  /* 100ms */
                nanosleep(&ts, NULL);
            }
            retry_delay *= 2;
            if (retry_delay > SM_WATCHER_RECONNECT_MAX_S)
                retry_delay = SM_WATCHER_RECONNECT_MAX_S;
        }
    }

    log_info("Watcher stopped — %d incidents", watcher.incident_count);
    return 0;
}
