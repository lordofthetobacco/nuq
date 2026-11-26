#include "bus.h"

#include <stdint.h>
#include <string.h>

#include <nng/nng.h>

#include "util.h"

/* NNG reports errors as return values, not through errno. The last failure
 * is kept here so bus_error() can describe it the way nn_errno() did. */
static nng_err last_err;

static int fail(int rv)
{
    last_err = (nng_err)rv;
    return -1;
}

/* NNG sockets are structs holding a positive id. The id is what callers see
 * as the socket number, so the bus API keeps using plain ints. */
static nng_socket sock_of(int id)
{
    nng_socket s = NNG_SOCKET_INITIALIZER;
    s.id = (uint32_t)id;
    return s;
}

/* NNG must be initialised before any other call. nng_init() may be called
 * again later, but once is enough for a process. */
static int lib_init(void)
{
    static int done;
    if (done) return 0;
    int rv = nng_init(NULL);
    if (rv != 0) return fail(rv);
    done = 1;
    return 0;
}

static int open_socket(int (*open_fn)(nng_socket *), nng_socket *s)
{
    if (lib_init() < 0) return -1;
    int rv = open_fn(s);
    return rv != 0 ? fail(rv) : 0;
}

static int bind_socket(int (*open_fn)(nng_socket *), const char *url)
{
    nng_socket s;
    if (open_socket(open_fn, &s) < 0) return -1;
    /* A vanished peer must never be able to stall the broker. */
    nng_socket_set_ms(s, NNG_OPT_SENDTIMEO, 1000);
    int rv = nng_listen(s, url, NULL, 0);
    if (rv != 0) {
        nng_socket_close(s);
        return fail(rv);
    }
    return nng_socket_id(s);
}

int bus_rep_bind(const char *url)
{
    return bind_socket(nng_rep0_open, url);
}

int bus_pub_bind(const char *url)
{
    return bind_socket(nng_pub0_open, url);
}

int bus_rcvfd(int sock)
{
    int fd = -1;
    int rv = nng_socket_get_recv_poll_fd(sock_of(sock), &fd);
    return rv != 0 ? fail(rv) : fd;
}

int bus_set_rcvtimeo(int sock, int timeout_ms)
{
    int rv = nng_socket_set_ms(sock_of(sock), NNG_OPT_RECVTIMEO, timeout_ms);
    return rv != 0 ? fail(rv) : 0;
}

static int connect_socket(int (*open_fn)(nng_socket *), const char *url, int timeout_ms)
{
    nng_socket s;
    if (open_socket(open_fn, &s) < 0) return -1;
    /* Replies can carry a whole inbox; never cap their size. Must be set
     * before dialing, since dialers copy it when they are created. */
    nng_socket_set_size(s, NNG_OPT_RECVMAXSZ, 0);
    if (timeout_ms >= 0) {
        nng_socket_set_ms(s, NNG_OPT_SENDTIMEO, timeout_ms);
        nng_socket_set_ms(s, NNG_OPT_RECVTIMEO, timeout_ms);
    }
    /* Non-blocking dial: like nn_connect, succeed even when the broker is
     * not listening yet and keep reconnecting in the background. */
    int rv = nng_dial(s, url, NULL, NNG_FLAG_NONBLOCK);
    if (rv != 0) {
        nng_socket_close(s);
        return fail(rv);
    }
    return nng_socket_id(s);
}

int bus_req_connect(const char *url, int timeout_ms)
{
    return connect_socket(nng_req0_open, url, timeout_ms);
}

int bus_sub_connect(const char *url)
{
    return connect_socket(nng_sub0_open, url, -1);
}

int bus_sub_add(int sock, const char *prefix)
{
    int rv = nng_sub0_socket_subscribe(sock_of(sock), prefix, strlen(prefix));
    return rv != 0 ? fail(rv) : 0;
}

char *bus_recv(int sock, int flags, size_t *len)
{
    nng_msg *msg = NULL;
    int rv = nng_recvmsg(sock_of(sock), &msg, (flags & BUS_DONTWAIT) ? NNG_FLAG_NONBLOCK : 0);
    if (rv != 0) {
        fail(rv);
        return NULL;
    }
    size_t n = nng_msg_len(msg);
    char *out = xmalloc(n + 1);
    memcpy(out, nng_msg_body(msg), n);
    out[n] = '\0';
    nng_msg_free(msg);
    if (len) *len = n;
    return out;
}

int bus_send(int sock, const char *buf, size_t len, int flags)
{
    int rv = nng_send(sock_of(sock), (void *)buf, len, (flags & BUS_DONTWAIT) ? NNG_FLAG_NONBLOCK : 0);
    return rv != 0 ? fail(rv) : 0;
}

void bus_close(int sock)
{
    if (sock >= 0) nng_socket_close(sock_of(sock));
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
    return nng_strerror(last_err);
}
