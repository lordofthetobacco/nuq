#ifndef NQ_PATHS_H
#define NQ_PATHS_H

#include <limits.h>
#include <stddef.h>

#include "compat.h"

/* Everything derived from the grid's directory. One grid per directory. */
typedef struct {
    char dir[PATH_MAX];      /* realpath of the grid directory */
    char grid[9];            /* 8 hex chars, hash of dir */
#ifdef _WIN32
    char rundir[PATH_MAX];   /* lock and env files; the pipes are named apart */
#else
    char rundir[128];        /* sockets and lock; short because of sun_path */
#endif
    char statedir[PATH_MAX]; /* journal and broker log */
    char rpc_url[192];       /* ipc://<rundir>/rpc.sock */
    char ev_url[192];        /* ipc://<rundir>/events.sock */
#ifdef _WIN32
    char lockfile[PATH_MAX + 16];
#else
    char lockfile[192];
#endif
    char journal[PATH_MAX];
    char logfile[PATH_MAX];
    char session[96];        /* tmux session name */
} nq_paths;

/* dir: explicit directory, else $NUQ_DIR, else the current directory. */
int paths_init(nq_paths *p, const char *dir, char *err, size_t errlen);
/* Creates rundir (0700, owner-checked) and statedir. */
int paths_mkdirs(const nq_paths *p, char *err, size_t errlen);

#endif
