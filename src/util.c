#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

void *xcalloc(size_t n, size_t size)
{
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (!p) die("out of memory");
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    return memcpy(xmalloc(n), s, n);
}

static void sb_reserve(sb_t *sb, size_t extra)
{
    if (sb->len + extra + 1 <= sb->cap) return;
    size_t cap = sb->cap ? sb->cap : 64;
    while (cap < sb->len + extra + 1) cap *= 2;
    sb->p = xrealloc(sb->p, cap);
    sb->cap = cap;
}

void sb_add(sb_t *sb, const char *s, size_t n)
{
    sb_reserve(sb, n);
    memcpy(sb->p + sb->len, s, n);
    sb->len += n;
    sb->p[sb->len] = '\0';
}

void sb_adds(sb_t *sb, const char *s)
{
    sb_add(sb, s, strlen(s));
}

void sb_addf(sb_t *sb, const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n >= 0) {
        sb_reserve(sb, (size_t)n);
        vsnprintf(sb->p + sb->len, (size_t)n + 1, fmt, ap2);
        sb->len += (size_t)n;
    }
    va_end(ap2);
}

void sb_free(sb_t *sb)
{
    free(sb->p);
    sb->p = NULL;
    sb->len = sb->cap = 0;
}

void av_add(av_t *av, const char *s)
{
    if (av->n + 2 > av->cap) {
        av->cap = av->cap ? av->cap * 2 : 16;
        av->v = xrealloc(av->v, av->cap * sizeof *av->v);
    }
    av->v[av->n++] = s;
    av->v[av->n] = NULL;
}

void av_free(av_t *av)
{
    free(av->v);
    av->v = NULL;
    av->n = av->cap = 0;
}

void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("nuq: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

void nq_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("nuq: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void nq_log(const char *fmt, ...)
{
    char ts[32];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s ", ts);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fflush(stderr);
}

uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void sleep_ms(int ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
}

uint64_t fnv1a64(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

void sanitize_text(char *s)
{
    unsigned char *r = (unsigned char *)s, *w = r;
    while (*r) {
        if (*r == 0xC2 && r[1] >= 0x80 && r[1] <= 0x9F) {
            r += 2;
            continue;
        }
        if ((*r < 0x20 && *r != '\n' && *r != '\t') || *r == 0x7F) {
            r++;
            continue;
        }
        *w++ = *r++;
    }
    *w = '\0';
}

int valid_name(const char *s)
{
    if (!s || !*s || *s == '-') return 0;
    size_t n = 0;
    for (; *s; s++, n++) {
        unsigned char c = (unsigned char)*s;
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '_' || c == '.' || c == '-';
        if (!ok) return 0;
    }
    return n <= 32;
}

int mkdir_p(const char *path, mode_t mode)
{
    char buf[PATH_MAX];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof buf) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buf, path, n + 1);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(buf, mode) < 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(buf, mode) < 0 && errno != EEXIST) return -1;
    return 0;
}

char *read_fd_all(int fd, size_t max, size_t *len)
{
    sb_t sb = {0};
    char buf[8192];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            sb_free(&sb);
            return NULL;
        }
        if (n == 0) break;
        if (sb.len + (size_t)n > max) {
            sb_free(&sb);
            errno = EFBIG;
            return NULL;
        }
        sb_add(&sb, buf, (size_t)n);
    }
    if (!sb.p) sb_add(&sb, "", 0);
    if (len) *len = sb.len;
    return sb.p;
}

int self_exe(char *buf, size_t len)
{
    char tmp[PATH_MAX];
#ifdef __APPLE__
    uint32_t n = sizeof tmp;
    if (_NSGetExecutablePath(tmp, &n) != 0) return -1;
#else
    ssize_t n = readlink("/proc/self/exe", tmp, sizeof tmp - 1);
    if (n <= 0) return -1;
    tmp[n] = '\0';
#endif
    char real[PATH_MAX];
    if (!realpath(tmp, real)) return -1;
    if (strlen(real) >= len) return -1;
    strcpy(buf, real);
    return 0;
}

static int is_executable_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

