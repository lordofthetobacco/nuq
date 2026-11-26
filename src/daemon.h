#ifndef NQ_DAEMON_H
#define NQ_DAEMON_H

#include <stddef.h>

#include "paths.h"

/* Runs the broker in the foreground until `nuq nuke`, a signal, or the
 * grid's tmux session ending. */
int cmd_daemon(const nq_paths *p);

/* Is a broker holding this grid's lock? Does not touch NNG. */
int daemon_alive(const nq_paths *p);

/* Starts a detached broker if none is running and waits until it answers. */
int daemon_ensure(const nq_paths *p, char *err, size_t errlen);

#endif
