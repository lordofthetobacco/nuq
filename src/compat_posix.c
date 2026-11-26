/* POSIX implementation of compat.h: thin wrappers over the calls nuq has
 * always used on macOS and Linux. */
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int nq_realpath(const char *path, char *out, size_t len)
{
    char real[PATH_MAX];
    if (!realpath(path, real)) return -1;
    size_t n = strlen(real);
    if (n >= len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(out, real, n + 1);
    return 0;
}

void nq_path_norm(char *path)
{
    (void)path;
}

int nq_is_abs(const char *path)
{
    return path && path[0] == '/';
}

const char *nq_basename(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

const char *nq_home(void)
{
    const char *h = getenv("HOME");
    return h && *h ? h : NULL;
}

const char *nq_user(void)
{
    const char *u = getenv("USER");
    return u && *u ? u : NULL;
}

int nq_mkdir(const char *path, int mode)
{
    return mkdir(path, (mode_t)mode);
}

int nq_rename_replace(const char *from, const char *to)
{
    return rename(from, to);
}

int nq_setenv(const char *key, const char *value)
{
    return setenv(key, value, 1);
}

int nq_unsetenv(const char *key)
{
    return unsetenv(key);
}

char **nq_environ(void)
{
    return environ;
}

int nq_lock_acquire(const char *path)
{
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int nq_lock_write(int handle, const char *text)
{
    size_t len = strlen(text);
    if (ftruncate(handle, 0) < 0 || pwrite(handle, text, len, 0) != (ssize_t)len) return -1;
    return 0;
}

int nq_lock_probe(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    int held = flock(fd, LOCK_SH | LOCK_NB) < 0 && errno == EWOULDBLOCK;
    if (!held) flock(fd, LOCK_UN);
    close(fd);
    return held;
}

void nq_lock_release(int handle)
{
    if (handle >= 0) close(handle);
}

int nq_spawn_detached(const char *const argv[], const char *logfile)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        /* New session, then a second fork so the child is not a session
         * leader and can never acquire a controlling terminal. */
        setsid();
        if (fork() != 0) _exit(0);
        int null = open("/dev/null", O_RDONLY);
        int log = open(logfile, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (null >= 0) dup2(null, 0);
        if (log >= 0) {
            dup2(log, 1);
            dup2(log, 2);
        }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return 0;
}

int nq_spawn_wait(const char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return -1;
}

int nq_term_size(int *cols, int *rows)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0 || ws.ws_col == 0 || ws.ws_row == 0) return -1;
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
}

void nq_console_init(void)
{
}
