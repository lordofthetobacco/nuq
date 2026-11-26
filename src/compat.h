#ifndef NQ_COMPAT_H
#define NQ_COMPAT_H

/* The few places where POSIX and native Windows differ, behind one interface.
 * POSIX builds keep using the system calls they always used; compat_posix.c
 * wraps them. compat_win.c implements the same contract on Win32. Every path
 * nuq stores or compares is absolute with forward slashes; on Windows it also
 * carries an upper-case drive letter ("C:/Users/x/proj"). */

#include <stddef.h>
#include <sys/types.h>

#if defined(__GNUC__) || defined(__clang__)
#define NQ_PRINTF(fmt, first) __attribute__((format(printf, fmt, first)))
#define NQ_NORETURN __attribute__((noreturn))
#define NQ_SENTINEL __attribute__((sentinel))
#elif defined(_MSC_VER)
#define NQ_PRINTF(fmt, first)
#define NQ_NORETURN __declspec(noreturn)
#define NQ_SENTINEL
#else
#define NQ_PRINTF(fmt, first)
#define NQ_NORETURN
#define NQ_SENTINEL
#endif

#ifdef _WIN32

#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
#ifdef _WIN64
typedef __int64 ssize_t;
#else
typedef int ssize_t;
#endif
#endif
#ifndef _MODE_T_DEFINED
#define _MODE_T_DEFINED
typedef unsigned short mode_t;
#endif
#ifndef _PID_T_DEFINED
#define _PID_T_DEFINED
typedef int pid_t;
#endif

/* Paths are UTF-8 (the manifest sets the code page), so a MAX_PATH-length
 * path can take several times MAX_PATH bytes. */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#endif

#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif

#ifndef F_OK
#define F_OK 0
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef R_OK
#define R_OK 4
#endif

#define strtok_r strtok_s
static inline struct tm *localtime_r(const time_t *t, struct tm *out)
{
    return localtime_s(out, t) == 0 ? out : NULL;
}
ssize_t getline(char **line, size_t *cap, FILE *f);

/* The CRT's isatty() is true for any character device, NUL included, so
 * `nuq ... </dev/null >/dev/null` would think it has a terminal. This one
 * is true only for a console (including a pseudo console). */
int nq_isatty(int fd);
#define isatty nq_isatty

#define NQ_PATH_LIST_SEP ';'
#define NQ_O_BINARY _O_BINARY
#define NQ_O_NOINHERIT _O_NOINHERIT

#else /* POSIX */

#include <fcntl.h>
#include <unistd.h>

#define NQ_PATH_LIST_SEP ':'
#define NQ_O_BINARY 0
#define NQ_O_NOINHERIT O_CLOEXEC

#endif

/* Canonical absolute path of an existing file or directory. Windows: forward
 * slashes, upper-case drive letter, true on-disk case. 0, or -1 with errno. */
int nq_realpath(const char *path, char *out, size_t len);
/* Rewrites a path in place to the stored form (Windows: '\\' to '/', drive
 * letter upper-cased). No file-system access. No-op on POSIX. */
void nq_path_norm(char *path);
/* "/x"; on Windows also "C:/x", "C:\\x", "//server/x". */
int nq_is_abs(const char *path);
/* Part after the last separator ('/', and on Windows also '\\' and "C:"). */
const char *nq_basename(const char *path);
/* $HOME, else %USERPROFILE% (normalised); NULL if neither. */
const char *nq_home(void);
/* $USER, else %USERNAME%; NULL if neither. */
const char *nq_user(void);
/* mkdir(2); mode ignored on Windows. */
int nq_mkdir(const char *path, int mode);
/* Atomically replaces `to` (which may exist) with `from`. */
int nq_rename_replace(const char *from, const char *to);
int nq_setenv(const char *key, const char *value);
int nq_unsetenv(const char *key);
/* NULL-terminated "K=V" strings of the current process environment. */
char **nq_environ(void);

/* Single-instance lock file. acquire: exclusive, non-blocking, never inherited
 * by child processes; returns a handle >= 0, or -1 (errno EWOULDBLOCK when
 * another process holds it). The holder may still write its pid into the file
 * through nq_lock_write. probe: 1 if some process holds the lock, else 0. */
int nq_lock_acquire(const char *path);
int nq_lock_write(int handle, const char *text);
int nq_lock_probe(const char *path);
void nq_lock_release(int handle);

/* Starts argv detached from the caller's terminal and session: stdin is the
 * null device, stdout and stderr append to logfile, nothing else is inherited,
 * and the child survives the caller. 0 if started, -1 if not. */
int nq_spawn_detached(const char *const argv[], const char *logfile);
/* Runs argv with the caller's console and standard handles, waits, returns the
 * exit status (128+signal on POSIX), or -1 if it could not be started. */
int nq_spawn_wait(const char *const argv[]);
/* Size of the terminal on stdout. 0 on success. */
int nq_term_size(int *cols, int *rows);
/* Call first in main(). Windows: UTF-8 console code pages, binary stdin,
 * stdout and stderr, UTF-8 locale. No-op on POSIX. */
void nq_console_init(void);

#endif
