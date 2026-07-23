#include "sinks/tcp.h"
#include "broker.h"
#include "logger.h"
#include "constants.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define LOG_TAG "tcp"

typedef struct sm_tcp_sink {
    sm_sink_t base;
    sm_broker_t *broker;
    int listen_fd;
    int port;
    struct in_addr bind_addr;
} sm_tcp_sink_t;

static int tcp_start(sm_sink_t *self, void *broker)
{
    sm_tcp_sink_t *tcp = (sm_tcp_sink_t *)self;
    tcp->broker = broker;

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
    addr.sin_addr = tcp->bind_addr;
    addr.sin_port = htons((uint16_t)tcp->port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        SM_LOG_ERROR(LOG_TAG, "bind port %d: %s", tcp->port, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 8) < 0) {
        SM_LOG_ERROR(LOG_TAG, "listen: %s", strerror(errno));
        close(fd);
        return -1;
    }

    /* Set non-blocking for epoll */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    tcp->listen_fd = fd;
    self->fd = fd;

    SM_LOG_INFO(LOG_TAG, "listening on port %d", tcp->port);
    return 0;
}

static void tcp_on_readable(sm_sink_t *self)
{
    sm_tcp_sink_t *tcp = (sm_tcp_sink_t *)self;

    for (;;) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int fd = accept4(tcp->listen_fd, (struct sockaddr *)&peer, &peer_len, SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            SM_LOG_WARN(LOG_TAG, "accept: %s", strerror(errno));
            break;
        }

        char addr_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &peer.sin_addr, addr_str, sizeof(addr_str));
        SM_LOG_INFO(LOG_TAG, "connection from %s:%d", addr_str, ntohs(peer.sin_port));

        sm_client_t *c = sm_broker_register_client(tcp->broker, fd);
        if (c)
            c->requires_auth = 1;  /* network origin — token enforced if set */
    }
}

static void tcp_stop(sm_sink_t *self)
{
    sm_tcp_sink_t *tcp = (sm_tcp_sink_t *)self;
    if (tcp->listen_fd >= 0) {
        close(tcp->listen_fd);
        tcp->listen_fd = -1;
        self->fd = -1;
    }
    SM_LOG_INFO(LOG_TAG, "stopped");
}

static void tcp_destroy(sm_sink_t *self)
{
    free(self);
}

sm_sink_t *sm_tcp_sink_new(int port, const char *bind_addr)
{
    sm_tcp_sink_t *tcp = calloc(1, sizeof(*tcp));
    tcp->base.name = "tcp";
    tcp->base.fd = -1;
    tcp->base.start = tcp_start;
    tcp->base.on_readable = tcp_on_readable;
    tcp->base.stop = tcp_stop;
    tcp->base.destroy = tcp_destroy;
    tcp->listen_fd = -1;
    tcp->port = port;
    if (!bind_addr || inet_pton(AF_INET, bind_addr, &tcp->bind_addr) != 1)
        tcp->bind_addr.s_addr = htonl(INADDR_LOOPBACK);
    return &tcp->base;
}
