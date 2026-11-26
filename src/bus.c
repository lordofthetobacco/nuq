#include "bus.h"

#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <sddl.h>
#else
#include <poll.h>
#endif

#ifdef NQ_NN_INSTALLED_HEADERS
#include <nanomsg/nn.h>
#include <nanomsg/pubsub.h>
#include <nanomsg/reqrep.h>
#else
#include "nn.h"
#include "pubsub.h"
#include "reqrep.h"
#endif
#ifdef _WIN32
#ifdef NQ_NN_INSTALLED_HEADERS
#include <nanomsg/ipc.h>
#else
#include "ipc.h"
#endif
#endif

#include "util.h"

#ifdef _WIN32
/* nanomsg's ipc:// is a named pipe, which by default every local account
 * may open for reading. Only this user (and SYSTEM) may use ours, the way the
 * private runtime directory guards the Unix sockets. nanomsg keeps the
 * pointer, so the attributes live as long as the process. */
static SECURITY_ATTRIBUTES *pipe_security(void)
{
    static SECURITY_ATTRIBUTES sa;
    static int done;
    if (done) return sa.lpSecurityDescriptor ? &sa : NULL;
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
    PSECURITY_DESCRIPTOR sd = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl, SDDL_REVISION_1, &sd, NULL)) return NULL;
    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    return &sa;
}
#endif

static int bind_socket(int proto, const char *url)
{
    int s = nn_socket(AF_SP, proto);
    if (s < 0) return -1;
    /* A vanished peer must never be able to stall the broker. */
    int to = 1000;
    nn_setsockopt(s, NN_SOL_SOCKET, NN_SNDTIMEO, &to, sizeof to);
#ifdef _WIN32
    SECURITY_ATTRIBUTES *sa = pipe_security();
    if (!sa || nn_setsockopt(s, NN_IPC, NN_IPC_SEC_ATTR, sa, sizeof *sa) < 0) {
        nn_close(s);
        return -1;
    }
#endif
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

int bus_wait(int sock, int timeout_ms)
{
#ifdef _WIN32
    SOCKET fd;
    size_t len = sizeof fd;
    if (nn_getsockopt(sock, NN_SOL_SOCKET, NN_RCVFD, &fd, &len) < 0) return -1;
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
