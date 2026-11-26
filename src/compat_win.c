/* Native Windows implementation of compat.h, and of the process, file and
 * clock helpers that util.c implements with POSIX calls.
 *
 * The executables carry a manifest that sets the process code page to UTF-8,
 * so the narrow ("A") Win32 functions and the CRT take and return UTF-8 and
 * no wide-character conversion is needed here. */
#include "util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void errno_from_win(DWORD e)
{
    switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
        errno = ENOENT;
        break;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
        errno = EACCES;
        break;
    case ERROR_LOCK_VIOLATION:
        errno = EWOULDBLOCK;
        break;
    case ERROR_FILENAME_EXCED_RANGE:
    case ERROR_INSUFFICIENT_BUFFER:
        errno = ENAMETOOLONG;
        break;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        errno = EEXIST;
        break;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        errno = ENOMEM;
        break;
    default:
        errno = EINVAL;
        break;
    }
}

static int is_sep(char c)
{
    return c == '/' || c == '\\';
}

ssize_t getline(char **line, size_t *cap, FILE *f)
{
    if (!line || !cap || !f) {
        errno = EINVAL;
        return -1;
    }
    if (!*line || *cap == 0) {
        *cap = 128;
        *line = xrealloc(*line, *cap);
    }
    size_t n = 0;
    int c;
    while ((c = getc(f)) != EOF) {
        if (n + 2 > *cap) {
            *cap *= 2;
            *line = xrealloc(*line, *cap);
        }
        (*line)[n++] = (char)c;
        if (c == '\n') break;
    }
    (*line)[n] = '\0';
    return n ? (ssize_t)n : -1;
}

int nq_isatty(int fd)
{
    DWORD mode;
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    return h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode);
}

/* ---- paths ---------------------------------------------------------- */

void nq_path_norm(char *path)
{
    if (!path) return;
    for (char *p = path; *p; p++)
        if (*p == '\\') *p = '/';
    if (isalpha((unsigned char)path[0]) && path[1] == ':')
        path[0] = (char)toupper((unsigned char)path[0]);
}

int nq_is_abs(const char *path)
{
    if (!path) return 0;
    if (is_sep(path[0])) return 1;
    return isalpha((unsigned char)path[0]) && path[1] == ':' && is_sep(path[2]);
}

const char *nq_basename(const char *path)
{
    const char *base = path;
    if (isalpha((unsigned char)path[0]) && path[1] == ':') base = path + 2;
    for (const char *p = base; *p; p++)
        if (is_sep(*p)) base = p + 1;
    return base;
}

