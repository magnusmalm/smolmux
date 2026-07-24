/*
 * smolmux-cli — command-line client for interacting with a running smolmux broker
 *
 * Connects to a running smolmux broker over its Unix socket and provides
 * subcommands that map to broker operations. Designed to be easily used
 * by both humans and LLMs with shell access.
 *
 * Usage: smolmux-cli [options] <command> [args...]
 */

#include "constants.h"
#include "protocol.h"
#include "broker_info.h"
#include "board_manifest.h"
#include "util/base64.h"
#include "util/json_helpers.h"
#include "util/sock_util.h"
#include "util/str.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <glob.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* --- Global state --- */

/* Read buffer grows on demand: a single response line can exceed 64 KB
 * (a history_response is up to SM_HISTORY_ENCODE_MAX_CHUNKS *
 * SM_HISTORY_ENCODE_MAX_BYTES raw ≈ 128 KB, ~170 KB once base64+JSON), and a
 * fixed buffer would leave 0 read space and be misread as a disconnect. */
#define CLI_READ_BUF_INIT (64 * 1024)
#define CLI_READ_BUF_MAX  (8 * 1024 * 1024)

static struct {
    int sock_fd;
    char *read_buf;
    size_t read_len;
    size_t read_cap;
    int json_output;
    int timeout_ms;
    int verbose;
    volatile sig_atomic_t interrupted;
} cli;

static void signal_handler(int sig)
{
    (void)sig;
    cli.interrupted = 1;
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

    int rc = sm_write_all(cli.sock_fd, line, len);
    free(line);
    cJSON_Delete(msg);
    return rc;
}

/* --- Read buffer / message parsing --- */

/* Read incoming data and parse complete JSON lines.
 * Calls handler for each decoded message. Returns -1 on disconnect. */
static int read_messages(void (*handler)(sm_msg_t *msg, void *ctx), void *ctx)
{
    /* Ensure there is room to read. Grow (rather than stall at 0 bytes and
     * look like a disconnect) when the buffer is full of an unterminated
     * line, bounded by CLI_READ_BUF_MAX. */
    if (cli.read_len + 1 >= cli.read_cap) {
        size_t new_cap = cli.read_cap ? cli.read_cap * 2 : CLI_READ_BUF_INIT;
        if (new_cap > CLI_READ_BUF_MAX) {
            if (cli.read_cap >= CLI_READ_BUF_MAX) {
                fprintf(stderr, "smolmux-cli: response line exceeds %d bytes\n",
                        CLI_READ_BUF_MAX);
                return -1;
            }
            new_cap = CLI_READ_BUF_MAX;
        }
        char *nb = realloc(cli.read_buf, new_cap);
        if (!nb) return -1;
        cli.read_buf = nb;
        cli.read_cap = new_cap;
    }

    ssize_t n = read(cli.sock_fd,
                     cli.read_buf + cli.read_len,
                     cli.read_cap - cli.read_len - 1);
    if (n <= 0) return -1;

    cli.read_len += (size_t)n;
    cli.read_buf[cli.read_len] = '\0';

    char *start = cli.read_buf;
    char *nl;
    while ((nl = memchr(start, '\n',
                        cli.read_len - (size_t)(start - cli.read_buf))) != NULL) {
        size_t line_len = (size_t)(nl - start);
        sm_msg_t msg = sm_msg_decode(start, line_len);
        if (msg.root && handler)
            handler(&msg, ctx);
        sm_msg_free(&msg);
        start = nl + 1;
    }

    size_t remaining = cli.read_len - (size_t)(start - cli.read_buf);
    if (remaining > 0 && start != cli.read_buf)
        memmove(cli.read_buf, start, remaining);
    cli.read_len = remaining;

    return 0;
}

/* Test hooks (mirrors sm_broker_test_*): drive read_messages against a
 * caller-supplied fd and inspect the grown buffer capacity. */
int cli_test_read_messages(int fd, void (*handler)(sm_msg_t *msg, void *ctx),
                           void *ctx, size_t *cap_out)
{
    cli.sock_fd = fd;
    int rc = read_messages(handler, ctx);
    if (cap_out) *cap_out = cli.read_cap;
    return rc;
}

void cli_test_reset(void)
{
    free(cli.read_buf);
    cli.read_buf = NULL;
    cli.read_len = 0;
    cli.read_cap = 0;
    cli.sock_fd = -1;
}

/* --- Synchronous response wait --- */

typedef struct {
    sm_msg_type_t expected_type;
    const char *expected_id;
    cJSON *result;           /* captured response root (caller must cJSON_Delete) */
    sm_strbuf_t *output_buf; /* if non-NULL, accumulate output data here */
    int done;
    int got_error;
    char error_msg[256];
} wait_ctx_t;

static void wait_handler(sm_msg_t *msg, void *ctx)
{
    wait_ctx_t *w = ctx;

    /* Accumulate output messages if requested */
    if (msg->type == SM_MSG_OUTPUT && w->output_buf) {
        const char *b64 = sm_json_get_string(msg->root, "data");
        if (b64) {
            size_t dec_len;
            uint8_t *data = sm_base64_decode(b64, strlen(b64), &dec_len);
            if (data && dec_len > 0)
                sm_strbuf_append(w->output_buf, (const char *)data, dec_len);
            free(data);
        }
        return;
    }

    /* Check for error response matching our id */
    if (msg->type == SM_MSG_ERROR) {
        const char *id = sm_json_get_string(msg->root, "id");
        if (w->expected_id && id && strcmp(id, w->expected_id) == 0) {
            const char *m = sm_json_get_string(msg->root, "message");
            snprintf(w->error_msg, sizeof(w->error_msg), "%s", m ? m : "unknown error");
            w->got_error = 1;
            w->done = 1;
            return;
        }
        /* Error with empty id might also be for us */
        if (id && id[0] == '\0') {
            const char *m = sm_json_get_string(msg->root, "message");
            snprintf(w->error_msg, sizeof(w->error_msg), "%s", m ? m : "unknown error");
            w->got_error = 1;
            w->done = 1;
            return;
        }
    }

    /* Check for expected response type */
    if (msg->type == w->expected_type) {
        const char *id = sm_json_get_string(msg->root, "id");
        /* Match by id if we have one, otherwise accept any of the right type */
        if (!w->expected_id || !id || strcmp(id, w->expected_id) == 0) {
            w->result = cJSON_Duplicate(msg->root, 1);
            w->done = 1;
            return;
        }
    }

    /* For broadcast messages like suspended/resumed, match by type alone */
    if (msg->type == SM_MSG_SUSPENDED || msg->type == SM_MSG_RESUMED) {
        if (msg->type == w->expected_type) {
            w->result = cJSON_Duplicate(msg->root, 1);
            w->done = 1;
            return;
        }
    }

    /* Print anomaly/link events to stderr in verbose mode */
    if (cli.verbose) {
        if (msg->type == SM_MSG_ANOMALY) {
            const char *pat = sm_json_get_string(msg->root, "pattern_name");
            fprintf(stderr, "[anomaly] %s\n", pat ? pat : "?");
        } else if (msg->type == SM_MSG_LINK_DOWN) {
            fprintf(stderr, "[link_down]\n");
        } else if (msg->type == SM_MSG_LINK_UP) {
            fprintf(stderr, "[link_up]\n");
        }
    }
}

static int wait_for_response(sm_msg_type_t expected_type, const char *expected_id,
                             int timeout_ms, cJSON **out_root,
                             sm_strbuf_t *output_buf)
{
    wait_ctx_t w = {
        .expected_type = expected_type,
        .expected_id = expected_id,
        .result = NULL,
        .output_buf = output_buf,
        .done = 0,
        .got_error = 0,
    };
    w.error_msg[0] = '\0';

    struct pollfd fds[1] = {{ .fd = cli.sock_fd, .events = POLLIN }};

    int remaining_ms = timeout_ms;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (!w.done && !cli.interrupted) {
        int ret = poll(fds, 1, remaining_ms > 0 ? remaining_ms : 100);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (ret > 0 && (fds[0].revents & POLLIN)) {
            if (read_messages(wait_handler, &w) < 0) {
                fprintf(stderr, "error: broker disconnected\n");
                return -1;
            }
        }
        if (ret > 0 && (fds[0].revents & (POLLHUP | POLLERR))) {
            fprintf(stderr, "error: broker disconnected\n");
            return -1;
        }

        /* Update remaining time */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int elapsed_ms = (int)((now.tv_sec - start.tv_sec) * 1000 +
                               (now.tv_nsec - start.tv_nsec) / 1000000);
        remaining_ms = timeout_ms - elapsed_ms;

        if (remaining_ms <= 0 && timeout_ms > 0) {
            fprintf(stderr, "error: timeout waiting for response\n");
            return -2;
        }
    }

    if (w.got_error) {
        fprintf(stderr, "error: %s\n", w.error_msg);
        if (w.result) cJSON_Delete(w.result);
        return -3;
    }

    if (w.done && out_root)
        *out_root = w.result;
    else if (w.result)
        cJSON_Delete(w.result);

    return w.done ? 0 : -1;
}

/* --- Connection and handshake --- */

static int connect_and_hello(const char *socket_path, const char *role)
{
    cli.sock_fd = sm_connect_unix(socket_path);
    if (cli.sock_fd < 0) {
        fprintf(stderr, "error: cannot connect to %s: %s\n",
                socket_path, strerror(errno));
        return -1;
    }

    /* Send hello */
    send_msg(sm_msg_hello("smolmux-cli", role));

    /* Wait for welcome */
    cJSON *welcome = NULL;
    int rc = wait_for_response(SM_MSG_WELCOME, NULL, 5000, &welcome, NULL);
    if (rc != 0) {
        fprintf(stderr, "error: handshake failed\n");
        close(cli.sock_fd);
        return -1;
    }

    if (cli.verbose) {
        const char *port = sm_json_get_string(welcome, "port");
        int baud = sm_json_get_int(welcome, "baud", 0);
        const char *your_role = sm_json_get_string(welcome, "your_role");
        fprintf(stderr, "connected: port=%s baud=%d role=%s\n",
                port ? port : "?", baud, your_role ? your_role : "?");
    }

    cJSON_Delete(welcome);
    return 0;
}