int find_on_path(const char *name, char *buf, size_t len)
{
    if (strchr(name, '/')) {
        char real[PATH_MAX];
        if (!is_executable_file(name) || !realpath(name, real) || strlen(real) >= len) return -1;
        strcpy(buf, real);
        return 0;
    }
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    while (*path) {
        const char *end = strchr(path, ':');
        size_t dlen = end ? (size_t)(end - path) : strlen(path);
        char cand[PATH_MAX];
        int n = snprintf(cand, sizeof cand, "%.*s/%s", (int)dlen, dlen ? path : ".", name);
        if (n > 0 && (size_t)n < sizeof cand && is_executable_file(cand) && (size_t)n < len) {
            strcpy(buf, cand);
            return 0;
        }
        if (!end) break;
        path = end + 1;
    }
    return -1;
}

static void redirect_null(int target, int flags)
{
    int fd = open("/dev/null", flags);
    if (fd < 0) return;
    if (fd != target) {
        dup2(fd, target);
        close(fd);
    }
}

int run_cmd(const char *const argv[], const char *in, size_t in_len, sb_t *out, int flags)
{
    int ip[2] = {-1, -1}, op[2] = {-1, -1};
    int debug = getenv("NUQ_DEBUG") != NULL;
    if (in && pipe(ip) < 0) return -1;
    if (out && pipe(op) < 0) {
        if (in) {
            close(ip[0]);
            close(ip[1]);
        }
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        if (in) {
            close(ip[0]);
            close(ip[1]);
        }
        if (out) {
            close(op[0]);
            close(op[1]);
        }
        return -1;
    }
    if (pid == 0) {
        if (in) {
            dup2(ip[0], 0);
            close(ip[0]);
            close(ip[1]);
        } else {
            redirect_null(0, O_RDONLY);
        }
        if (out) {
            dup2(op[1], 1);
            if (flags & RUN_MERGE_STDERR) dup2(op[1], 2);
            close(op[0]);
            close(op[1]);
        } else {
            redirect_null(1, O_WRONLY);
        }
        if (!debug && !(out && (flags & RUN_MERGE_STDERR))) redirect_null(2, O_WRONLY);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    if (in) {
        close(ip[0]);
        size_t off = 0;
        while (off < in_len) {
            ssize_t n = write(ip[1], in + off, in_len - off);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            off += (size_t)n;
        }
        close(ip[1]);
    }
    if (out) {
        close(op[1]);
        char buf[4096];
        for (;;) {
            ssize_t n = read(op[0], buf, sizeof buf);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            sb_add(out, buf, (size_t)n);
        }
        close(op[0]);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

const char *jstr(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

double jnum(const cJSON *o, const char *key, double def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

int jbool(const cJSON *o, const char *key)
{
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, key));
}

/* SHA-256 (FIPS 180-4). Small and unoptimised: it hashes a few short
 * strings per launch. */
static uint32_t rotr32(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32 - n));
}

static void sha256_block(uint32_t st[8], const unsigned char *p)
{
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64], a[8];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 |
               (uint32_t)p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(a, st, sizeof a);
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = rotr32(a[4], 6) ^ rotr32(a[4], 11) ^ rotr32(a[4], 25);
        uint32_t ch = (a[4] & a[5]) ^ (~a[4] & a[6]);
        uint32_t t1 = a[7] + s1 + ch + K[i] + w[i];
        uint32_t s0 = rotr32(a[0], 2) ^ rotr32(a[0], 13) ^ rotr32(a[0], 22);
        uint32_t maj = (a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]);
        uint32_t t2 = s0 + maj;
        a[7] = a[6];
        a[6] = a[5];
        a[5] = a[4];
        a[4] = a[3] + t1;
        a[3] = a[2];
        a[2] = a[1];
        a[1] = a[0];
        a[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) st[i] += a[i];
}

void sha256_hex(const void *data, size_t len, char *out)
{
    uint32_t st[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const unsigned char *p = data;
    size_t left = len;
    for (; left >= 64; left -= 64, p += 64) sha256_block(st, p);

    unsigned char tail[128] = {0};
    memcpy(tail, p, left);
    tail[left] = 0x80;
    size_t total = left < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) tail[total - 1 - (size_t)i] = (unsigned char)(bits >> (8 * i));
    sha256_block(st, tail);
    if (total == 128) sha256_block(st, tail + 64);

    for (int i = 0; i < 8; i++) snprintf(out + 8 * i, 9, "%08x", st[i]);
}
