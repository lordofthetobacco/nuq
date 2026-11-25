#ifndef NQ_BUS_H
#define NQ_BUS_H

#include <stddef.h>

/* Thin wrapper over nanomsg. bus.c is the only file that includes nn.h, so
 * moving to NNG later means rewriting one file.
 *
 * nanomsg starts worker threads on the first socket and they do not survive
 * fork(): never call these between fork() and exec(). */

#define BUS_DONTWAIT 1

/* Broker side. Return a socket (>= 0) or -1. */
int bus_rep_bind(const char *url);
int bus_pub_bind(const char *url);
/* File descriptor that becomes readable when a message is waiting. Only for
 * poll(); never read from it. */
int bus_rcvfd(int sock);

/* Client side. */
int bus_req_connect(const char *url, int timeout_ms);
int bus_sub_connect(const char *url);
int bus_sub_add(int sock, const char *prefix);
int bus_set_rcvtimeo(int sock, int timeout_ms);

/* Returns a malloc'd NUL-terminated copy of the next message, or NULL. */
char *bus_recv(int sock, int flags, size_t *len);
int bus_send(int sock, const char *buf, size_t len, int flags);
void bus_close(int sock);

/* One request, one reply. Returns the malloc'd reply or NULL on timeout. */
char *bus_request(const char *url, const char *req, int timeout_ms);

const char *bus_error(void);

#endif
