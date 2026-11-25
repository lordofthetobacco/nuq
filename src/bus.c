#include "bus.h"

#include <string.h>

#ifdef NQ_NN_INSTALLED_HEADERS
#include <nanomsg/nn.h>
#include <nanomsg/pubsub.h>
#include <nanomsg/reqrep.h>
#else
#include "nn.h"
#include "pubsub.h"
#include "reqrep.h"
#endif

#include "util.h"

static int bind_socket(int proto, const char *url)
{
    int s = nn_socket(AF_SP, proto);
    if (s < 0) return -1;
    /* A vanished peer must never be able to stall the broker. */
    int to = 1000;
    nn_setsockopt(s, NN_SOL_SOCKET, NN_SNDTIMEO, &to, sizeof to);
    if (nn_bind(s, url) < 0) {
        nn_close(s);
        return -1;
    }
    return s;
}

int bus_rep_bind(const char *url)
{
    return bind_socket(NN_REP, url);
}

int bus_pub_bind(const char *url)
{
    return bind_socket(NN_PUB, url);
}

int bus_rcvfd(int sock)
{
    int fd = -1;
    size_t len = sizeof fd;
    if (nn_getsockopt(sock, NN_SOL_SOCKET, NN_RCVFD, &fd, &len) < 0) return -1;
    return fd;
}

int bus_set_rcvtimeo(int sock, int timeout_ms)
{
    return nn_setsockopt(sock, NN_SOL_SOCKET, NN_RCVTIMEO, &timeout_ms, sizeof timeout_ms);
}

static int connect_socket(int proto, const char *url, int timeout_ms)
{
    int s = nn_socket(AF_SP, proto);
    if (s < 0) return -1;
    /* Replies can carry a whole inbox; the 1 MiB default would truncate. */
    int unlimited = -1;
    nn_setsockopt(s, NN_SOL_SOCKET, NN_RCVMAXSIZE, &unlimited, sizeof unlimited);
    if (timeout_ms >= 0) {
        nn_setsockopt(s, NN_SOL_SOCKET, NN_SNDTIMEO, &timeout_ms, sizeof timeout_ms);
        nn_setsockopt(s, NN_SOL_SOCKET, NN_RCVTIMEO, &timeout_ms, sizeof timeout_ms);
    }
    if (nn_connect(s, url) < 0) {
        nn_close(s);
        return -1;
    }
    return s;
}

int bus_req_connect(const char *url, int timeout_ms)
{
    return connect_socket(NN_REQ, url, timeout_ms);
}

int bus_sub_connect(const char *url)
{
    return connect_socket(NN_SUB, url, -1);
}

int bus_sub_add(int sock, const char *prefix)
{
    return nn_setsockopt(sock, NN_SUB, NN_SUB_SUBSCRIBE, prefix, strlen(prefix));
}

char *bus_recv(int sock, int flags, size_t *len)
{
    void *buf = NULL;
    int n = nn_recv(sock, &buf, NN_MSG, (flags & BUS_DONTWAIT) ? NN_DONTWAIT : 0);
    if (n < 0) return NULL;
    char *out = xmalloc((size_t)n + 1);
    memcpy(out, buf, (size_t)n);
    out[n] = '\0';
    nn_freemsg(buf);
    if (len) *len = (size_t)n;
    return out;
}

int bus_send(int sock, const char *buf, size_t len, int flags)
{
    return nn_send(sock, buf, len, (flags & BUS_DONTWAIT) ? NN_DONTWAIT : 0) < 0 ? -1 : 0;
}

void bus_close(int sock)
{
    if (sock >= 0) nn_close(sock);
}

char *bus_request(const char *url, const char *req, int timeout_ms)
{
    int s = bus_req_connect(url, timeout_ms);
    if (s < 0) return NULL;
    char *reply = NULL;
    if (bus_send(s, req, strlen(req), 0) == 0) reply = bus_recv(s, 0, NULL);
    bus_close(s);
    return reply;
}

const char *bus_error(void)
{
    return nn_strerror(nn_errno());
}
