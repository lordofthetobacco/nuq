#include "bus.h"

#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <sddl.h>
#include <stdio.h>
#else
#include <poll.h>
#endif

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

#ifdef _WIN32
/* The broker's endpoints are named pipes on Windows. NNG calls those
 * winpipe://; its ipc:// is a Unix socket file there, which the access
 * control below would not cover. Callers keep saying ipc://. */
static const char *pipe_url(const char *url, char *buf, size_t len)
{
    if (strncmp(url, "ipc://", 6) != 0) return url;
    snprintf(buf, len, "winpipe://%s", url + 6);
    return buf;
}

/* A named pipe may by default be opened for reading by every local account.
 * Only this user (and SYSTEM) may use ours, the way the private runtime
 * directory guards the Unix sockets. NNG keeps the pointer, so the
 * descriptor lives as long as the process. */
static PSECURITY_DESCRIPTOR pipe_security(void)
{
    static PSECURITY_DESCRIPTOR sd;
    static int done;
    if (done) return sd;
    done = 1;
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return NULL;
    union {
        TOKEN_USER user;
        char buf[256];
    } info;
    DWORD n = 0;
    char *sid = NULL;
    BOOL ok = GetTokenInformation(tok, TokenUser, &info, sizeof info, &n) &&
              ConvertSidToStringSidA(info.user.User.Sid, &sid);
    CloseHandle(tok);
    if (!ok) return NULL;
    char sddl[256];
    snprintf(sddl, sizeof sddl, "D:P(A;;GA;;;%s)(A;;GA;;;SY)", sid);
    LocalFree(sid);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl, SDDL_REVISION_1, &sd, NULL)) sd = NULL;
    return sd;
}
#endif

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
#ifdef _WIN32
    char pipe[256];
    nng_listener l;
    PSECURITY_DESCRIPTOR sd = pipe_security();
    int rv = sd ? nng_listener_create(&l, s, pipe_url(url, pipe, sizeof pipe)) : NNG_EPERM;
    if (rv == 0) rv = nng_listener_set_security_descriptor(l, sd);
    if (rv == 0) rv = nng_listener_start(l, 0);
#else
    int rv = nng_listen(s, url, NULL, 0);
#endif
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

int bus_wait(int sock, int timeout_ms)
{
#ifdef _WIN32
    /* NNG's descriptor is a loopback SOCKET here, which only select() takes. */
    int rcv = bus_rcvfd(sock);
    if (rcv < 0) return -1;
    SOCKET fd = (SOCKET)rcv;
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(fd, &rd);
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    int n = select(0, &rd, NULL, NULL, timeout_ms < 0 ? NULL : &tv);
#else
    struct pollfd pfd = {bus_rcvfd(sock), POLLIN, 0};
    if (pfd.fd < 0) return -1;
    int n = poll(&pfd, 1, timeout_ms);
#endif
    return n < 0 ? -1 : n > 0;
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
#ifdef _WIN32
    char pipe[256];
    url = pipe_url(url, pipe, sizeof pipe);
#endif
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