int nq_realpath(const char *path, char *out, size_t len)
{
    if (!path || !*path) {
        errno = ENOENT;
        return -1;
    }
    /* Opening the file and asking for its final name resolves relative
     * parts, links and 8.3 names, and returns the on-disk case, so one
     * directory has one spelling (the grid id is a hash of it). */
    HANDLE h = CreateFileA(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        errno_from_win(GetLastError());
        return -1;
    }
    char buf[PATH_MAX + 8];
    DWORD n = GetFinalPathNameByHandleA(h, buf, (DWORD)sizeof buf, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    DWORD e = GetLastError();
    CloseHandle(h);
    if (n == 0 || n >= sizeof buf) {
        errno_from_win(n ? ERROR_INSUFFICIENT_BUFFER : e);
        return -1;
    }
    const char *p = buf;
    char unc[PATH_MAX + 8];
    if (!strncmp(p, "\\\\?\\UNC\\", 8)) {
        snprintf(unc, sizeof unc, "//%s", p + 8);
        p = unc;
    } else if (!strncmp(p, "\\\\?\\", 4)) {
        p += 4;
    }
    size_t plen = strlen(p);
    if (plen >= len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(out, p, plen + 1);
    nq_path_norm(out);
    return 0;
}

const char *nq_home(void)
{
    static char home[PATH_MAX];
    const char *h = getenv("HOME");
    if (!h || !*h) h = getenv("USERPROFILE");
    if (!h || !*h) return NULL;
    snprintf(home, sizeof home, "%s", h);
    nq_path_norm(home);
    return home;
}

const char *nq_user(void)
{
    const char *u = getenv("USER");
    if (!u || !*u) u = getenv("USERNAME");
    return u && *u ? u : NULL;
}

int nq_mkdir(const char *path, int mode)
{
    (void)mode;
    return _mkdir(path);
}

int nq_rename_replace(const char *from, const char *to)
{
    if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 0;
    errno_from_win(GetLastError());
    return -1;
}

/* _putenv_s updates both the CRT's copy (getenv) and the process
 * environment block that CreateProcess hands to children. */
int nq_setenv(const char *key, const char *value)
{
    if (!key || !*key || strchr(key, '=')) {
        errno = EINVAL;
        return -1;
    }
    if (!*value) {
        /* The CRT cannot hold an empty value (_putenv_s removes the name);
         * children still get the variable, set but empty. */
        _putenv_s(key, "");
        return SetEnvironmentVariableA(key, "") ? 0 : -1;
    }
    return _putenv_s(key, value) == 0 ? 0 : -1;
}

int nq_unsetenv(const char *key)
{
    if (!key || !*key || strchr(key, '=')) {
        errno = EINVAL;
        return -1;
    }
    _putenv_s(key, "");
    SetEnvironmentVariableA(key, NULL);
    return 0;
}

char **nq_environ(void)
{
    return _environ;
}

/* ---- lock file ------------------------------------------------------ */

/* The lock covers one byte far past the end of the file, so the pid text at
 * the start stays readable by other processes while the lock is held. */
#define LOCK_OFFSET_HIGH 0x40000000u

static HANDLE lock_open(const char *path, DWORD disposition)
{
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, FALSE};
    return CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       &sa, disposition, FILE_ATTRIBUTE_NORMAL, NULL);
}

static int lock_try(HANDLE h)
{
    OVERLAPPED ov = {0};
    ov.OffsetHigh = LOCK_OFFSET_HIGH;
    return LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov) ? 0 : -1;
}

int nq_lock_acquire(const char *path)
{
    HANDLE h = lock_open(path, OPEN_ALWAYS);
    if (h == INVALID_HANDLE_VALUE) {
        errno_from_win(GetLastError());
        return -1;
    }
    if (lock_try(h) < 0) {
        DWORD e = GetLastError();
        CloseHandle(h);
        errno = (e == ERROR_LOCK_VIOLATION || e == ERROR_IO_PENDING) ? EWOULDBLOCK : EINVAL;
        return -1;
    }
    int fd = _open_osfhandle((intptr_t)h, 0);
    if (fd < 0) {
        CloseHandle(h);
        return -1;
    }
    return fd;
}

