#include "sinks/ws.h"
#include "broker.h"
#include "logger.h"
#include "constants.h"
#include "util/sha1.h"
#include "util/base64.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define LOG_TAG "ws"
#define WS_GUID "258EAFA5-E914-47DA-95CA-5AB5443F11F3"

/* WebSocket opcodes */
#define WS_OP_TEXT  0x1
#define WS_OP_CLOSE 0x8
#define WS_OP_PING  0x9
#define WS_OP_PONG  0xA

typedef struct sm_ws_client {
    int ws_fd;         /* TCP socket to browser */
    int bridge_fd;     /* our end of socketpair */
    uint8_t ws_buf[SM_WS_READ_BUF_SIZE];
    size_t ws_buf_len;
    uint8_t br_buf[SM_WS_READ_BUF_SIZE];
    size_t br_buf_len;
    int closing;
} sm_ws_client_t;

typedef struct sm_ws_sink {
    sm_sink_t base;
    sm_broker_t *broker;
    int listen_fd;
    int port;
    pthread_t thread;
    int shutdown_pipe[2];
    sm_ws_client_t clients[SM_WS_MAX_CLIENTS];
    int client_count;
} sm_ws_sink_t;

/* --- WebSocket framing --- */

/*
 * Decode one WebSocket frame from buf[0..len).
 * Returns total frame bytes consumed, 0 if incomplete, -1 on error (oversize).
 * Sets *opcode, *payload, *payload_len on success.
 */
static ssize_t ws_decode_frame(uint8_t *buf, size_t len,
                               int *opcode, const uint8_t **payload,
                               size_t *payload_len)
{
    if (len < 2) return 0;

    *opcode = buf[0] & 0x0F;
    int masked = (buf[1] >> 7) & 1;
    uint64_t plen = buf[1] & 0x7F;
    size_t hdr = 2;

    if (plen == 126) {
        if (len < 4) return 0;
        plen = ((uint64_t)buf[2] << 8) | buf[3];
        hdr = 4;
    } else if (plen == 127) {
        if (len < 10) return 0;
        plen = 0;
        for (int i = 0; i < 8; i++)
            plen = (plen << 8) | buf[2 + i];
        hdr = 10;
    }

    /* Bound plen first so the total below can't overflow. */
    if (plen > SM_WS_READ_BUF_SIZE) return -1;

    size_t mask_len = masked ? 4 : 0;
    size_t total = hdr + mask_len + (size_t)plen;
    /* Reject frames whose full size (header + mask + payload) can never fit
     * ws_buf: otherwise a payload in (BUF-hdr-mask, BUF] passes the plen check
     * yet never fully buffers, the read stalls at 0 bytes, and the connection
     * is misread as EOF. -1 signals error (vs 0 for incomplete). */
    if (total > SM_WS_READ_BUF_SIZE) return -1;
    if (len < total) return 0;

    if (masked) {
        const uint8_t *mask = buf + hdr;
        /* Unmask in-place */
        uint8_t *p = buf + hdr + 4;
        for (size_t i = 0; i < (size_t)plen; i++)
            p[i] ^= mask[i & 3];
        *payload = p;
    } else {
        *payload = buf + hdr;
    }
    *payload_len = (size_t)plen;
    return total;
}

/*
 * Encode a WebSocket text frame into out. Returns bytes written.
 * Server-to-client frames are not masked.
 */
static size_t ws_encode_frame(uint8_t *out, size_t out_cap,
                              int opcode, const uint8_t *data, size_t len)
{
    size_t hdr;
    if (len < 126)      hdr = 2;
    else if (len < 65536) hdr = 4;
    else                 hdr = 10;

    if (hdr + len > out_cap) return 0;

    out[0] = (uint8_t)(0x80 | (opcode & 0x0F)); /* FIN + opcode */
    if (len < 126) {
        out[1] = (uint8_t)len;
    } else if (len < 65536) {
        out[1] = 126;
        out[2] = (uint8_t)(len >> 8);
        out[3] = (uint8_t)(len);
    } else {
        out[1] = 127;
        for (int i = 0; i < 8; i++)
            out[2 + i] = (uint8_t)(len >> (56 - 8 * i));
    }

    memcpy(out + hdr, data, len);
    return hdr + len;
}

/* --- WebSocket handshake --- */