/* --- Subcommand implementations --- */

static int cmd_send(int argc, char **argv)
{
    const char *expect_pattern = NULL;
    int timeout = cli.timeout_ms > 0 ? cli.timeout_ms : 5000;

    static const struct option opts[] = {
        {"expect",  required_argument, NULL, 'e'},
        {"timeout", required_argument, NULL, 't'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    optind = 1;
    while ((opt = getopt_long(argc, argv, "e:t:", opts, NULL)) != -1) {
        switch (opt) {
        case 'e': expect_pattern = optarg; break;
        case 't': timeout = atoi(optarg); break;
        default: break;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "error: missing command argument\n"
                "usage: smolmux-cli send [--expect <pattern>] [--timeout <ms>] <command>\n");
        return 1;
    }

    /* Join remaining args as the command */
    sm_strbuf_t cmd;
    sm_strbuf_init(&cmd);
    for (int i = optind; i < argc; i++) {
        if (i > optind) sm_strbuf_append(&cmd, " ", 1);
        sm_strbuf_append_str(&cmd, argv[i]);
    }
    sm_strbuf_append(&cmd, "\n", 1);

    /* Build pattern — default to common shell prompts if not specified */
    if (!expect_pattern)
        expect_pattern = "\\$\\s*$|\\#\\s*$|>\\s*$";

    /* Send send_expect */
    cJSON *msg = sm_msg_send_expect("cli-send",
                                     (const uint8_t *)cmd.data, cmd.len,
                                     expect_pattern, timeout);
    sm_strbuf_destroy(&cmd);

    send_msg(msg);

    /* Wait for expect_result, accumulating output */
    sm_strbuf_t output;
    sm_strbuf_init(&output);

    cJSON *result = NULL;
    int rc = wait_for_response(SM_MSG_EXPECT_RESULT, "cli-send",
                               timeout + 2000, &result, &output);

    if (rc == 0 && result) {
        if (cli.json_output) {
            /* In JSON mode, include match info and output */
            cJSON *out = cJSON_CreateObject();
            int matched = sm_json_get_bool(result, "matched", 0);
            cJSON_AddBoolToObject(out, "matched", matched);
            cJSON_AddStringToObject(out, "output", output.data ? output.data : "");
            char *s = cJSON_PrintUnformatted(out);
            if (s) { printf("%s\n", s); free(s); }
            cJSON_Delete(out);
        } else {
            /* Print accumulated output */
            if (output.data && output.len > 0)
                fwrite(output.data, 1, output.len, stdout);
            int matched = sm_json_get_bool(result, "matched", 0);
            if (!matched)
                fprintf(stderr, "[timeout — pattern not matched]\n");
        }
        cJSON_Delete(result);
    }

    sm_strbuf_destroy(&output);
    return rc == 0 ? 0 : 1;
}

static int cmd_read(int argc, char **argv)
{
    int last_bytes = 8192;

    static const struct option opts[] = {
        {"bytes", required_argument, NULL, 'n'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    optind = 1;
    while ((opt = getopt_long(argc, argv, "n:", opts, NULL)) != -1) {
        switch (opt) {
        case 'n': last_bytes = atoi(optarg); break;
        default: break;
        }
    }

    send_msg(sm_msg_history_request("cli-read", 0.0, last_bytes));

    cJSON *result = NULL;
    int rc = wait_for_response(SM_MSG_HISTORY_RESPONSE, "cli-read",
                               cli.timeout_ms > 0 ? cli.timeout_ms : 5000,
                               &result, NULL);
    if (rc != 0) return 1;

    cJSON *chunks = cJSON_GetObjectItem(result, "chunks");
    if (cli.json_output) {
        char *s = cJSON_PrintUnformatted(result);
        if (s) { printf("%s\n", s); free(s); }
    } else {
        if (!chunks || cJSON_GetArraySize(chunks) == 0) {
            printf("(no output)\n");
        } else {
            cJSON *chunk;
            cJSON_ArrayForEach(chunk, chunks) {
                const char *b64 = sm_json_get_string(chunk, "data");
                if (b64) {
                    size_t dec_len;
                    uint8_t *data = sm_base64_decode(b64, strlen(b64), &dec_len);
                    if (data && dec_len > 0) {
                        /* Strip NUL bytes */
                        for (size_t i = 0; i < dec_len; i++) {
                            if (data[i] != 0)
                                fputc(data[i], stdout);
                        }
                    }
                    free(data);
                }
            }
        }
    }

    cJSON_Delete(result);
    return 0;
}

static int cmd_write(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "error: missing data argument\n"
                "usage: smolmux-cli write <data>\n");
        return 1;
    }

    /* Join remaining args */
    sm_strbuf_t data;
    sm_strbuf_init(&data);
    for (int i = 1; i < argc; i++) {
        if (i > 1) sm_strbuf_append(&data, " ", 1);
        sm_strbuf_append_str(&data, argv[i]);
    }

    send_msg(sm_msg_send("cli-wr", (const uint8_t *)data.data, data.len));
    sm_strbuf_destroy(&data);

    /* Brief wait for error (200ms) */
    struct pollfd fds[1] = {{ .fd = cli.sock_fd, .events = POLLIN }};
    if (poll(fds, 1, 200) > 0 && (fds[0].revents & POLLIN)) {
        cJSON *err = NULL;
        wait_for_response(SM_MSG_ERROR, "cli-wr", 100, &err, NULL);
        if (err) cJSON_Delete(err);
        /* Error already printed by wait_for_response */
        return 1;
    }

    if (!cli.json_output)
        printf("OK\n");
    else
        printf("{\"status\":\"ok\"}\n");
    return 0;
}

/* Print the boot-stage checklist from a status_response `boot` object and
 * return a state exit code: 0 = complete, 2 = stalled, 3 = in progress /
 * not started. */
static int print_boot_progress(cJSON *boot)
{
    int furthest = sm_json_get_int(boot, "furthest", -1);
    int total = sm_json_get_int(boot, "total", 0);
    int stalled = sm_json_get_bool(boot, "stalled", 0);
    int done = sm_json_get_bool(boot, "terminal_reached", 0);
    cJSON *stages = cJSON_GetObjectItem(boot, "stages");

    int reached = 0, idx = 0;
    const char *furthest_name = NULL;
    if (cJSON_IsArray(stages)) {
        cJSON *s;
        cJSON_ArrayForEach(s, stages) {
            if (sm_json_get_bool(s, "reached", 0)) reached++;
            if (idx == furthest) furthest_name = sm_json_get_string(s, "name");
            idx++;
        }
    }

    const char *state = done ? "complete" : (stalled ? "STALLED"
                      : (furthest < 0 ? "not started" : "in progress"));
    printf("Boot:      %d/%d stages", reached, total);
    if (furthest_name)
        printf(" (furthest: %s)", furthest_name);
    printf(" — %s\n", state);

    if (cJSON_IsArray(stages)) {
        idx = 0;
        cJSON *s;
        cJSON_ArrayForEach(s, stages) {
            const char *name = sm_json_get_string(s, "name");
            int r = sm_json_get_bool(s, "reached", 0);
            printf("  [%c] %s%s\n", r ? 'x' : ' ', name ? name : "?",
                   idx == furthest ? "  <-- furthest" : "");
            idx++;
        }
    }
    return done ? 0 : (stalled ? 2 : 3);
}

static int cmd_status(int argc, char **argv)
{
    (void)argc; (void)argv;

    send_msg(sm_msg_status("cli-status"));

    cJSON *result = NULL;
    int rc = wait_for_response(SM_MSG_STATUS_RESPONSE, "cli-status",
                               cli.timeout_ms > 0 ? cli.timeout_ms : 5000,
                               &result, NULL);
    if (rc != 0) return 1;

    if (cli.json_output) {
        char *s = cJSON_PrintUnformatted(result);
        if (s) { printf("%s\n", s); free(s); }
    } else {
        const char *port = sm_json_get_string(result, "port");
        int baud = sm_json_get_int(result, "baud", 0);
        int connected = sm_json_get_bool(result, "connected", 0);
        int suspended = sm_json_get_bool(result, "suspended", 0);
        const char *takeover = sm_json_get_string(result, "takeover_client");

        printf("Port:      %s\n", port ? port : "?");
        printf("Baud:      %d\n", baud);
        printf("Connected: %s\n", connected ? "yes" : "no");
        printf("Suspended: %s\n", suspended ? "yes" : "no");
        printf("Takeover:  %s\n", takeover ? takeover : "none");

        cJSON *clients = cJSON_GetObjectItem(result, "clients");
        if (clients && cJSON_GetArraySize(clients) > 0) {
            printf("Clients:\n");
            cJSON *c;
            cJSON_ArrayForEach(c, clients) {
                const char *name = sm_json_get_string(c, "name");
                const char *role = sm_json_get_string(c, "role");
                printf("  - %s (%s)\n", name ? name : "?", role ? role : "?");
            }
        }

        const char *log_path = sm_json_get_string(result, "log_path");
        if (log_path)
            printf("Log:       %s\n", log_path);

        /* Boot-stage progress (present only when the profile declares stages) */
        cJSON *boot = cJSON_GetObjectItem(result, "boot");
        if (boot)
            print_boot_progress(boot);
    }

    cJSON_Delete(result);
    return 0;
}

/* `boot-status`: scriptable — prints the checklist and exits by state
 * (0 = complete, 2 = stalled, 3 = in progress/not started, 4 = no stages
 * declared, 1 = error/timeout). */
static int cmd_boot_status(int argc, char **argv)
{
    (void)argc; (void)argv;

    send_msg(sm_msg_status("cli-boot"));

    cJSON *result = NULL;
    int rc = wait_for_response(SM_MSG_STATUS_RESPONSE, "cli-boot",
                               cli.timeout_ms > 0 ? cli.timeout_ms : 5000,
                               &result, NULL);
    if (rc != 0) return 1;

    cJSON *boot = cJSON_GetObjectItem(result, "boot");
    if (!boot) {
        if (cli.json_output)
            printf("null\n");
        else
            printf("No boot_stages defined in the device profile.\n");
        cJSON_Delete(result);
        return 4;
    }

    int state;
    if (cli.json_output) {
        int done = sm_json_get_bool(boot, "terminal_reached", 0);
        int stalled = sm_json_get_bool(boot, "stalled", 0);
        state = done ? 0 : (stalled ? 2 : 3);
        char *s = cJSON_PrintUnformatted(boot);
        if (s) { printf("%s\n", s); free(s); }
    } else {
        state = print_boot_progress(boot);
    }

    cJSON_Delete(result);
    return state;
}

static int cmd_history(int argc, char **argv)
{
    int last_bytes = 0;
    double seconds = 0.0;

    static const struct option opts[] = {
        {"last-bytes", required_argument, NULL, 'n'},
        {"seconds",    required_argument, NULL, 's'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    optind = 1;
    while ((opt = getopt_long(argc, argv, "n:s:", opts, NULL)) != -1) {
        switch (opt) {
        case 'n': last_bytes = atoi(optarg); break;
        case 's': seconds = atof(optarg); break;
        default: break;
        }
    }

    double since_ts = 0.0;
    if (seconds > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        since_ts = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9 - seconds;
    }

    send_msg(sm_msg_history_request("cli-hist", since_ts, last_bytes));

    cJSON *result = NULL;
    int rc = wait_for_response(SM_MSG_HISTORY_RESPONSE, "cli-hist",
                               cli.timeout_ms > 0 ? cli.timeout_ms : 10000,
                               &result, NULL);
    if (rc != 0) return 1;

    cJSON *chunks = cJSON_GetObjectItem(result, "chunks");
    if (cli.json_output) {
        char *s = cJSON_PrintUnformatted(result);
        if (s) { printf("%s\n", s); free(s); }
    } else {
        if (!chunks || cJSON_GetArraySize(chunks) == 0) {
            printf("(no output in history)\n");
        } else {
            cJSON *chunk;
            cJSON_ArrayForEach(chunk, chunks) {
                const char *b64 = sm_json_get_string(chunk, "data");
                if (b64) {
                    size_t dec_len;
                    uint8_t *data = sm_base64_decode(b64, strlen(b64), &dec_len);
                    if (data && dec_len > 0) {
                        for (size_t i = 0; i < dec_len; i++) {
                            if (data[i] != 0)
                                fputc(data[i], stdout);
                        }
                    }
                    free(data);
                }
            }
        }
    }

    cJSON_Delete(result);
    return 0;
}

static int cmd_incidents(int argc, char **argv)
{
    double seconds = 0.0;

    static const struct option opts[] = {
        {"seconds", required_argument, NULL, 's'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    optind = 1;
    while ((opt = getopt_long(argc, argv, "s:", opts, NULL)) != -1) {
        switch (opt) {
        case 's': seconds = atof(optarg); break;
        default: break;
        }
    }

    double since_ts = 0.0;
    if (seconds > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        since_ts = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9 - seconds;
    }

    send_msg(sm_msg_incidents_request("cli-inc", since_ts));

    cJSON *result = NULL;
    int rc = wait_for_response(SM_MSG_INCIDENTS_RESPONSE, "cli-inc",
                               cli.timeout_ms > 0 ? cli.timeout_ms : 5000,
                               &result, NULL);
    if (rc != 0) return 1;

    cJSON *incidents = cJSON_GetObjectItem(result, "incidents");
    if (cli.json_output) {
        char *s = cJSON_PrintUnformatted(result);
        if (s) { printf("%s\n", s); free(s); }
    } else {
        int count = incidents ? cJSON_GetArraySize(incidents) : 0;
        if (count == 0) {
            printf("No anomalies detected.\n");
        } else {
            int num = 0;
            cJSON *inc;
            cJSON_ArrayForEach(inc, incidents) {
                num++;
                const char *pattern = sm_json_get_string(inc, "pattern_name");
                const char *severity = sm_json_get_string(inc, "severity");
                const char *match = sm_json_get_string(inc, "match_text");
                const char *pre_ctx = sm_json_get_string(inc, "pre_context");

                printf("### Incident %d: %s [%s]\n", num,
                       pattern ? pattern : "?", severity ? severity : "?");
                printf("Match: %s\n", match ? match : "");
                if (pre_ctx && pre_ctx[0])
                    printf("Pre-context:\n```\n%s\n```\n", pre_ctx);
                printf("\n");
            }
        }
    }

    cJSON_Delete(result);
    return 0;
}

static int cmd_list_ports(int argc, char **argv)
{
    (void)argc; (void)argv;

    glob_t g;
    size_t total = sm_glob_serial_ports(&g);

    if (cli.json_output) {
        cJSON *arr = cJSON_CreateArray();
        for (size_t i = 0; i < total; i++)
            cJSON_AddItemToArray(arr, cJSON_CreateString(g.gl_pathv[i]));
        char *s = cJSON_PrintUnformatted(arr);
        if (s) { printf("%s\n", s); free(s); }
        cJSON_Delete(arr);
    } else {
        if (total == 0) {
            printf("No serial ports found.\n");
        } else {
            for (size_t i = 0; i < total; i++)
                printf("%s\n", g.gl_pathv[i]);
        }
    }

    globfree(&g);
    return 0;
}

static int cmd_sysrq(int argc, char **argv)
{
    if (argc < 2 || !argv[1][0]) {
        fprintf(stderr, "error: missing SysRq key\n"
                "usage: smolmux-cli sysrq <key>\n");
        return 1;
    }

    int break_ms = 500;
    int delay_ms = 100;
    const char *key = NULL;

    static const struct option opts[] = {
        {"break-ms", required_argument, NULL, 'b'},
        {"delay-ms", required_argument, NULL, 'd'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    optind = 1;
    while ((opt = getopt_long(argc, argv, "b:d:", opts, NULL)) != -1) {
        switch (opt) {
        case 'b': break_ms = atoi(optarg); break;
        case 'd': delay_ms = atoi(optarg); break;
        default: break;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "error: missing SysRq key\n");
        return 1;
    }
    key = argv[optind];

    /* Step 1: Send break */
    send_msg(sm_msg_pin_control("cli-brk", "break", "pulse", break_ms));

    /* Wait for ack */
    cJSON *ack = NULL;
    int rc = wait_for_response(SM_MSG_UNKNOWN, "cli-brk",
                               break_ms + 2000, &ack, NULL);
    /* Accept any response with matching id, or just continue */
    if (ack) cJSON_Delete(ack);

    /* Step 2: Delay */
    struct timespec ts = {.tv_sec = delay_ms / 1000,
                          .tv_nsec = (delay_ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);

    /* Step 3: Send key character */
    uint8_t ch = (uint8_t)key[0];
    send_msg(sm_msg_send("cli-sysrq", &ch, 1));

    if (cli.json_output)
        printf("{\"status\":\"ok\",\"key\":\"%c\"}\n", key[0]);
    else
        printf("OK (SysRq+%c)\n", key[0]);

    (void)rc;
    return 0;
}

static int cmd_pin(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "error: missing pin and action\n"
                "usage: smolmux-cli pin <dtr|rts|break> <set|clear|toggle|pulse> [--duration <ms>]\n");
        return 1;
    }

    const char *pin = argv[1];
    const char *action = argv[2];
    int duration_ms = 250;

    /* Check for --duration */
    for (int i = 3; i < argc - 1; i++) {
        if (strcmp(argv[i], "--duration") == 0)
            duration_ms = atoi(argv[i + 1]);
    }

    send_msg(sm_msg_pin_control("cli-pin", pin, action, duration_ms));

    cJSON *result = NULL;
    /* Broker sends back an ack (type="pin_control" status="ok") or error */
    int rc = wait_for_response(SM_MSG_PIN_CONTROL, "cli-pin",
                               cli.timeout_ms > 0 ? cli.timeout_ms : 5000,
                               &result, NULL);
    if (rc == 0) {
        if (cli.json_output)
            printf("{\"status\":\"ok\"}\n");
        else
            printf("OK\n");
        if (result) cJSON_Delete(result);
        return 0;
    }
    if (result) cJSON_Delete(result);
    return 1;
}

static int cmd_suspend(int argc, char **argv)
{
    (void)argc; (void)argv;

    send_msg(sm_msg_suspend("cli-sus"));

    cJSON *result = NULL;
    int rc = wait_for_response(SM_MSG_SUSPENDED, NULL,
                               cli.timeout_ms > 0 ? cli.timeout_ms : 5000,
                               &result, NULL);
    if (rc == 0) {
        if (cli.json_output)
            printf("{\"status\":\"ok\",\"action\":\"suspended\"}\n");
        else
            printf("OK — serial port suspended\n");
        if (result) cJSON_Delete(result);
        return 0;
    }
    if (result) cJSON_Delete(result);
    return 1;
}

static int cmd_resume(int argc, char **argv)
{
    (void)argc; (void)argv;

    send_msg(sm_msg_resume("cli-res"));

    cJSON *result = NULL;
    int rc = wait_for_response(SM_MSG_RESUMED, NULL,
                               cli.timeout_ms > 0 ? cli.timeout_ms : 5000,
                               &result, NULL);
    if (rc == 0) {
        if (cli.json_output)
            printf("{\"status\":\"ok\",\"action\":\"resumed\"}\n");
        else
            printf("OK — serial port resumed\n");
        if (result) cJSON_Delete(result);
        return 0;
    }
    if (result) cJSON_Delete(result);
    return 1;
}

/* with-port <command> [args...]:
 *   suspend the broker (release the port), run the command to completion,
 *   then ALWAYS resume — turning the racy suspend->tool->resume dance into one
 *   atomic, crash-safe step for flash-then-test loops. The command is exec'd
 *   directly (no shell), inheriting stdio. The CLI's exit code is the command's
 *   exit code (128+signal if it was killed), so `with-port flash && ...` works.
 */
static int cmd_with_port(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "error: with-port requires a command to run\n"
                        "  usage: smolmux-cli with-port <command> [args...]\n");
        return 2;
    }
    char **cmd_argv = &argv[1];
    int timeout = cli.timeout_ms > 0 ? cli.timeout_ms : 5000;

    /* 1. Suspend. If we can't release the port, do NOT run the command —
     * running it against a still-held port would fail confusingly. */
    send_msg(sm_msg_suspend("cli-wp-sus"));
    cJSON *sres = NULL;
    if (wait_for_response(SM_MSG_SUSPENDED, NULL, timeout, &sres, NULL) != 0) {
        if (sres) cJSON_Delete(sres);
        fprintf(stderr, "error: could not suspend broker; command not run\n");
        return 1;
    }
    if (sres) cJSON_Delete(sres);
    if (!cli.json_output)
        fprintf(stderr, "[with-port] port released; running: %s\n", cmd_argv[0]);

    /* 2. Run the command. */
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: drop the broker socket so the tool can't inherit it, restore
         * default signal dispositions the parent changed, then exec. */
        if (cli.sock_fd >= 0) close(cli.sock_fd);
        signal(SIGPIPE, SIG_DFL);
        execvp(cmd_argv[0], cmd_argv);
        fprintf(stderr, "error: exec %s: %s\n", cmd_argv[0], strerror(errno));
        _exit(127);
    }

    int child_code = 1;
    if (pid < 0) {
        fprintf(stderr, "error: fork: %s\n", strerror(errno));
        /* fall through to resume — we already released the port */
    } else {
        int status = 0;
        /* Ignore EINTR: a Ctrl-C hits the whole foreground group, so the child
         * dies and we must still fall through to resume, not abandon it. */
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            ;
        if (WIFEXITED(status))
            child_code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
            child_code = 128 + WTERMSIG(status);
    }

    /* 3. Resume — ALWAYS, even if the command failed or was interrupted. The
     * interrupt (if any) was meant for the command, not for our cleanup, so
     * clear it here or wait_for_response would bail before confirming resume. */
    cli.interrupted = 0;
    send_msg(sm_msg_resume("cli-wp-res"));
    cJSON *rres = NULL;
    int rrc = wait_for_response(SM_MSG_RESUMED, NULL, timeout, &rres, NULL);
    if (rres) cJSON_Delete(rres);
    if (rrc != 0) {
        fprintf(stderr, "WARNING: command finished but the broker did not "
                        "resume — the port may still be released. Run "
                        "'smolmux-cli resume' to re-acquire it.\n");
        if (child_code == 0)
            child_code = 1;   /* surface the resume failure */
    } else if (!cli.json_output) {
        fprintf(stderr, "[with-port] port re-acquired.\n");
    }

    if (cli.json_output)
        printf("{\"status\":\"%s\",\"action\":\"with-port\",\"exit_code\":%d}\n",
               child_code == 0 ? "ok" : "error", child_code);

    return child_code;
}

/* Test hook: connect to sock_path as controller and run with-port on argv
 * (argv[0] is the "with-port" token, argv[1..] the command, argv must be
 * NULL-terminated for execvp). Returns with-port's exit code; leaves cli
 * disconnected. */
int cli_test_with_port(const char *sock_path, int argc, char **argv,
                       int timeout_ms)
{
    cli_test_reset();
    cli.interrupted = 0;
    cli.json_output = 0;
    cli.timeout_ms = timeout_ms;
    if (connect_and_hello(sock_path, "controller") != 0)
        return -1;
    int rc = cmd_with_port(argc, argv);
    if (cli.sock_fd >= 0) {
        close(cli.sock_fd);
        cli.sock_fd = -1;
    }
    return rc;
}

static int cmd_watchdog(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "add") != 0) {
        fprintf(stderr, "usage: smolmux-cli watchdog add --name <name> --pattern <regex> [--severity <level>]\n");
        return 1;
    }

    const char *name = NULL;
    const char *pattern = NULL;
    const char *severity = "warning";

    static const struct option opts[] = {
        {"name",     required_argument, NULL, 'n'},
        {"pattern",  required_argument, NULL, 'p'},
        {"severity", required_argument, NULL, 's'},
        {NULL, 0, NULL, 0}
    };

    /* Skip "add" */
    int sub_argc = argc - 1;
    char **sub_argv = argv + 1;

    int opt;
    optind = 1;
    while ((opt = getopt_long(sub_argc, sub_argv, "n:p:s:", opts, NULL)) != -1) {
        switch (opt) {
        case 'n': name = optarg; break;
        case 'p': pattern = optarg; break;
        case 's': severity = optarg; break;
        default: break;
        }
    }

    if (!name || !pattern) {
        fprintf(stderr, "error: --name and --pattern are required\n");
        return 1;
    }

    cJSON *patterns = cJSON_CreateArray();
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "name", name);
    cJSON_AddStringToObject(entry, "pattern", pattern);
    cJSON_AddStringToObject(entry, "severity", severity);
    cJSON_AddItemToArray(patterns, entry);

    send_msg(sm_msg_configure_anomaly("cli-wd", patterns));

    cJSON *result = NULL;
    int rc = wait_for_response(SM_MSG_CONFIGURE_ANOMALY, "cli-wd",
                               cli.timeout_ms > 0 ? cli.timeout_ms : 5000,
                               &result, NULL);
    if (rc == 0) {
        if (cli.json_output) {
            char *s = cJSON_PrintUnformatted(result);
            if (s) { printf("%s\n", s); free(s); }
        } else {
            int added = sm_json_get_int(result, "patterns_added", 0);
            printf("Watchdog '%s' added (severity=%s, patterns_added=%d)\n",
                   name, severity, added);
        }
        cJSON_Delete(result);
        return 0;
    }
    if (result) cJSON_Delete(result);
    return 1;
}

static int cmd_autorespond(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: smolmux-cli autorespond <add|list|remove> ...\n"
                "  add --name <n> --pattern <regex> --send <str> [--once] "
                "[--cooldown <ms>]\n"
                "  list\n"
                "  remove --name <n>\n");
        return 1;
    }
    const char *sub = argv[1];

    if (strcmp(sub, "list") == 0) {
        send_msg(sm_msg_autoresponders_request("cli-ar"));
        cJSON *res = NULL;
        int rc = wait_for_response(SM_MSG_AUTORESPONDERS_RESPONSE, "cli-ar",
                                   cli.timeout_ms > 0 ? cli.timeout_ms : 5000,
                                   &res, NULL);
        if (rc != 0) return 1;
        if (cli.json_output) {
            char *s = cJSON_PrintUnformatted(res);
            if (s) { printf("%s\n", s); free(s); }
        } else {
            cJSON *rules = cJSON_GetObjectItem(res, "rules");
            int nr = (rules && cJSON_IsArray(rules)) ? cJSON_GetArraySize(rules) : 0;
            if (nr == 0) {
                printf("No autoresponder rules.\n");
            } else {
                cJSON *r;
                cJSON_ArrayForEach(r, rules) {
                    const char *nm = sm_json_get_string(r, "name");
                    const char *pat = sm_json_get_string(r, "pattern");
                    printf("  %-16s /%s/  once=%s cooldown=%dms\n",
                           nm ? nm : "?", pat ? pat : "?",
                           sm_json_get_bool(r, "once", 0) ? "yes" : "no",
                           sm_json_get_int(r, "cooldown_ms", 0));
                }
            }
        }
        if (res) cJSON_Delete(res);
        return 0;
    }

    /* add / remove share --name parsing */
    const char *name = NULL, *pattern = NULL, *send = NULL;
    int once = 0, cooldown = SM_AR_DEFAULT_COOLDOWN_MS;
    static const struct option opts[] = {
        {"name",     required_argument, NULL, 'n'},
        {"pattern",  required_argument, NULL, 'p'},
        {"send",     required_argument, NULL, 's'},
        {"once",     no_argument,       NULL, 'o'},
        {"cooldown", required_argument, NULL, 'c'},
        {NULL, 0, NULL, 0}
    };
    int sub_argc = argc - 1;
    char **sub_argv = argv + 1;
    int opt;
    optind = 1;
    while ((opt = getopt_long(sub_argc, sub_argv, "n:p:s:oc:", opts, NULL)) != -1) {
        switch (opt) {
        case 'n': name = optarg; break;
        case 'p': pattern = optarg; break;
        case 's': send = optarg; break;
        case 'o': once = 1; break;
        case 'c': cooldown = atoi(optarg); break;
        default: break;
        }
    }

    if (strcmp(sub, "remove") == 0) {
        if (!name) { fprintf(stderr, "error: --name is required\n"); return 1; }
        send_msg(sm_msg_configure_autoresponder("cli-ar", name, NULL, NULL, 0,
                                                0, 0, 1));
        cJSON *res = NULL;
        int rc = wait_for_response(SM_MSG_CONFIGURE_AUTORESPONDER, "cli-ar",
                                   cli.timeout_ms > 0 ? cli.timeout_ms : 5000,
                                   &res, NULL);
        if (rc != 0) return 1;
        int removed = res ? sm_json_get_bool(res, "removed", 0) : 0;
        if (cli.json_output)
            printf("{\"removed\":%s}\n", removed ? "true" : "false");
        else
            printf(removed ? "Removed autoresponder '%s'\n"
                           : "No autoresponder named '%s'\n", name);
        if (res) cJSON_Delete(res);
        return removed ? 0 : 1;
    }

    if (strcmp(sub, "add") == 0) {
        if (!name || !pattern || !send) {
            fprintf(stderr, "error: --name, --pattern and --send are required\n");
            return 1;
        }
        uint8_t resp[SM_AR_RESPONSE_MAX];
        size_t rlen = sm_str_unescape(send, resp, sizeof(resp));

        send_msg(sm_msg_configure_autoresponder("cli-ar", name, pattern,
                                                resp, rlen, once, cooldown, 0));
        cJSON *res = NULL;
        int rc = wait_for_response(SM_MSG_CONFIGURE_AUTORESPONDER, "cli-ar",
                                   cli.timeout_ms > 0 ? cli.timeout_ms : 5000,
                                   &res, NULL);
        if (rc != 0) return 1;
        if (cli.json_output) {
            char *s = cJSON_PrintUnformatted(res);
            if (s) { printf("%s\n", s); free(s); }
        } else {
            printf("Autoresponder '%s' added (pattern=/%s/, %zu bytes%s)\n",
                   name, pattern, rlen, once ? ", once" : "");
        }
        if (res) cJSON_Delete(res);
        return 0;
    }

    fprintf(stderr, "error: unknown autorespond subcommand '%s'\n", sub);
    return 1;
}