int nq_lock_write(int handle, const char *text)
{
    HANDLE h = (HANDLE)_get_osfhandle(handle);
    if (h == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER zero = {0};
    DWORD n = 0, len = (DWORD)strlen(text);
    if (!SetFilePointerEx(h, zero, NULL, FILE_BEGIN) || !SetEndOfFile(h) || !WriteFile(h, text, len, &n, NULL) ||
        n != len) {
        errno_from_win(GetLastError());
        return -1;
    }
    return 0;
}

int nq_lock_probe(const char *path)
{
    HANDLE h = lock_open(path, OPEN_EXISTING);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int held = 1;
    if (lock_try(h) == 0) {
        OVERLAPPED ov = {0};
        ov.OffsetHigh = LOCK_OFFSET_HIGH;
        UnlockFileEx(h, 0, 1, 0, &ov);
        held = 0;
    }
    CloseHandle(h);
    return held;
}

void nq_lock_release(int handle)
{
    if (handle >= 0) _close(handle); /* closing the handle drops the lock */
}

/* ---- processes ------------------------------------------------------ */

static int has_ext(const char *path, const char *ext)
{
    size_t n = strlen(path), e = strlen(ext);
    return n > e && !_stricmp(path + n - e, ext);
}

static int is_batch(const char *path)
{
    return has_ext(path, ".cmd") || has_ext(path, ".bat");
}

/* Appends one argument quoted the way the MSVC runtime (and nearly every
 * Windows program) splits its command line back into argv. */
static void quote_arg(sb_t *sb, const char *a)
{
    if (*a && !strpbrk(a, " \t\n\v\"")) {
        sb_adds(sb, a);
        return;
    }
    sb_adds(sb, "\"");
    for (const char *p = a;; p++) {
        size_t bs = 0;
        while (*p == '\\') {
            p++;
            bs++;
        }
        if (!*p) {
            for (size_t i = 0; i < bs * 2; i++) sb_adds(sb, "\\");
            break;
        }
        if (*p == '"') {
            for (size_t i = 0; i < bs * 2 + 1; i++) sb_adds(sb, "\\");
            sb_adds(sb, "\"");
        } else {
            for (size_t i = 0; i < bs; i++) sb_adds(sb, "\\");
            sb_add(sb, p, 1);
        }
    }
    sb_adds(sb, "\"");
}

/* Builds the CreateProcess application name and command line for argv,
 * resolving argv[0] like execvp (PATH, plus PATHEXT here). Batch files run
 * through cmd.exe, which parses the line a second time: every argument is
 * quoted so its metacharacters stay literal, '%' is written as a sequence
 * that expands to '%', and an argument with '"' or a line break is refused,
 * because cmd.exe cannot be made to pass those through safely. */
static int build_cmdline(const char *const argv[], char *app, size_t applen, sb_t *line)
{
    if (!argv || !argv[0] || find_on_path(argv[0], app, applen) < 0) {
        errno = ENOENT;
        return -1;
    }
    /* The program sees its own path as argv[0]; cmd.exe, for one, takes
     * "C:/Windows/..." for a "/Windows" switch. */
    for (char *p = app; *p; p++)
        if (*p == '/') *p = '\\';
    if (!is_batch(app)) {
        for (int i = 0; argv[i]; i++) {
            if (i) sb_adds(line, " ");
            quote_arg(line, i ? argv[i] : app);
        }
        return 0;
    }
    char comspec[MAX_PATH];
    UINT n = GetSystemDirectoryA(comspec, sizeof comspec);
    if (n == 0 || n + sizeof "\\cmd.exe" > sizeof comspec) return -1;
    strcat(comspec, "\\cmd.exe");
    sb_adds(line, "\"");
    sb_adds(line, comspec);
    sb_adds(line, "\" /d /v:off /s /c \"");
    for (int i = 0; argv[i]; i++) {
        const char *a = i ? argv[i] : app;
        if (strpbrk(a, "\"\r\n")) {
            sb_free(line);
            errno = EINVAL;
            return -1;
        }
        if (i) sb_adds(line, " ");
        sb_adds(line, "\"");
        for (const char *p = a; *p; p++) {
            if (*p == '%')
                sb_adds(line, "%%cd:~,%");
            else
                sb_add(line, p, 1);
        }
        /* A trailing backslash would escape the closing quote for the
         * program's own argv parser. */
        size_t bs = 0;
        for (const char *p = a + strlen(a); p > a && p[-1] == '\\'; p--) bs++;
        for (size_t k = 0; k < bs; k++) sb_adds(line, "\\");
        sb_adds(line, "\"");
    }
    sb_adds(line, "\"");
    snprintf(app, applen, "%s", comspec);
    return 0;
}

static HANDLE open_null(DWORD access)
{
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    return CreateFileA("NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
}

static HANDLE dup_inheritable(HANDLE h)
{
    HANDLE d = NULL;
    if (!h || h == INVALID_HANDLE_VALUE) return NULL;
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &d, 0, TRUE, DUPLICATE_SAME_ACCESS))
        return NULL;
    return d;
}

/* Starts a process that inherits exactly the given standard handles and
 * nothing else (a lock file or a bus pipe must never leak into tmux or
 * the broker). Any of in/out/err may be NULL. */