/*
 * Find header value in HTTP request. Returns pointer into buf or NULL.
 * Sets *vlen to value length (trimmed of leading space).
 */
static const char *find_header(const char *buf, const char *name, size_t *vlen)
{
    const char *p = buf;
    size_t nlen = strlen(name);
    while ((p = strstr(p, name)) != NULL) {
        /* Check it's at start of line */
        if (p != buf && *(p - 1) != '\n') { p += nlen; continue; }
        p += nlen;
        if (*p == ':') {
            p++;
            while (*p == ' ') p++;
            const char *end = strstr(p, "\r\n");
            if (!end) end = p + strlen(p);
            *vlen = (size_t)(end - p);
            return p;
        }
    }
    return NULL;
}

/*
 * Perform WebSocket handshake on fd.
 * Returns 0 on success, -1 on failure.
 */
/* NOTE: this handshake is synchronous within the single WS thread, so a local
 * client that connects and never completes the request holds the thread for up
 * to the 5s budget below (bounded, but repeated stalls can starve WS delivery).
 * A full fix tracks per-connection handshake state in the poll set; deferred
 * deliberately — the listener binds loopback only, so this is a local-only DoS,
 * and the rewrite's concurrency risk outweighs the benefit for that threat.
 * Tracked: docs/issue-ws-hardening-deferred.md (ISSUE-WS-1). */
static int ws_handshake(int fd)
{
    char buf[4096];
    size_t total = 0;

    /* Read HTTP request with timeout */
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    for (int i = 0; i < 50; i++) {
        if (poll(&pfd, 1, 100) <= 0) continue;
        ssize_t n = read(fd, buf + total, sizeof(buf) - total - 1);
        if (n <= 0) return -1;
        total += (size_t)n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }

    if (!strstr(buf, "\r\n\r\n")) return -1;

    /* Validate HTTP upgrade request (RFC 6455 §4.2.1) */
    if (strncmp(buf, "GET ", 4) != 0) return -1;

    size_t upgrade_len;
    const char *upgrade = find_header(buf, "Upgrade", &upgrade_len);
    if (!upgrade || upgrade_len < 9 || strncasecmp(upgrade, "websocket", 9) != 0)
        return -1;

    size_t conn_len;
    const char *conn = find_header(buf, "Connection", &conn_len);
    if (!conn) return -1;
    /* Check that "Upgrade" appears in Connection header value */
    {
        int found_upgrade = 0;
        for (size_t ci = 0; ci + 7 <= conn_len; ci++) {
            if (strncasecmp(conn + ci, "Upgrade", 7) == 0)
                { found_upgrade = 1; break; }
        }
        if (!found_upgrade) return -1;
    }

    /* Reject cross-origin requests (CSWSH prevention). Only an Origin that is
     * present and non-localhost is rejected: browsers always send Origin, so a
     * malicious page is caught here, while a missing Origin means a non-browser
     * local client (websocat, the monitor), which is not a CSWSH vector. The
     * listener binds loopback only, so failing open on absent Origin does not
     * widen exposure. Tracked: docs/issue-ws-hardening-deferred.md (ISSUE-WS-2). */
    size_t origin_len;
    const char *origin = find_header(buf, "Origin", &origin_len);
    if (origin) {
        /* Only allow localhost origins */
        int allowed = 0;
        if (origin_len >= 16 && strncmp(origin, "http://localhost", 16) == 0 &&
            (origin_len == 16 || origin[16] == '/' || origin[16] == ':'))
            allowed = 1;
        if (origin_len >= 16 && strncmp(origin, "http://127.0.0.1", 16) == 0 &&
            (origin_len == 16 || origin[16] == '/' || origin[16] == ':'))
            allowed = 1;
        if (origin_len >= 12 && strncmp(origin, "http://[::1]", 12) == 0 &&
            (origin_len == 12 || origin[12] == '/' || origin[12] == ':'))
            allowed = 1;
        if (!allowed) {
            SM_LOG_WARN("ws", "rejected WebSocket connection from origin: %.*s",
                        (int)origin_len, origin);
            return -1;
        }
    }

    /* Extract Sec-WebSocket-Key */
    size_t klen;
    const char *key = find_header(buf, "Sec-WebSocket-Key", &klen);
    if (!key || klen < 16 || klen > 128) return -1;

    /* Compute accept hash: SHA1(key + GUID) then base64 */
    char concat[256];
    size_t clen = (size_t)snprintf(concat, sizeof(concat), "%.*s%s",
                                    (int)klen, key, WS_GUID);

    uint8_t hash[SM_SHA1_DIGEST_SIZE];
    sm_sha1((const uint8_t *)concat, clen, hash);

    char *accept_b64 = sm_base64_encode(hash, SM_SHA1_DIGEST_SIZE);
    if (!accept_b64) return -1;

    /* Send 101 response */
    char resp[512];
    int rlen = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n", accept_b64);
    free(accept_b64);

    ssize_t written = 0;
    while (written < rlen) {
        ssize_t w = write(fd, resp + written, (size_t)(rlen - written));
        if (w <= 0) return -1;
        written += w;
    }

    return 0;
}