/* --- Monitor: streaming output for a duration --- */

typedef struct {
    sm_strbuf_t *output;
    int anomaly_count;
} monitor_ctx_t;

static void monitor_handler(sm_msg_t *msg, void *ctx)
{
    monitor_ctx_t *m = ctx;

    if (msg->type == SM_MSG_OUTPUT) {
        const char *b64 = sm_json_get_string(msg->root, "data");
        if (b64) {
            size_t dec_len;
            uint8_t *data = sm_base64_decode(b64, strlen(b64), &dec_len);
            if (data && dec_len > 0) {
                if (cli.json_output) {
                    sm_strbuf_append(m->output, (const char *)data, dec_len);
                } else {
                    fwrite(data, 1, dec_len, stdout);
                    fflush(stdout);
                }
            }
            free(data);
        }
    } else if (msg->type == SM_MSG_ANOMALY) {
        m->anomaly_count++;
        if (!cli.json_output) {
            const char *pat = sm_json_get_string(msg->root, "pattern_name");
            const char *sev = sm_json_get_string(msg->root, "severity");
            fprintf(stderr, "\n[anomaly:%s] %s\n", sev ? sev : "?", pat ? pat : "?");
        }
    } else if (msg->type == SM_MSG_LINK_DOWN) {
        if (!cli.json_output)
            fprintf(stderr, "\n[link_down]\n");
    }
}

