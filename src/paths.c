#include "paths.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "util.h"

#define LONGEST_SOCK "/events.sock"

static int rundir_fits(const char *base, unsigned uid, const char *grid, char *out, size_t outlen)
{
    int n = snprintf(out, outlen, "%s/nuq-%u/%s", base, uid, grid);
    size_t limit = sizeof(((struct sockaddr_un *)0)->sun_path);
    return n > 0 && (size_t)n < outlen && (size_t)n + strlen(LONGEST_SOCK) < limit;
}

int paths_init(nq_paths *p, const char *dir, char *err, size_t errlen)
{
    memset(p, 0, sizeof *p);
    if (!dir || !*dir) dir = getenv("NUQ_DIR");
    if (!dir || !*dir) dir = ".";
    if (!realpath(dir, p->dir)) {
        snprintf(err, errlen, "%s: %s", dir, strerror(errno));
        return -1;
    }
    uint64_t h = fnv1a64(p->dir);
    snprintf(p->grid, sizeof p->grid, "%08x", (unsigned)((h ^ (h >> 32)) & 0xffffffffu));

    /* Unix socket paths are limited to ~104 bytes, so fall back to /tmp when
     * the preferred runtime directory is too deep. */
    unsigned uid = (unsigned)getuid();
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !*rt || !rundir_fits(rt, uid, p->grid, p->rundir, sizeof p->rundir)) {
        if (!rundir_fits("/tmp", uid, p->grid, p->rundir, sizeof p->rundir)) {
            snprintf(err, errlen, "runtime directory path is too long");
            return -1;
        }
    }
    snprintf(p->rpc_url, sizeof p->rpc_url, "ipc://%s/rpc.sock", p->rundir);
    snprintf(p->ev_url, sizeof p->ev_url, "ipc://%s%s", p->rundir, LONGEST_SOCK);
    snprintf(p->lockfile, sizeof p->lockfile, "%s/daemon.lock", p->rundir);

    const char *state = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    int n;
    if (state && *state)
        n = snprintf(p->statedir, sizeof p->statedir, "%s/nuq/%s", state, p->grid);
    else if (home && *home)
        n = snprintf(p->statedir, sizeof p->statedir, "%s/.local/state/nuq/%s", home, p->grid);
    else
        n = snprintf(p->statedir, sizeof p->statedir, "%s/state", p->rundir);
    if (n < 0 || (size_t)n >= sizeof p->statedir - 16) {
        snprintf(err, errlen, "state directory path is too long");
        return -1;
    }
    snprintf(p->journal, sizeof p->journal, "%s/log.jsonl", p->statedir);
    snprintf(p->logfile, sizeof p->logfile, "%s/daemon.log", p->statedir);

    /* tmux session names may not contain '.' or ':'. */
    const char *base = strrchr(p->dir, '/');
    base = base && base[1] ? base + 1 : "root";
    char clean[41];
    size_t j = 0;
    for (const char *s = base; *s && j < sizeof clean - 1; s++) {
        unsigned char c = (unsigned char)*s;
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '_' || c == '-';
        clean[j++] = ok ? (char)c : '_';
    }
    clean[j] = '\0';
    snprintf(p->session, sizeof p->session, "nq-%s-%s", clean, p->grid);
    return 0;
}

int paths_mkdirs(const nq_paths *p, char *err, size_t errlen)
{
    /* <base>/nuq-<uid> must be ours and private: it holds the sockets. */
    char parent[sizeof p->rundir];
    snprintf(parent, sizeof parent, "%s", p->rundir);
    char *slash = strrchr(parent, '/');
    if (slash) *slash = '\0';
    if (mkdir(parent, 0700) < 0 && errno != EEXIST) {
        snprintf(err, errlen, "mkdir %s: %s", parent, strerror(errno));
        return -1;
    }
    struct stat st;
    if (lstat(parent, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 077) != 0) {
        snprintf(err, errlen, "%s must be a private directory owned by you", parent);
        return -1;
    }
    if (mkdir(p->rundir, 0700) < 0 && errno != EEXIST) {
        snprintf(err, errlen, "mkdir %s: %s", p->rundir, strerror(errno));
        return -1;
    }
    if (mkdir_p(p->statedir, 0700) < 0) {
        snprintf(err, errlen, "mkdir %s: %s", p->statedir, strerror(errno));
        return -1;
    }
    return 0;
}