/* --- WS client management --- */

static void ws_remove_client(sm_ws_sink_t *ws, int idx)
{
    sm_ws_client_t *c = &ws->clients[idx];
    SM_LOG_INFO(LOG_TAG, "closing ws client (ws_fd=%d)", c->ws_fd);
    if (c->ws_fd >= 0) close(c->ws_fd);
    if (c->bridge_fd >= 0) close(c->bridge_fd);
    /* Compact array */
    ws->clients[idx] = ws->clients[ws->client_count - 1];
    ws->client_count--;
}

/* Write all bytes or return -1 */
static int write_all(int fd, const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = {.fd = fd, .events = POLLOUT};
                poll(&pfd, 1, 100);
                continue;
            }
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

/* Write payload + newline atomically to bridge fd */
static int write_line(int fd, const uint8_t *data, size_t len)
{
    uint8_t buf[SM_WS_READ_BUF_SIZE + 1];
    if (len + 1 > sizeof(buf)) return -1;
    memcpy(buf, data, len);
    buf[len] = '\n';
    return write_all(fd, buf, len + 1);
}

/* --- Thread --- */

static void *ws_thread(void *arg)
{
    sm_ws_sink_t *ws = arg;

    while (1) {
        /* Build poll set: shutdown_pipe + listen_fd + per-client (ws_fd, bridge_fd) */
        struct pollfd fds[2 + SM_WS_MAX_CLIENTS * 2];
        int nfds = 0;

        fds[nfds].fd = ws->shutdown_pipe[0];
        fds[nfds].events = POLLIN;
        nfds++;

        fds[nfds].fd = ws->listen_fd;
        fds[nfds].events = POLLIN;
        nfds++;

        for (int i = 0; i < ws->client_count; i++) {
            fds[nfds].fd = ws->clients[i].ws_fd;
            fds[nfds].events = POLLIN;
            nfds++;
            fds[nfds].fd = ws->clients[i].bridge_fd;
            fds[nfds].events = POLLIN;
            nfds++;
        }

        int ret = poll(fds, (nfds_t)nfds, 200);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* Check shutdown */
        if (fds[0].revents & POLLIN)
            break;

        /* Snapshot client count — only process clients that were in the poll set.
         * Accepting new connections below may increment client_count, but those
         * new clients' fds were not polled and their revents are uninitialized. */
        int poll_client_count = ws->client_count;

        /* Check listener for new connections */
        if (fds[1].revents & POLLIN) {
            struct sockaddr_in peer;
            socklen_t peer_len = sizeof(peer);
            int cfd = accept4(ws->listen_fd, (struct sockaddr *)&peer, &peer_len, SOCK_CLOEXEC);
            if (cfd >= 0) {
                if (ws->client_count >= SM_WS_MAX_CLIENTS) {
                    SM_LOG_WARN(LOG_TAG, "max ws clients reached");
                    close(cfd);
                } else if (ws_handshake(cfd) != 0) {
                    SM_LOG_WARN(LOG_TAG, "ws handshake failed");
                    close(cfd);
                } else {
                    /* Create socketpair bridge */
                    int pair[2];
                    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) < 0) {
                        SM_LOG_ERROR(LOG_TAG, "socketpair: %s", strerror(errno));
                        close(cfd);
                    } else {
                        /* Register broker_fd (pair[0]) with broker (thread-safe).
                         * pair[0] ownership transfers to the broker; the WS thread
                         * only touches pair[1] (bridge_fd). If the WS client
                         * disconnects before the broker reads pair[0] from the pipe,
                         * closing pair[1] causes EPOLLHUP on pair[0] which the
                         * broker handles via normal client removal. */
                        sm_broker_register_client_async(ws->broker, pair[0]);

                        /* Set bridge_fd non-blocking */
                        int fl = fcntl(pair[1], F_GETFL, 0);
                        fcntl(pair[1], F_SETFL, fl | O_NONBLOCK);

                        /* Set ws_fd non-blocking */
                        fl = fcntl(cfd, F_GETFL, 0);
                        fcntl(cfd, F_SETFL, fl | O_NONBLOCK);

                        sm_ws_client_t *wc = &ws->clients[ws->client_count++];
                        memset(wc, 0, sizeof(*wc));
                        wc->ws_fd = cfd;
                        wc->bridge_fd = pair[1];

                        char addr_str[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET, &peer.sin_addr, addr_str, sizeof(addr_str));
                        SM_LOG_INFO(LOG_TAG, "ws client from %s:%d",
                                    addr_str, ntohs(peer.sin_port));
                    }
                }
            }
        }

        /* Process client fds (indices 2..nfds in pairs of ws_fd, bridge_fd) */
        int fi = 2;
        for (int i = 0; i < poll_client_count; i++) {
            sm_ws_client_t *c = &ws->clients[i];
            int removed = 0;

            /* ws_fd readable: decode WS frame → write JSON to bridge_fd */
            if (fds[fi].revents & (POLLIN | POLLHUP | POLLERR)) {
                if (fds[fi].revents & POLLIN) {
                    ssize_t n = read(c->ws_fd, c->ws_buf + c->ws_buf_len,
                                     sizeof(c->ws_buf) - c->ws_buf_len);
                    if (n <= 0) {
                        ws_remove_client(ws, i);
                        i--; fi += 2; continue;
                    }
                    c->ws_buf_len += (size_t)n;

                    /* Process all complete frames */
                    size_t off = 0;
                    while (off < c->ws_buf_len) {
                        int op;
                        const uint8_t *payload;
                        size_t plen;
                        ssize_t consumed = ws_decode_frame(c->ws_buf + off,
                                                    c->ws_buf_len - off,
                                                    &op, &payload, &plen);
                        if (consumed == 0) break;
                        if (consumed < 0) {
                            /* Oversize frame — send close 1009 and disconnect */
                            uint8_t close_payload[2] = {0x03, 0xF1}; /* 1009 */
                            uint8_t frame[128];
                            size_t flen = ws_encode_frame(frame, sizeof(frame),
                                                          WS_OP_CLOSE, close_payload, 2);
                            if (flen > 0)
                                write_all(c->ws_fd, frame, flen);
                            removed = 1;
                            break;
                        }

                        if (op == WS_OP_TEXT) {
                            /* Forward JSON line to bridge (atomic write) */
                            if (write_line(c->bridge_fd, payload, plen) < 0) {
                                removed = 1;
                                break;
                            }
                        } else if (op == WS_OP_PING) {
                            uint8_t frame[SM_WS_READ_BUF_SIZE];
                            size_t flen = ws_encode_frame(frame, sizeof(frame),
                                                          WS_OP_PONG, payload, plen);
                            if (flen > 0)
                                write_all(c->ws_fd, frame, flen);
                        } else if (op == WS_OP_CLOSE) {
                            removed = 1;
                            break;
                        }
                        off += (size_t)consumed;
                    }
                    /* Shift remaining data */
                    if (off > 0 && off < c->ws_buf_len) {
                        memmove(c->ws_buf, c->ws_buf + off, c->ws_buf_len - off);
                    }
                    c->ws_buf_len -= off;
                } else {
                    /* HUP/ERR on ws_fd */
                    removed = 1;
                }
            }

            if (removed) {
                ws_remove_client(ws, i);
                i--; fi += 2; continue;
            }

            /* bridge_fd readable: read JSON line → encode as WS frame → write to ws_fd */
            if (fds[fi + 1].revents & (POLLIN | POLLHUP | POLLERR)) {
                if (fds[fi + 1].revents & POLLIN) {
                    ssize_t n = read(c->bridge_fd, c->br_buf + c->br_buf_len,
                                     sizeof(c->br_buf) - c->br_buf_len);
                    if (n <= 0) {
                        ws_remove_client(ws, i);
                        i--; fi += 2; continue;
                    }
                    c->br_buf_len += (size_t)n;

                    /* Process complete lines */
                    size_t off = 0;
                    while (off < c->br_buf_len) {
                        uint8_t *nl = memchr(c->br_buf + off, '\n',
                                             c->br_buf_len - off);
                        if (!nl) break;

                        size_t line_len = (size_t)(nl - (c->br_buf + off));
                        /* Send JSON (without newline) as WS text frame */
                        uint8_t frame[SM_WS_READ_BUF_SIZE + 14];
                        size_t flen = ws_encode_frame(frame, sizeof(frame),
                                                      WS_OP_TEXT,
                                                      c->br_buf + off, line_len);
                        if (flen > 0) {
                            if (write_all(c->ws_fd, frame, flen) < 0) {
                                removed = 1;
                                break;
                            }
                        }
                        off = (size_t)(nl - c->br_buf) + 1;
                    }

                    if (removed) {
                        ws_remove_client(ws, i);
                        i--; fi += 2; continue;
                    }

                    if (off > 0 && off < c->br_buf_len)
                        memmove(c->br_buf, c->br_buf + off, c->br_buf_len - off);
                    c->br_buf_len -= off;
                } else {
                    /* HUP/ERR on bridge_fd */
                    ws_remove_client(ws, i);
                    i--; fi += 2; continue;
                }
            }

            fi += 2;
        }
    }

    /* Cleanup all clients */
    while (ws->client_count > 0)
        ws_remove_client(ws, 0);

    return NULL;
}