static int cmd_monitor(int argc, char **argv)
{
    int duration = 30;

    static const struct option opts[] = {
        {"duration", required_argument, NULL, 'd'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    optind = 1;
    while ((opt = getopt_long(argc, argv, "d:", opts, NULL)) != -1) {
        switch (opt) {
        case 'd': duration = atoi(optarg); break;
        default: break;
        }
    }

    if (duration > 300) duration = 300;
    if (duration < 1) duration = 1;

    sm_strbuf_t output;
    sm_strbuf_init(&output);
    monitor_ctx_t mctx = { .output = &output, .anomaly_count = 0 };

    if (!cli.json_output)
        fprintf(stderr, "[monitoring for %d seconds — Ctrl-C to stop]\n", duration);

    struct pollfd fds[1] = {{ .fd = cli.sock_fd, .events = POLLIN }};
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (!cli.interrupted) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int elapsed_ms = (int)((now.tv_sec - start.tv_sec) * 1000 +
                               (now.tv_nsec - start.tv_nsec) / 1000000);
        int remaining_ms = duration * 1000 - elapsed_ms;
        if (remaining_ms <= 0) break;

        int ret = poll(fds, 1, remaining_ms < 1000 ? remaining_ms : 1000);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (ret > 0 && (fds[0].revents & POLLIN)) {
            if (read_messages(monitor_handler, &mctx) < 0)
                break;
        }
        if (ret > 0 && (fds[0].revents & (POLLHUP | POLLERR)))
            break;
    }

    if (cli.json_output) {
        cJSON *out = cJSON_CreateObject();
        cJSON_AddStringToObject(out, "output", output.data ? output.data : "");
        cJSON_AddNumberToObject(out, "anomalies", mctx.anomaly_count);
        char *s = cJSON_PrintUnformatted(out);
        if (s) { printf("%s\n", s); free(s); }
        cJSON_Delete(out);
    } else {
        fprintf(stderr, "\n[monitoring complete — %d anomalies]\n", mctx.anomaly_count);
    }

    sm_strbuf_destroy(&output);
    return 0;
}

/* --- Report formatting helpers --- */

static void format_status_section(sm_strbuf_t *report, cJSON *status)
{
    sm_strbuf_printf(report, "## Port Status\n");
    if (!status) return;

    const char *port = sm_json_get_string(status, "port");
    int baud = sm_json_get_int(status, "baud", 0);
    int connected = sm_json_get_bool(status, "connected", 0);
    int suspended = sm_json_get_bool(status, "suspended", 0);
    sm_strbuf_printf(report, "- Port: %s\n", port ? port : "?");
    sm_strbuf_printf(report, "- Baud: %d\n", baud);
    sm_strbuf_printf(report, "- Connected: %s\n", connected ? "yes" : "no");
    sm_strbuf_printf(report, "- Suspended: %s\n", suspended ? "yes" : "no");

    cJSON *clients = cJSON_GetObjectItem(status, "clients");
    if (clients)
        sm_strbuf_printf(report, "- Clients: %d\n", cJSON_GetArraySize(clients));

    cJSON *boot = cJSON_GetObjectItem(status, "boot");
    if (boot) {
        int furthest = sm_json_get_int(boot, "furthest", -1);
        int total = sm_json_get_int(boot, "total", 0);
        int stalled = sm_json_get_bool(boot, "stalled", 0);
        int done = sm_json_get_bool(boot, "terminal_reached", 0);
        cJSON *stages = cJSON_GetObjectItem(boot, "stages");
        int reached = 0;
        if (cJSON_IsArray(stages)) {
            cJSON *s;
            cJSON_ArrayForEach(s, stages)
                if (sm_json_get_bool(s, "reached", 0)) reached++;
        }
        const char *state = done ? "complete" : (stalled ? "STALLED"
                          : (furthest < 0 ? "not started" : "in progress"));
        sm_strbuf_printf(report, "- Boot: %d/%d stages — %s\n",
                         reached, total, state);
    }
}

static void format_incidents_section(sm_strbuf_t *report, cJSON *incidents_resp)
{
    sm_strbuf_printf(report, "\n## Incidents\n");
    cJSON *incidents = incidents_resp ?
        cJSON_GetObjectItem(incidents_resp, "incidents") : NULL;

    if (!incidents || cJSON_GetArraySize(incidents) == 0) {
        sm_strbuf_printf(report, "None detected.\n");
        return;
    }

    cJSON *inc;
    cJSON_ArrayForEach(inc, incidents) {
        const char *pat = sm_json_get_string(inc, "pattern_name");
        const char *sev = sm_json_get_string(inc, "severity");
        const char *match = sm_json_get_string(inc, "match_text");
        sm_strbuf_printf(report, "- **%s** [%s]: %s\n",
                         pat ? pat : "?", sev ? sev : "?",
                         match ? match : "");
    }
}

static void format_history_section(sm_strbuf_t *report, cJSON *history_resp)
{
    sm_strbuf_printf(report, "\n## Recent Output (last 32KB)\n");
    cJSON *chunks = history_resp ?
        cJSON_GetObjectItem(history_resp, "chunks") : NULL;

    if (!chunks || cJSON_GetArraySize(chunks) == 0) {
        sm_strbuf_printf(report, "(no output)\n");
        return;
    }

    sm_strbuf_printf(report, "```\n");
    cJSON *chunk;
    cJSON_ArrayForEach(chunk, chunks) {
        const char *b64 = sm_json_get_string(chunk, "data");
        if (!b64) continue;
        size_t dec_len;
        uint8_t *data = sm_base64_decode(b64, strlen(b64), &dec_len);
        if (data && dec_len > 0) {
            for (size_t i = 0; i < dec_len; i++) {
                if (data[i] != 0)
                    sm_strbuf_append(report, (const char *)&data[i], 1);
            }
        }
        free(data);
    }
    sm_strbuf_printf(report, "\n```\n");
}

static int request_json(sm_msg_type_t type, const char *id, int timeout_ms,
                        cJSON **out)
{
    return wait_for_response(type, id,
                             timeout_ms > 0 ? timeout_ms : 5000, out, NULL);
}

static int cmd_report(int argc, char **argv)
{
    (void)argc; (void)argv;
    int timeout = cli.timeout_ms;

    /* Fetch all data */
    send_msg(sm_msg_status("cli-rpt-s"));
    cJSON *status = NULL;
    request_json(SM_MSG_STATUS_RESPONSE, "cli-rpt-s", timeout, &status);

    send_msg(sm_msg_incidents_request("cli-rpt-i", 0.0));
    cJSON *incidents_resp = NULL;
    request_json(SM_MSG_INCIDENTS_RESPONSE, "cli-rpt-i", timeout, &incidents_resp);

    send_msg(sm_msg_history_request("cli-rpt-h", 0.0, 32768));
    cJSON *history_resp = NULL;
    request_json(SM_MSG_HISTORY_RESPONSE, "cli-rpt-h",
                 timeout > 0 ? timeout : 10000, &history_resp);

    if (cli.json_output) {
        cJSON *out = cJSON_CreateObject();
        if (status) cJSON_AddItemToObject(out, "status", cJSON_Duplicate(status, 1));
        if (incidents_resp) cJSON_AddItemToObject(out, "incidents", cJSON_Duplicate(incidents_resp, 1));
        if (history_resp) cJSON_AddItemToObject(out, "history", cJSON_Duplicate(history_resp, 1));
        char *s = cJSON_PrintUnformatted(out);
        if (s) { printf("%s\n", s); free(s); }
        cJSON_Delete(out);
    } else {
        sm_strbuf_t report;
        sm_strbuf_init(&report);
        sm_strbuf_printf(&report, "# smolmux Device Report\n\n");
        format_status_section(&report, status);
        format_incidents_section(&report, incidents_resp);
        format_history_section(&report, history_resp);

        if (report.data)
            fwrite(report.data, 1, report.len, stdout);
        sm_strbuf_destroy(&report);
    }

    if (status) cJSON_Delete(status);
    if (incidents_resp) cJSON_Delete(incidents_resp);
    if (history_resp) cJSON_Delete(history_resp);
    return 0;
}

/* --- Help and usage --- */

static void usage(const char *prog)
{
    fprintf(stderr,
        "%s — command-line client for smolmux broker\n"
        "\n"
        "Connects to a running smolmux broker and executes commands.\n"
        "Designed for both human and LLM use (structured output with --json).\n"
        "\n"
        "USAGE:\n"
        "  %s [options] <command> [args...]\n"
        "\n"
        "GLOBAL OPTIONS:\n"
        "  -s, --socket <path>     Broker Unix socket (auto-discover if omitted)\n"
        "  -j, --json              Output in JSON format\n"
        "  -t, --timeout <ms>      Response timeout in milliseconds\n"
        "  -v, --verbose           Debug output to stderr\n"
        "  -V, --version           Show version\n"
        "  -h, --help              Show this help\n"
        "\n",
        prog, prog);
    fprintf(stderr,
        "COMMANDS:\n"
        "  send <command>          Send a command and wait for response\n"
        "    --expect <pattern>    Regex to match end of response (default: shell prompt)\n"
        "    --timeout <ms>        Per-command timeout (default: 5000)\n"
        "\n"
        "  read                    Read recent output from ring buffer\n"
        "    --bytes <n>           Number of bytes to read (default: 8192)\n"
        "\n"
        "  write <data>            Write raw data without waiting for response\n"
        "\n"
        "  status                  Show broker and port status\n"
        "\n"
        "  boot-status             Show cold-boot progress; exit 0=complete,\n"
        "                          2=stalled, 3=in-progress, 4=no stages\n"
        "\n"
        "  history                 Get timestamped output history\n"
        "    --last-bytes <n>      Return last N bytes\n"
        "    --seconds <n>         Return output from last N seconds\n"
        "\n"
        "  incidents               Show detected anomalies/crashes\n"
        "    --seconds <n>         Only incidents from last N seconds\n"
        "\n"
        "  list-ports              List available serial ports (no broker needed)\n"
        "\n"
        "  brokers                 List active brokers and what each holds\n"
        "                          (no broker needed; use --json for agents)\n"
        "\n"
        "  boards                  Group active brokers by --board (a board's wires)\n"
        "                          (no broker needed; use --json for agents)\n"
        "\n"
        "  board up <manifest>     Start every wire in a *.board.json manifest\n"
        "    --foreground, -F      Tie the wires' lifetime to this run (Ctrl-C stops all)\n"
        "  board down <name>       Stop all running wires of a board (SIGTERM)\n"
        "\n"
        "  shutdown                Stop one broker cleanly (alias: stop)\n"
        "                          Uses -s <path>, or the sole running broker;\n"
        "                          SIGTERM + wait until the socket disappears\n"
        "  board status <name>     Show a board's active wires\n"
        "  board list [dir]        List *.board.json manifests (dir, $SMOLMUX_BOARD_DIR,\n"
        "                          or ~/.config/smolmux) and which are up\n"
        "\n"
        "  sysrq <key>             Send Linux SysRq (BREAK + key)\n"
        "    --break-ms <ms>       Break duration (default: 500)\n"
        "    --delay-ms <ms>       Delay between break and key (default: 100)\n"
        "\n"
        "  break-uboot             Flood a key to break into a bootdelay=0 bootloader\n"
        "    --key <str>           Key to flood (default: space)\n"
        "    --stop <regex>        Stop when this matches (default: U-Boot prompt)\n"
        "    --no-stop             Flood for the full duration, no stop pattern\n"
        "    --interval <ms>       Between keystrokes (default: 10)\n"
        "    --duration <ms>       Max flood time (default: 2000)\n"
        "\n"
        "  pin <pin> <action>      Control serial pins\n"
        "    pin:    dtr, rts, break\n"
        "    action: set, clear, toggle, pulse\n"
        "    --duration <ms>       Break/pulse duration (default: 250)\n"
        "\n"
        "  suspend                 Release the serial port for external tools\n"
        "  resume                  Re-acquire the serial port\n"
        "  with-port <cmd> [args]  Suspend, run <cmd>, then always resume\n"
        "                          (atomic handoff, e.g. flash-then-test)\n"
        "\n"
        "  watchdog add            Add anomaly detection pattern\n"
        "    --name <name>         Pattern name (required)\n"
        "    --pattern <regex>     Regex to match (required)\n"
        "    --severity <level>    critical, warning, or info (default: warning)\n"
        "\n"
        "  autorespond <add|list|remove>  Standing expect->send rules\n"
        "    add --name <n> --pattern <regex> --send <str> [--once] "
        "[--cooldown <ms>]\n"
        "    (--send interprets \\n \\r \\t \\0 escapes; e.g. --send 'y\\n')\n"
        "\n"
        "  monitor                 Stream output for a duration\n"
        "    --duration <seconds>  How long to monitor (default: 30, max: 300)\n"
        "\n"
        "  report                  Generate a status report\n"
        "\n"
        "EXAMPLES:\n"
        "  %s send \"uname -a\"\n"
        "  %s send --expect 'login:' --timeout 10000 \"\"\n"
        "  %s read --bytes 4096\n"
        "  %s status --json\n"
        "  %s history --seconds 60\n"
        "  %s sysrq h\n"
        "  %s watchdog add --name oom --pattern 'Out of memory' --severity critical\n"
        "  %s monitor --duration 60\n"
        "  %s list-ports\n"
        "\n"
        "SOCKET DISCOVERY:\n"
        "  1. --socket <path> flag\n"
        "  2. $SMOLMUX_SOCKET environment variable\n"
        "  3. Glob $XDG_RUNTIME_DIR/smolmux-*.sock\n"
        "  4. Glob /tmp/smolmux-*.sock\n"
        "\n"
        "ENVIRONMENT:\n"
        "  SMOLMUX_SOCKET    Override broker socket path\n",
        prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

/* List active brokers and what each holds. No broker connection needed — it
 * enumerates and probes every broker. --json emits a machine-readable array
 * (agent-facing); text mode prints one line per broker. */
static int cmd_brokers(int argc, char **argv)
{
    (void)argc; (void)argv;

    sm_broker_info_t infos[64];
    size_t n = sm_broker_discover(infos, 64, 1000);
    size_t shown = n < 64 ? n : 64;

    if (cli.json_output) {
        cJSON *arr = cJSON_CreateArray();
        for (size_t i = 0; i < shown; i++)
            cJSON_AddItemToArray(arr, sm_broker_info_to_json(&infos[i]));
        char *s = cJSON_PrintUnformatted(arr);
        if (s) { printf("%s\n", s); free(s); }
        cJSON_Delete(arr);
    } else if (n == 0) {
        printf("No active smolmux brokers found.\n");
    } else {
        for (size_t i = 0; i < shown; i++) {
            char line[320];
            sm_broker_info_format(&infos[i], line, sizeof(line));
            printf("%s\n", line);
        }
        if (n > shown)
            printf("... and %zu more\n", n - shown);
    }
    return 0;
}

/* Group reachable brokers by their --board label and print each board with its
 * wires. Brokers without a board (or unreachable) fall under "(unlabeled)".
 * The "board object" is derived live from discovery — no central registry. */
static int cmd_boards(int argc, char **argv)
{
    (void)argc; (void)argv;

    sm_broker_info_t infos[64];
    size_t n = sm_broker_discover(infos, 64, 1000);
    size_t shown = n < 64 ? n : 64;

    /* Collect the distinct board names in first-seen order. */
    const char *boards[64];
    size_t board_n = 0;
    for (size_t i = 0; i < shown; i++) {
        const char *b = infos[i].reachable && infos[i].board[0]
                            ? infos[i].board : "(unlabeled)";
        int seen = 0;
        for (size_t j = 0; j < board_n; j++)
            if (strcmp(boards[j], b) == 0) { seen = 1; break; }
        if (!seen && board_n < 64) boards[board_n++] = b;
    }

    if (cli.json_output) {
        cJSON *arr = cJSON_CreateArray();
        for (size_t bi = 0; bi < board_n; bi++) {
            cJSON *bo = cJSON_CreateObject();
            cJSON_AddStringToObject(bo, "board", boards[bi]);
            cJSON *wires = cJSON_CreateArray();
            for (size_t i = 0; i < shown; i++) {
                const char *b = infos[i].reachable && infos[i].board[0]
                                    ? infos[i].board : "(unlabeled)";
                if (strcmp(b, boards[bi]) == 0)
                    cJSON_AddItemToArray(wires, sm_broker_info_to_json(&infos[i]));
            }
            cJSON_AddItemToObject(bo, "wires", wires);
            cJSON_AddItemToArray(arr, bo);
        }
        char *s = cJSON_PrintUnformatted(arr);
        if (s) { printf("%s\n", s); free(s); }
        cJSON_Delete(arr);
        return 0;
    }

    if (n == 0) {
        printf("No active smolmux brokers found.\n");
        return 0;
    }

    for (size_t bi = 0; bi < board_n; bi++) {
        size_t count = 0;
        for (size_t i = 0; i < shown; i++) {
            const char *b = infos[i].reachable && infos[i].board[0]
                                ? infos[i].board : "(unlabeled)";
            if (strcmp(b, boards[bi]) == 0) count++;
        }
        printf("Board: %s (%zu wire%s)\n", boards[bi], count, count == 1 ? "" : "s");
        for (size_t i = 0; i < shown; i++) {
            const char *b = infos[i].reachable && infos[i].board[0]
                                ? infos[i].board : "(unlabeled)";
            if (strcmp(b, boards[bi]) != 0) continue;
            const char *role = infos[i].role[0] ? infos[i].role : "-";
            const char *lt = infos[i].reachable ? infos[i].link_type : "?";
            printf("  %-8s %-4s %-22s %s%s\n",
                   role, lt, infos[i].endpoint, infos[i].socket,
                   infos[i].reachable ? "" : "  (unreachable)");
        }
    }
    return 0;
}

/* --- board up/down/status: manifest-driven launcher --- */

/* Locate the smolmux broker binary: alongside this executable, else PATH. */
static void resolve_broker_path(char *out, size_t len)
{
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            snprintf(out, len, "%s/smolmux", exe);
            if (access(out, X_OK) == 0) return;
        }
    }
    snprintf(out, len, "smolmux");   /* fall back to PATH via execvp */
}

/* Spawn one wire's broker. Detached (default): setsid so it outlives us.
 * Foreground: stay in our process group so lifetime is tied to this run.
 * stdout/stderr go to a per-wire logfile in both modes. Returns the pid. */
static pid_t spawn_wire(const char *path, char *const argv[],
                        int foreground, const char *logfile)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (!foreground) setsid();
        int fd = open(logfile, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) { dup2(nul, 0); if (nul > 0) close(nul); }
        execv(path, argv);
        execvp("smolmux", argv);   /* path not executable -> try PATH */
        _exit(127);
    }
    return pid;
}

