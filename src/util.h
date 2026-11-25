#ifndef NQ_UTIL_H
#define NQ_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "cJSON.h"

#ifndef NQ_VERSION
#define NQ_VERSION "dev"
#endif

/* Largest message body the broker accepts. */
#define NQ_MAX_BODY (256 * 1024)

/* Growable, always NUL-terminated string buffer. Zero-initialise to use. */
typedef struct {
    char *p;
    size_t len, cap;
} sb_t;

void sb_add(sb_t *sb, const char *s, size_t n);
void sb_adds(sb_t *sb, const char *s);
void sb_addf(sb_t *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_free(sb_t *sb);

/* NULL-terminated argv builder. Strings are borrowed, not copied. */
typedef struct {
    const char **v;
    size_t n, cap;
} av_t;

void av_add(av_t *av, const char *s);
void av_free(av_t *av);

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void nq_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Timestamped line on stderr; the detached broker's stderr is its log file. */
void nq_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

uint64_t now_ms(void);
void sleep_ms(int ms);
uint64_t fnv1a64(const char *s);

/* Drops control characters (C0 except \n and \t, DEL, and UTF-8 encoded C1)
 * in place, so text can be pasted into a terminal without acting as keys. */
void sanitize_text(char *s);

/* Agent and topic names: 1..32 of [A-Za-z0-9_.-], not starting with '-'. */
int valid_name(const char *s);

int mkdir_p(const char *path, mode_t mode);
/* Reads fd to EOF. Returns a NUL-terminated buffer, or NULL if it exceeds max. */
char *read_fd_all(int fd, size_t max, size_t *len);
/* Absolute path of the running executable. */
int self_exe(char *buf, size_t len);
/* Looks `name` up on PATH (or checks it directly if it contains '/'). */
int find_on_path(const char *name, char *buf, size_t len);

#define RUN_MERGE_STDERR 1
/* Runs argv without a shell. Feeds `in` to stdin, collects stdout into `out`.
 * Returns the exit status, or -1 if the process could not be run. */
int run_cmd(const char *const argv[], const char *in, size_t in_len, sb_t *out, int flags);

/* Lowercase hex SHA-256 of data; out must hold 65 bytes. */
void sha256_hex(const void *data, size_t len, char *out);

/* cJSON accessors that tolerate missing keys and wrong types. */
const char *jstr(const cJSON *o, const char *key);
double jnum(const cJSON *o, const char *key, double def);
int jbool(const cJSON *o, const char *key);

#endif
