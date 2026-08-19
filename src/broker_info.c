#include "broker_info.h"   /* SO_PEERCRED/struct ucred via global _GNU_SOURCE */
#include "protocol.h"
#include "util/sock_util.h"
#include "util/json_helpers.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int send_json(int fd, cJSON *msg)
{
    size_t len;
    char *line = sm_msg_encode(msg, &len);
    cJSON_Delete(msg);
    if (!line) return -1;
    int rc = sm_write_all(fd, line, len);
    free(line);
    return rc;
}

static void parse_status(sm_broker_info_t *out, cJSON *resp)
{
    const char *board = sm_json_get_string(resp, "board");
    snprintf(out->board, sizeof(out->board), "%s", board ? board : "");
    const char *role = sm_json_get_string(resp, "role");
    snprintf(out->role, sizeof(out->role), "%s", role ? role : "");

    const char *lt = sm_json_get_string(resp, "link_type");
    snprintf(out->link_type, sizeof(out->link_type), "%s", lt ? lt : "?");

    /* GDB reports its device via "target"; UART via "port". */
    const char *ep;
    if (strcmp(out->link_type, "gdb") == 0)
        ep = sm_json_get_string(resp, "target");
    else
        ep = sm_json_get_string(resp, "port");
    snprintf(out->endpoint, sizeof(out->endpoint), "%s", ep ? ep : "?");

    /* Baud is meaningful only for UART links; a GDB link's broker still carries
     * a default baudrate, so report 0 (n/a) rather than a misleading value. */
    out->baud = strcmp(out->link_type, "uart") == 0
                    ? sm_json_get_int(resp, "baud", 0) : 0;
    out->connected = sm_json_get_bool(resp, "connected", 0);
    out->suspended = sm_json_get_bool(resp, "suspended", 0);

    /* The probe itself is a connected client at status time; report only the
     * *other* clients so the listing answers "is someone already on this?". */
    cJSON *clients = cJSON_GetObjectItemCaseSensitive(resp, "clients");
    int n = cJSON_IsArray(clients) ? cJSON_GetArraySize(clients) : 0;
    out->client_count = n > 0 ? n - 1 : 0;
}

int sm_broker_probe(const char *socket_path, sm_broker_info_t *out,
                    int timeout_ms)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->socket, sizeof(out->socket), "%s", socket_path);
    snprintf(out->link_type, sizeof(out->link_type), "?");
    snprintf(out->endpoint, sizeof(out->endpoint), "?");
    out->pid = -1;

    int fd = sm_connect_unix(socket_path);
    if (fd < 0) return -1;

    /* The broker is the peer of this connection, so SO_PEERCRED yields its pid
     * without any pidfile — keeps teardown fully discovery-driven. */
    struct ucred cred;
    socklen_t clen = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &clen) == 0)
        out->pid = (int)cred.pid;

    /* An observer hello does not disturb the device; then ask for status. */
    if (send_json(fd, sm_msg_hello("smolmux-probe", "observer")) < 0 ||
        send_json(fd, sm_msg_status("probe")) < 0) {
        close(fd);
        return -1;
    }

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }

    char buf[8192];
    size_t len = 0;
    int found = 0;

    while (!found) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long remain = (deadline.tv_sec - now.tv_sec) * 1000 +
                      (deadline.tv_nsec - now.tv_nsec) / 1000000;
        if (remain <= 0) break;

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, (int)remain);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) break;
        if (pfd.revents & (POLLHUP | POLLERR)) break;

        ssize_t n = read(fd, buf + len, sizeof(buf) - len - 1);
        if (n <= 0) break;
        len += (size_t)n;

        char *start = buf, *nl;
        while ((nl = memchr(start, '\n', len - (size_t)(start - buf))) != NULL) {
            sm_msg_t m = sm_msg_decode(start, (size_t)(nl - start));
            start = nl + 1;
            if (m.root && m.type == SM_MSG_STATUS_RESPONSE) {
                parse_status(out, m.root);
                out->reachable = 1;
                found = 1;
                sm_msg_free(&m);
                break;
            }
            sm_msg_free(&m);
        }
        size_t remaining = len - (size_t)(start - buf);
        if (remaining > 0 && start != buf)
            memmove(buf, start, remaining);
        len = remaining;
    }

    close(fd);
    return found ? 0 : -1;
}

cJSON *sm_broker_info_to_json(const sm_broker_info_t *info)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "socket", info->socket);
    cJSON_AddBoolToObject(o, "reachable", info->reachable);
    if (info->reachable) {
        cJSON_AddStringToObject(o, "board", info->board);
        cJSON_AddStringToObject(o, "role", info->role);
        cJSON_AddStringToObject(o, "link_type", info->link_type);
        cJSON_AddStringToObject(o, "endpoint", info->endpoint);
        cJSON_AddNumberToObject(o, "baud", info->baud);
        cJSON_AddBoolToObject(o, "connected", info->connected);
        cJSON_AddBoolToObject(o, "suspended", info->suspended);
        cJSON_AddNumberToObject(o, "clients", info->client_count);
        if (info->pid > 0)
            cJSON_AddNumberToObject(o, "pid", info->pid);
    }
    return o;
}

size_t sm_broker_discover(sm_broker_info_t *out, size_t max, int timeout_ms)
{
    char socks[128][SM_SOCK_PATH_MAX];
    size_t n = sm_discover_all_sockets(socks, 128);
    size_t probe_n = n < max ? n : max;
    if (probe_n > 128) probe_n = 128;
    for (size_t i = 0; i < probe_n; i++)
        sm_broker_probe(socks[i], &out[i], timeout_ms);
    return n;
}

void sm_broker_info_format(const sm_broker_info_t *info, char *buf, size_t len)
{
    if (!info->reachable) {
        snprintf(buf, len, "%-40s  (unreachable)", info->socket);
        return;
    }

    const char *label = strcmp(info->link_type, "uart") == 0 ? "UART"
                      : strcmp(info->link_type, "gdb") == 0 ? "GDB "
                      : "?   ";

    char endpoint[192];
    if (strcmp(info->link_type, "uart") == 0 && info->baud > 0)
        snprintf(endpoint, sizeof(endpoint), "%s @%d", info->endpoint, info->baud);
    else
        snprintf(endpoint, sizeof(endpoint), "%s", info->endpoint);

    char board[128] = "";
    if (info->board[0]) {
        if (info->role[0])
            snprintf(board, sizeof(board), "  board=%s/%s", info->board, info->role);
        else
            snprintf(board, sizeof(board), "  board=%s", info->board);
    }

    snprintf(buf, len, "%-40s  %s  %-26s  %s%s  %d client%s%s",
             info->socket, label, endpoint,
             info->connected ? "up" : "down",
             info->suspended ? " (suspended)" : "",
             info->client_count, info->client_count == 1 ? "" : "s", board);
}