static void wire_logfile(const sm_board_manifest_t *m, const sm_board_wire_t *w,
                         char *out, size_t len)
{
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir || !dir[0]) dir = "/tmp";
    snprintf(out, len, "%s/smolmux-%s-%s.log", dir, m->board, w->role);
}

/* Build the broker argv for a wire. Buffers must outlive the fork/exec. */
static int build_wire_argv(const char *broker, const sm_board_manifest_t *m,
                           const sm_board_wire_t *w, const char *sock,
                           char *baudstr, size_t baudlen, char **argv)
{
    int a = 0;
    argv[a++] = (char *)broker;
    if (strcmp(w->link, "gdb") == 0) {
        argv[a++] = "--gdb";
        argv[a++] = "--gdb-path"; argv[a++] = (char *)w->gdb_path;
        if (w->target[0]) { argv[a++] = "--gdb-target"; argv[a++] = (char *)w->target; }
    } else {
        snprintf(baudstr, baudlen, "%d", w->baud);
        argv[a++] = (char *)w->device;
        argv[a++] = "-b"; argv[a++] = baudstr;
        if (w->profile[0]) { argv[a++] = "-p"; argv[a++] = (char *)w->profile; }
    }
    argv[a++] = "--board"; argv[a++] = (char *)m->board;
    argv[a++] = "--role";  argv[a++] = (char *)w->role;
    argv[a++] = "-s";      argv[a++] = (char *)sock;
    argv[a] = NULL;
    return a;
}