static int start_process(const char *const argv[], HANDLE in, HANDLE out, HANDLE err, DWORD flags,
                         PROCESS_INFORMATION *pi)
{
    char app[PATH_MAX];
    sb_t line = {0};
    if (build_cmdline(argv, app, sizeof app, &line) < 0) {
        sb_free(&line);
        return -1;
    }

    HANDLE list[3];
    DWORD nlist = 0;
    HANDLE std[3] = {in, out, err};
    for (int i = 0; i < 3; i++) {
        if (!std[i]) continue;
        int dup = 0;
        for (DWORD k = 0; k < nlist; k++) dup |= list[k] == std[i];
        if (!dup) list[nlist++] = std[i];
    }

    SIZE_T attrsz = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attrsz);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = xmalloc(attrsz);
    int ok = InitializeProcThreadAttributeList(attrs, 1, 0, &attrsz) &&
             (nlist == 0 || UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list,
                                                      nlist * sizeof list[0], NULL, NULL));
    if (!ok) {
        free(attrs);
        sb_free(&line);
        return -1;
    }

    STARTUPINFOEXA si;
    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in;
    si.StartupInfo.hStdOutput = out;
    si.StartupInfo.hStdError = err;
    si.lpAttributeList = attrs;

    BOOL created = CreateProcessA(app, line.p, NULL, NULL, nlist > 0, flags | EXTENDED_STARTUPINFO_PRESENT, NULL,
                                  NULL, &si.StartupInfo, pi);
    DWORD e = GetLastError();
    if (!created && (flags & CREATE_BREAKAWAY_FROM_JOB) && e == ERROR_ACCESS_DENIED) {
        /* The job we are in does not allow breaking away. */
        created = CreateProcessA(app, line.p, NULL, NULL, nlist > 0,
                                 (flags & ~(DWORD)CREATE_BREAKAWAY_FROM_JOB) | EXTENDED_STARTUPINFO_PRESENT, NULL,
                                 NULL, &si.StartupInfo, pi);
        e = GetLastError();
    }
    DeleteProcThreadAttributeList(attrs);
    free(attrs);
    sb_free(&line);
    if (!created) {
        errno_from_win(e);
        return -1;
    }
    return 0;
}

/* A process without a console (the broker) would give every console child
 * a new visible console window. */
static DWORD console_flags(void)
{
    return GetConsoleWindow() ? 0 : CREATE_NO_WINDOW;
}