/* --- Sink callbacks --- */

static int ws_start(sm_sink_t *self, void *broker)
{
    sm_ws_sink_t *ws = (sm_ws_sink_t *)self;
    ws->broker = broker;

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        SM_LOG_ERROR(LOG_TAG, "socket: %s", strerror(errno));
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)ws->port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        SM_LOG_ERROR(LOG_TAG, "bind port %d: %s", ws->port, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 8) < 0) {
        SM_LOG_ERROR(LOG_TAG, "listen: %s", strerror(errno));
        close(fd);
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    ws->listen_fd = fd;
    /* fd = -1 for sink: thread owns the listener, not epoll */
    self->fd = -1;

    if (pipe2(ws->shutdown_pipe, O_CLOEXEC) < 0) {
        SM_LOG_ERROR(LOG_TAG, "pipe: %s", strerror(errno));
        close(fd);
        return -1;
    }

    if (pthread_create(&ws->thread, NULL, ws_thread, ws) != 0) {
        SM_LOG_ERROR(LOG_TAG, "pthread_create: %s", strerror(errno));
        close(fd);
        close(ws->shutdown_pipe[0]);
        close(ws->shutdown_pipe[1]);
        return -1;
    }

    SM_LOG_INFO(LOG_TAG, "listening on port %d", ws->port);
    return 0;
}

static void ws_stop(sm_sink_t *self)
{
    sm_ws_sink_t *ws = (sm_ws_sink_t *)self;
    /* Signal thread to exit */
    uint8_t b = 1;
    if (write(ws->shutdown_pipe[1], &b, 1) < 0)
        SM_LOG_WARN(LOG_TAG, "shutdown write: %s", strerror(errno));
    pthread_join(ws->thread, NULL);
    close(ws->shutdown_pipe[0]);
    close(ws->shutdown_pipe[1]);
    if (ws->listen_fd >= 0) {
        close(ws->listen_fd);
        ws->listen_fd = -1;
    }
    SM_LOG_INFO(LOG_TAG, "stopped");
}

static void ws_destroy(sm_sink_t *self)
{
    free(self);
}

sm_sink_t *sm_ws_sink_new(int port)
{
    sm_ws_sink_t *ws = calloc(1, sizeof(*ws));
    ws->base.name = "ws";
    ws->base.fd = -1;
    ws->base.start = ws_start;
    ws->base.stop = ws_stop;
    ws->base.destroy = ws_destroy;
    ws->listen_fd = -1;
    ws->port = port;
    ws->shutdown_pipe[0] = -1;
    ws->shutdown_pipe[1] = -1;
    return &ws->base;
}