static int board_up(const char *manifest_path, int foreground)
{
    sm_board_manifest_t m;
    if (sm_board_manifest_load(manifest_path, &m) != 0) {
        fprintf(stderr, "error: failed to load manifest %s\n", manifest_path);
        return 1;
    }

    char broker[4096];
    resolve_broker_path(broker, sizeof(broker));

    printf("Bringing up board '%s' (%zu wire%s)%s\n", m.board, m.wire_count,
           m.wire_count == 1 ? "" : "s", foreground ? " [foreground]" : "");

    pid_t pids[SM_BOARD_MAX_WIRES];
    char  socks[SM_BOARD_MAX_WIRES][SM_SOCK_PATH_MAX];
    int   started[SM_BOARD_MAX_WIRES] = {0};
    int   npids = 0;

    for (size_t i = 0; i < m.wire_count; i++) {
        sm_board_wire_t *w = &m.wires[i];
        if (sm_board_wire_socket(&m, w, socks[i], sizeof(socks[i])) != 0) {
            printf("  %-8s ERROR: socket path too long — set an explicit "
                   "\"socket\"\n", w->role);
            continue;
        }

        /* Idempotent: skip a wire whose broker is already answering. */
        sm_broker_info_t info;
        if (sm_broker_probe(socks[i], &info, 400) == 0 && info.reachable) {
            printf("  %-8s already up  (%s)\n", w->role, socks[i]);
            continue;
        }

        char baudstr[16];
        char *argv[24];
        build_wire_argv(broker, &m, w, socks[i], baudstr, sizeof(baudstr), argv);

        char logf[4096];
        wire_logfile(&m, w, logf, sizeof(logf));
        pid_t pid = spawn_wire(broker, argv, foreground, logf);
        if (pid < 0) {
            printf("  %-8s ERROR: fork failed\n", w->role);
            continue;
        }
        pids[npids++] = pid;
        started[i] = 1;
    }

    /* Verify: give freshly-spawned wires a moment, then probe. */
    for (size_t i = 0; i < m.wire_count; i++) {
        if (!started[i]) continue;
        sm_broker_info_t info;
        int up = 0;
        for (int try = 0; try < 15 && !up; try++) {
            if (sm_broker_probe(socks[i], &info, 200) == 0 && info.reachable)
                up = 1;
            else
                usleep(100000);
        }
        printf("  %-8s %s  (%s)\n", m.wires[i].role,
               up ? "up" : "FAILED — see log", socks[i]);
    }

    if (!foreground)
        return 0;

    /* Foreground: lifetime tied to this run. Wait for a signal, then stop all
     * wires we started (Ctrl-C in a terminal also signals them directly). */
    printf("Board '%s' up in foreground. Ctrl-C to stop all wires.\n", m.board);
    while (!cli.interrupted)
        pause();
    printf("\nStopping board '%s'...\n", m.board);
    for (int i = 0; i < npids; i++)
        kill(pids[i], SIGTERM);
    for (int i = 0; i < npids; i++) {
        int st;
        waitpid(pids[i], &st, 0);
    }
    return 0;
}