int nq_spawn_detached(const char *const argv[], const char *logfile)
{
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE in = open_null(GENERIC_READ);
    HANDLE log = CreateFileA(logfile, FILE_APPEND_DATA | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (in == INVALID_HANDLE_VALUE || log == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
        if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
        errno_from_win(e);
        return -1;
    }
    /* No console, its own process group (Ctrl-C in the starting terminal
     * does not reach it) and out of the starting pane's job object, so it
     * outlives whatever started it. */
    PROCESS_INFORMATION pi;
    int rc = start_process(argv, in, log, log, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB,
                           &pi);
    CloseHandle(in);
    CloseHandle(log);
    if (rc < 0) return -1;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

static int exit_status(HANDLE proc)
{
    DWORD code = 0;
    if (!GetExitCodeProcess(proc, &code)) return -1;
    if (code == (DWORD)STATUS_CONTROL_C_EXIT) return 130; /* as 128+SIGINT */
    /* Exception and NTSTATUS codes do not fit an exit status. */
    if (code > 0x7fffffffu) return 255;
    return (int)code;
}

int nq_spawn_wait(const char *const argv[])
{
    /* Same console, same process group (Ctrl-C reaches the child), same job.
     * The standard handles are passed as inheritable duplicates so a
     * redirected stdout works too. */
    HANDLE in = dup_inheritable(GetStdHandle(STD_INPUT_HANDLE));
    HANDLE out = dup_inheritable(GetStdHandle(STD_OUTPUT_HANDLE));
    HANDLE err = dup_inheritable(GetStdHandle(STD_ERROR_HANDLE));
    PROCESS_INFORMATION pi;
    int rc = start_process(argv, in, out, err, 0, &pi);
    if (in) CloseHandle(in);
    if (out) CloseHandle(out);
    if (err) CloseHandle(err);
    if (rc < 0) return -1;
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    rc = exit_status(pi.hProcess);
    CloseHandle(pi.hProcess);
    return rc;
}

typedef struct {
    HANDLE pipe;
    const char *buf;
    size_t len;
} feeder_t;

static DWORD WINAPI feed_stdin(LPVOID arg)
{
    feeder_t *f = arg;
    size_t off = 0;
    while (off < f->len) {
        DWORD chunk = f->len - off > 65536 ? 65536 : (DWORD)(f->len - off), n = 0;
        if (!WriteFile(f->pipe, f->buf + off, chunk, &n, NULL)) break;
        off += n;
    }
    CloseHandle(f->pipe);
    return 0;
}

int run_cmd(const char *const argv[], const char *in, size_t in_len, sb_t *out, int flags)
{
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    int debug = getenv("NUQ_DEBUG") != NULL;
    HANDLE in_r = NULL, in_w = NULL, out_r = NULL, out_w = NULL, err_h = NULL;
    int rc = -1;

    if (in) {
        if (!CreatePipe(&in_r, &in_w, &sa, 0)) goto done;
        SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    } else {
        in_r = open_null(GENERIC_READ);
    }
    if (out) {
        if (!CreatePipe(&out_r, &out_w, &sa, 0)) goto done;
        SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    } else {
        out_w = open_null(GENERIC_WRITE);
    }
    if (out && (flags & RUN_MERGE_STDERR))
        err_h = out_w;
    else if (debug)
        err_h = dup_inheritable(GetStdHandle(STD_ERROR_HANDLE));
    else
        err_h = open_null(GENERIC_WRITE);
    if (in_r == INVALID_HANDLE_VALUE) in_r = NULL;
    if (out_w == INVALID_HANDLE_VALUE) out_w = NULL;
    if (err_h == INVALID_HANDLE_VALUE) err_h = NULL;

    PROCESS_INFORMATION pi;
    if (start_process(argv, in_r, out_w, err_h, console_flags(), &pi) < 0) {
        rc = 127; /* as the POSIX child does when exec fails */
        goto done;
    }
    CloseHandle(pi.hThread);
    /* Our copies of the child's ends must go, or EOF never arrives. */
    if (in_r) CloseHandle(in_r);
    if (err_h && err_h != out_w) CloseHandle(err_h);
    if (out_w) CloseHandle(out_w);
    in_r = err_h = out_w = NULL;

    /* stdin is written from a second thread while this one drains stdout,
     * so a child that answers before reading all its input (tmux
     * load-buffer with a large paste) cannot deadlock us. */
    HANDLE feeder = NULL;
    feeder_t f = {in_w, in, in_len};
    if (in_w) {
        feeder = CreateThread(NULL, 0, feed_stdin, &f, 0, NULL);
        if (!feeder) feed_stdin(&f);
        in_w = NULL;
    }
    if (out_r) {
        char buf[4096];
        DWORD n;
        while (ReadFile(out_r, buf, sizeof buf, &n, NULL) && n > 0) sb_add(out, buf, n);
    }
    if (feeder) {
        WaitForSingleObject(feeder, INFINITE);
        CloseHandle(feeder);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    rc = exit_status(pi.hProcess);
    CloseHandle(pi.hProcess);

done:
    if (in_r) CloseHandle(in_r);
    if (in_w) CloseHandle(in_w);
    if (out_r) CloseHandle(out_r);
    if (err_h && err_h != out_w) CloseHandle(err_h);
    if (out_w) CloseHandle(out_w);
    return rc;
}

/* ---- files and executables ------------------------------------------ */

int self_exe(char *buf, size_t len)
{
    char tmp[PATH_MAX];
    DWORD n = GetModuleFileNameA(NULL, tmp, sizeof tmp);
    if (n == 0 || n >= sizeof tmp) return -1;
    return nq_realpath(tmp, buf, len);
}

static int is_file(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Windows decides what is executable by extension (PATHEXT). An npm
 * install puts an extensionless sh script next to the .cmd, so a name
 * without one of these extensions never matches as is. */
static int try_exts(const char *base, char *buf, size_t len)
{
    const char *pathext = getenv("PATHEXT");
    if (!pathext || !*pathext) pathext = ".COM;.EXE;.BAT;.CMD";
    const char *dot = strrchr(nq_basename(base), '.');
    char cand[PATH_MAX];
    for (const char *e = pathext; *e;) {
        const char *end = strchr(e, ';');
        size_t elen = end ? (size_t)(end - e) : strlen(e);
        if (elen) {
            int has = dot && strlen(dot) == elen && !_strnicmp(dot, e, elen);
            int n = has ? snprintf(cand, sizeof cand, "%s", base)
                        : snprintf(cand, sizeof cand, "%s%.*s", base, (int)elen, e);
            if (n > 0 && (size_t)n < sizeof cand && is_file(cand) && nq_realpath(cand, buf, len) == 0) return 0;
        }
        if (!end) break;
        e = end + 1;
    }
    return -1;
}

int find_on_path(const char *name, char *buf, size_t len)
{
    if (!name || !*name) return -1;
    if (strpbrk(name, "/\\:")) return try_exts(name, buf, len);
    const char *path = getenv("PATH");
    if (!path) return -1;
    while (*path) {
        const char *end = strchr(path, ';');
        size_t dlen = end ? (size_t)(end - path) : strlen(path);
        const char *d = path;
        if (dlen >= 2 && d[0] == '"' && d[dlen - 1] == '"') {
            d++;
            dlen -= 2;
        }
        char cand[PATH_MAX];
        int n = snprintf(cand, sizeof cand, "%.*s/%s", (int)dlen, d, name);
        if (dlen && n > 0 && (size_t)n < sizeof cand && try_exts(cand, buf, len) == 0) return 0;
        if (!end) break;
        path = end + 1;
    }
    return -1;
}

int mkdir_p(const char *path, mode_t mode)
{
    (void)mode;
    char buf[PATH_MAX];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof buf) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buf, path, n + 1);
    nq_path_norm(buf);
    /* Skip the root: "C:/", "//server/share/" or "/". */
    char *p = buf;
    if (isalpha((unsigned char)p[0]) && p[1] == ':') {
        p += 2;
    } else if (p[0] == '/' && p[1] == '/') {
        p += 2;
        for (int parts = 0; *p && parts < 2; p++)
            if (*p == '/') parts++;
    }
    while (*p == '/') p++;
    for (; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (_mkdir(buf) < 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (_mkdir(buf) < 0 && errno != EEXIST) return -1;
    return 0;
}

/* ---- clock and terminal --------------------------------------------- */

uint64_t now_ms(void)
{
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t t = (uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
    return (t - 116444736000000000ULL) / 10000u; /* 100 ns since 1601 to ms since 1970 */
}

void sleep_ms(int ms)
{
    Sleep(ms > 0 ? (DWORD)ms : 0);
}

int nq_term_size(int *cols, int *rows)
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleScreenBufferInfo(h, &info)) return -1;
    *cols = info.srWindow.Right - info.srWindow.Left + 1;
    *rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    return *cols > 0 && *rows > 0 ? 0 : -1;
}

static UINT saved_in_cp, saved_out_cp;

static void restore_console(void)
{
    if (saved_in_cp) SetConsoleCP(saved_in_cp);
    if (saved_out_cp) SetConsoleOutputCP(saved_out_cp);
}

void nq_console_init(void)
{
    /* The console's code pages belong to the console, not to us: put back
     * what the shell had when we exit. */
    saved_in_cp = GetConsoleCP();
    saved_out_cp = GetConsoleOutputCP();
    if (saved_in_cp || saved_out_cp) {
        SetConsoleCP(CP_UTF8);
        SetConsoleOutputCP(CP_UTF8);
        atexit(restore_console);
    }
    /* nuq reads and writes bytes: JSON, pasted text, the skill file. */
    _setmode(STDIN_FILENO, _O_BINARY);
    _setmode(STDOUT_FILENO, _O_BINARY);
    _setmode(STDERR_FILENO, _O_BINARY);
    /* Character classification and mbrtowc() in UTF-8; numbers keep the
     * "C" locale's decimal point. */
    setlocale(LC_CTYPE, ".UTF-8");
}