static int board_down(const char *name)
{
    sm_broker_info_t infos[64];
    size_t n = sm_broker_discover(infos, 64, 800);
    size_t shown = n < 64 ? n : 64;

    int signaled = 0;
    for (size_t i = 0; i < shown; i++) {
        if (!infos[i].reachable || strcmp(infos[i].board, name) != 0)
            continue;
        const char *role = infos[i].role[0] ? infos[i].role : "-";
        if (infos[i].pid <= 0) {
            printf("  %-8s unknown pid — stop manually (%s)\n", role, infos[i].socket);
            continue;
        }
        if (kill(infos[i].pid, SIGTERM) == 0) {
            printf("  %-8s stopped pid %d  (%s)\n", role, infos[i].pid, infos[i].socket);
            signaled++;
        } else {
            printf("  %-8s signal pid %d failed: %s\n", role, infos[i].pid,
                   strerror(errno));
        }
    }
    if (signaled == 0)
        printf("No running wires found for board '%s'.\n", name);
    else
        printf("Board '%s': stopped %d wire%s (SIGTERM).\n", name, signaled,
               signaled == 1 ? "" : "s");
    return 0;
}

static int board_status(const char *name)
{
    sm_broker_info_t infos[64];
    size_t n = sm_broker_discover(infos, 64, 800);
    size_t shown = n < 64 ? n : 64;

    int found = 0;
    for (size_t i = 0; i < shown; i++) {
        if (!infos[i].reachable || strcmp(infos[i].board, name) != 0)
            continue;
        char line[320];
        sm_broker_info_format(&infos[i], line, sizeof(line));
        printf("%s\n", line);
        found++;
    }
    if (!found)
        printf("No active wires for board '%s'.\n", name);
    return 0;
}

/* List *.board.json manifests from a dir (arg > $SMOLMUX_BOARD_DIR >
 * ~/.config/smolmux), cross-referenced with live discovery to mark which are
 * currently up. */
static int board_list(const char *dir_arg)
{
    char dirbuf[512];
    const char *dir = dir_arg;
    if (!dir) dir = getenv("SMOLMUX_BOARD_DIR");
    if (!dir) {
        const char *home = getenv("HOME");
        if (!home) {
            fprintf(stderr, "error: no board dir (pass one, set SMOLMUX_BOARD_DIR,"
                    " or set HOME)\n");
            return 1;
        }
        snprintf(dirbuf, sizeof(dirbuf), SM_PROFILE_CONFIG_DIR_FMT, home);
        dir = dirbuf;
    }

    char pattern[600];
    snprintf(pattern, sizeof(pattern), "%s/*.board.json", dir);
    glob_t g;
    memset(&g, 0, sizeof(g));
    glob(pattern, 0, NULL, &g);
    if (g.gl_pathc == 0) {
        if (cli.json_output) printf("[]\n");
        else printf("No board manifests in %s\n", dir);
        globfree(&g);
        return 0;
    }

    /* One live-discovery pass to tell which manifests are up. */
    sm_broker_info_t infos[64];
    size_t nlive = sm_broker_discover(infos, 64, 800);
    size_t livemax = nlive < 64 ? nlive : 64;

    cJSON *arr = cli.json_output ? cJSON_CreateArray() : NULL;

    for (size_t i = 0; i < g.gl_pathc; i++) {
        sm_board_manifest_t m;
        if (sm_board_manifest_load(g.gl_pathv[i], &m) != 0) {
            if (!cli.json_output)
                printf("%-16s %-9s (invalid manifest)  (%s)\n", "?", "-",
                       g.gl_pathv[i]);
            continue;
        }
        int up = 0;
        for (size_t k = 0; k < livemax; k++)
            if (infos[k].reachable && strcmp(infos[k].board, m.board) == 0)
                up++;

        if (cli.json_output) {
            cJSON *bo = cJSON_CreateObject();
            cJSON_AddStringToObject(bo, "board", m.board);
            cJSON_AddStringToObject(bo, "manifest", g.gl_pathv[i]);
            cJSON_AddNumberToObject(bo, "wires", (double)m.wire_count);
            cJSON_AddNumberToObject(bo, "running", up);
            cJSON *ws = cJSON_CreateArray();
            for (size_t w = 0; w < m.wire_count; w++) {
                cJSON *wo = cJSON_CreateObject();
                cJSON_AddStringToObject(wo, "role", m.wires[w].role);
                cJSON_AddStringToObject(wo, "link", m.wires[w].link);
                cJSON_AddItemToArray(ws, wo);
            }
            cJSON_AddItemToObject(bo, "wire_list", ws);
            cJSON_AddItemToArray(arr, bo);
        } else {
            char wires[256];
            size_t off = 0;
            for (size_t w = 0; w < m.wire_count && off < sizeof(wires); w++)
                off += snprintf(wires + off, sizeof(wires) - off, "%s%s(%s)",
                                w ? ", " : "", m.wires[w].role, m.wires[w].link);
            char state[16];
            if (up == 0) snprintf(state, sizeof(state), "down");
            else snprintf(state, sizeof(state), "%d/%zu up", up, m.wire_count);
            printf("%-16s %-9s %s  (%s)\n", m.board, state, wires, g.gl_pathv[i]);
        }
    }

    if (cli.json_output) {
        char *s = cJSON_PrintUnformatted(arr);
        if (s) { printf("%s\n", s); free(s); }
        cJSON_Delete(arr);
    }
    globfree(&g);
    return 0;
}

static int cmd_board(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: smolmux-cli board <up|down|status> ...\n");
        return 1;
    }
    const char *action = argv[1];

    if (strcmp(action, "up") == 0) {
        const char *manifest = NULL;
        int foreground = 0;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--foreground") == 0 || strcmp(argv[i], "-F") == 0)
                foreground = 1;
            else
                manifest = argv[i];
        }
        if (!manifest) {
            fprintf(stderr, "usage: smolmux-cli board up <manifest.json> "
                    "[--foreground]\n");
            return 1;
        }
        return board_up(manifest, foreground);
    }
    if (strcmp(action, "down") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: smolmux-cli board down <name>\n"); return 1; }
        return board_down(argv[2]);
    }
    if (strcmp(action, "status") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: smolmux-cli board status <name>\n"); return 1; }
        return board_status(argv[2]);
    }
    if (strcmp(action, "list") == 0)
        return board_list(argc >= 3 ? argv[2] : NULL);

    fprintf(stderr, "error: unknown board action '%s' (up|down|status|list)\n",
            action);
    return 1;
}

/* --- Subcommand dispatch table --- */

typedef struct {
    const char *name;
    int (*func)(int argc, char **argv);
    int needs_broker;      /* 0 = no broker needed */
    const char *role;      /* "observer" or "controller" */
} subcmd_t;

/* Proactively flood a key to break into a 0-delay bootloader (the thing an
 * agent-in-the-loop can't win reactively). The broker streams the key from
 * inside its event loop and stops the instant the prompt appears. */
static int cmd_break_uboot(int argc, char **argv)
{
    const char *key = " ";
    const char *stop = "=>\\s*$|U-Boot>\\s*$";   /* common U-Boot prompts */
    int interval_ms = SM_DEFAULT_FLOOD_INTERVAL_MS;
    int duration_ms = SM_DEFAULT_FLOOD_DURATION_MS;
    const char *reset_pin = NULL;        /* --reset dtr|rts (reset_and_interrupt) */
    const char *reset_active = "low";    /* --reset-active low|high */
    int reset_hold_ms = SM_DEFAULT_RESET_HOLD_MS;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--key") == 0 && i + 1 < argc) key = argv[++i];
        else if (strcmp(argv[i], "--stop") == 0 && i + 1 < argc) stop = argv[++i];
        else if (strcmp(argv[i], "--no-stop") == 0) stop = NULL;
        else if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc) interval_ms = atoi(argv[++i]);
        else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) duration_ms = atoi(argv[++i]);
        else if (strcmp(argv[i], "--reset") == 0 && i + 1 < argc) reset_pin = argv[++i];
        else if (strcmp(argv[i], "--reset-active") == 0 && i + 1 < argc) reset_active = argv[++i];
        else if (strcmp(argv[i], "--reset-hold") == 0 && i + 1 < argc) reset_hold_ms = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: smolmux-cli break-uboot [--key <str>] "
                    "[--stop <regex>|--no-stop] [--interval <ms>] [--duration <ms>]\n"
                    "         [--reset <dtr|rts> [--reset-active <low|high>] "
                    "[--reset-hold <ms>]]\n");
            return 1;
        }
    }
    if (!key[0]) { fprintf(stderr, "error: empty --key\n"); return 1; }
    if (reset_pin && strcmp(reset_pin, "dtr") != 0 && strcmp(reset_pin, "rts") != 0) {
        fprintf(stderr, "error: --reset must be 'dtr' or 'rts'\n");
        return 1;
    }
    if (strcmp(reset_active, "low") != 0 && strcmp(reset_active, "high") != 0) {
        fprintf(stderr, "error: --reset-active must be 'low' or 'high'\n");
        return 1;
    }

    cJSON *msg = sm_msg_interrupt_autoboot("cli-autoboot", (const uint8_t *)key,
                                           strlen(key), interval_ms, duration_ms, stop);
    if (reset_pin) {
        cJSON_AddStringToObject(msg, "reset_pin", reset_pin);
        /* active-low reset: assert = clear the line; active-high: assert = set */
        cJSON_AddStringToObject(msg, "reset_assert",
                                strcmp(reset_active, "high") == 0 ? "set" : "clear");
        cJSON_AddNumberToObject(msg, "reset_hold_ms", reset_hold_ms);
    }
    send_msg(msg);

    cJSON *res = NULL;
    /* Wait a little past the flood's own cap so the result is always seen. */
    int rc = wait_for_response(SM_MSG_AUTOBOOT_RESULT, "cli-autoboot",
                               duration_ms + 3000, &res, NULL);
    if (rc != 0) return 1;   /* error already printed by wait_for_response */

    int matched = res ? sm_json_get_bool(res, "matched", 0) : 0;
    const char *reason = res ? sm_json_get_string(res, "reason") : "unknown";
    int sent = res ? sm_json_get_int(res, "sent", 0) : 0;
    if (cli.json_output)
        printf("{\"matched\":%s,\"reason\":\"%s\",\"sent\":%d}\n",
               matched ? "true" : "false", reason ? reason : "", sent);
    else
        printf("%s (%s, %d keys sent)\n",
               matched ? "Prompt reached — bootloader interrupted" : "No prompt seen",
               reason ? reason : "?", sent);
    if (res) cJSON_Delete(res);
    return matched ? 0 : 2;   /* 2 = flooded but no prompt (timeout) */
}

/* Explicit -s path from the global options, for commands that manage a broker
 * without connecting through the needs_broker path (shutdown). */
static const char *g_socket_arg;

/* Stop one broker cleanly: probe its pid, SIGTERM, wait for the socket to
 * disappear. Uses the explicit -s path, or the sole running broker; with
 * several brokers up it refuses and lists them (never guess a kill target). */
static int cmd_shutdown(int argc, char *argv[])
{
    (void)argc; (void)argv;

    char sock[SM_SOCK_PATH_MAX];
    if (g_socket_arg) {
        snprintf(sock, sizeof(sock), "%s", g_socket_arg);
    } else {
        char paths[16][SM_SOCK_PATH_MAX];
        size_t n = sm_discover_all_sockets(paths, 16);
        if (n == 0) {
            fprintf(stderr, "error: no broker socket found\n"
                    "  Use -s <path> or start a broker first\n");
            return 1;
        }
        if (n > 1) {
            size_t shown = n < 16 ? n : 16;
            fprintf(stderr, "error: %zu brokers running — pick one with -s:\n", n);
            for (size_t i = 0; i < shown; i++)
                fprintf(stderr, "  %s\n", paths[i]);
            return 1;
        }
        snprintf(sock, sizeof(sock), "%s", paths[0]);
    }

    int pid = -1;
    char err[256];
    int timeout = cli.timeout_ms > 0 ? cli.timeout_ms : 5000;
    if (sm_broker_stop_by_socket(sock, timeout, &pid, err, sizeof(err)) != 0) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    printf("Stopped broker pid %d (%s removed).\n", pid, sock);
    return 0;
}

static const subcmd_t subcmds[] = {
    {"send",       cmd_send,       1, "controller"},
    {"read",       cmd_read,       1, "observer"},
    {"write",      cmd_write,      1, "controller"},
    {"status",     cmd_status,     1, "observer"},
    {"boot-status", cmd_boot_status, 1, "observer"},
    {"history",    cmd_history,    1, "observer"},
    {"incidents",  cmd_incidents,  1, "observer"},
    {"list-ports", cmd_list_ports, 0, NULL},
    {"brokers",    cmd_brokers,    0, NULL},
    {"boards",     cmd_boards,     0, NULL},
    {"board",      cmd_board,      0, NULL},
    {"shutdown",   cmd_shutdown,   0, NULL},
    {"stop",       cmd_shutdown,   0, NULL},
    {"sysrq",      cmd_sysrq,     1, "controller"},
    {"break-uboot", cmd_break_uboot, 1, "controller"},
    {"pin",        cmd_pin,       1, "controller"},
    {"suspend",    cmd_suspend,   1, "controller"},
    {"resume",     cmd_resume,    1, "controller"},
    {"with-port",  cmd_with_port, 1, "controller"},
    {"watchdog",   cmd_watchdog,  1, "controller"},
    {"autorespond", cmd_autorespond, 1, "controller"},
    {"monitor",    cmd_monitor,   1, "observer"},
    {"report",     cmd_report,    1, "observer"},
    {NULL, NULL, 0, NULL}
};

/* --- Main --- */

int main(int argc, char *argv[])
{
    const char *socket_path = NULL;

    memset(&cli, 0, sizeof(cli));
    cli.sock_fd = -1;

    /* Parse global options — stop at first non-option (subcommand) */
    static const struct option long_opts[] = {
        {"socket",  required_argument, NULL, 's'},
        {"json",    no_argument,       NULL, 'j'},
        {"timeout", required_argument, NULL, 't'},
        {"verbose", no_argument,       NULL, 'v'},
        {"version", no_argument,       NULL, 'V'},
        {"help",    no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    /* Use '+' prefix to stop at first non-option */
    while ((opt = getopt_long(argc, argv, "+s:jt:vVh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 's': socket_path = optarg; g_socket_arg = optarg; break;
        case 'j': cli.json_output = 1; break;
        case 't': cli.timeout_ms = atoi(optarg); break;
        case 'v': cli.verbose = 1; break;
        case 'V':
            printf("%s-cli %s\n", SM_NAME, SM_VERSION);
            return 0;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "error: no command specified\n\n");
        usage(argv[0]);
        return 1;
    }

    /* Find subcommand */
    const char *cmd_name = argv[optind];
    const subcmd_t *cmd = NULL;
    for (int i = 0; subcmds[i].name; i++) {
        if (strcmp(subcmds[i].name, cmd_name) == 0) {
            cmd = &subcmds[i];
            break;
        }
    }

    if (!cmd) {
        fprintf(stderr, "error: unknown command '%s'\n\n", cmd_name);
        usage(argv[0]);
        return 1;
    }

    /* Set up signal handling */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* Connect to broker if needed */
    if (cmd->needs_broker) {
        char discovered[108];
        if (!socket_path) {
            if (sm_discover_socket(discovered, sizeof(discovered)) == 0) {
                socket_path = discovered;   /* g_socket_arg stays NULL: explicit -s only */
            } else {
                fprintf(stderr, "error: no broker socket found\n"
                        "  Use -s <path>, set $SMOLMUX_SOCKET, or start a broker\n");
                return 1;
            }
        }

        if (connect_and_hello(socket_path, cmd->role) != 0)
            return 1;
    }

    /* Dispatch — pass subcommand args (argv starting at the subcommand) */
    int sub_argc = argc - optind;
    char **sub_argv = argv + optind;
    int rc = cmd->func(sub_argc, sub_argv);

    /* Cleanup */
    if (cli.sock_fd >= 0)
        close(cli.sock_fd);

    return rc;
}
