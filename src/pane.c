#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

#include "adapters.h"
#include "cli.h"
#include "util.h"

/* `nuq _pane NAME KIND MODE ENVFILE SUBS -- AGENT [args...]` is the
 * process tmux runs in each pane. It restores the launcher's environment,
 * tells the broker which pane the agent lives in, runs the agent, and
 * reports when it exits. */

static volatile sig_atomic_t g_child;

static void forward(int sig)
{
    if (g_child > 0) kill((pid_t)g_child, sig);
}

/* Variables that describe the pane itself and must not be overwritten by
 * the launcher's copies. */
static int pane_owned(const char *entry)
{
    static const char *const names[] = {"TMUX=",  "TMUX_PANE=", "TERM=",  "TERM_PROGRAM=",
                                        "TERM_PROGRAM_VERSION=", "SHLVL=", "PWD=", "OLDPWD=",
                                        "_=",     "COLUMNS=",   "LINES=", "NUQ_AGENT=",
                                        "NUQ_DIR=", "NUQ_TOKEN=", NULL};
    for (int i = 0; names[i]; i++)
        if (!strncmp(entry, names[i], strlen(names[i]))) return 1;
    return 0;
}

static void load_env(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    size_t len = 0;
    char *data = read_fd_all(fd, 8 * 1024 * 1024, &len);
    close(fd);
    unlink(path);
    if (!data) return;
    for (size_t off = 0; off < len;) {
        char *entry = data + off;
        size_t n = strlen(entry);
        off += n + 1;
        char *eq = strchr(entry, '=');
        if (!eq || eq == entry || pane_owned(entry)) continue;
        *eq = '\0';
        setenv(entry, eq + 1, 1);
    }
    free(data);
}

/* Agents are told to run `nuq ...`; make sure that finds this binary. */
static void put_self_on_path(const char *self)
{
    char found[PATH_MAX];
    if (find_on_path("nuq", found, sizeof found) == 0 && !strcmp(found, self)) return;
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", self);
    char *slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = '\0';
    const char *path = getenv("PATH");
    sb_t np = {0};
    sb_addf(&np, "%s%s%s", dir, path && *path ? ":" : "", path ? path : "");
    setenv("PATH", np.p, 1);
    sb_free(&np);
}

int cmd_pane(nq_ctx *c, int argc, char **argv)
{
    if (argc < 7 || strcmp(argv[5], "--")) die("_pane is run by nuq itself");
    const char *name = argv[0], *kind = argv[1], *envfile = argv[3], *subs = argv[4];
    int mode = atoi(argv[2]);
    const char *agent_path = argv[6];

    char self[PATH_MAX];
    if (self_exe(self, sizeof self) < 0) die("cannot locate the nuq executable");
    load_env(envfile);
    setenv("NUQ_DIR", c->p.dir, 1);
    setenv("NUQ_AGENT", name, 1);
    char token[48];
    snprintf(token, sizeof token, "%lx.%llx", (long)getpid(), (unsigned long long)now_ms());
    setenv("NUQ_TOKEN", token, 1);
    put_self_on_path(self);
    c->me = name;

    av_t av = {0}, owned = {0};
    adapter_command(c, kind, name, self, agent_path, argc - 7, argv + 7, &av, &owned);

    /* Register before the agent starts, so its first hook finds the pane. */
    cJSON *q = cli_req("hello");
    cJSON_AddStringToObject(q, "name", name);
    cJSON_AddStringToObject(q, "kind", kind);
    cJSON_AddStringToObject(q, "token", token);
    cJSON_AddNumberToObject(q, "mode", mode);
    const char *pane = getenv("TMUX_PANE");
    if (pane) cJSON_AddStringToObject(q, "pane", pane);
    if (strcmp(subs, "-")) {
        cJSON *arr = cJSON_AddArrayToObject(q, "subs");
        char *copy = xstrdup(subs), *save = NULL;
        for (char *t = strtok_r(copy, ",", &save); t; t = strtok_r(NULL, ",", &save))
            cJSON_AddItemToArray(arr, cJSON_CreateString(t));
        free(copy);
    }
    cJSON *r = cli_rpc(c, q, 2000);
    if (!r || !jbool(r, "ok")) {
        const char *e = r ? jstr(r, "error") : NULL;
        nq_warn("could not register with the broker%s%s; messages will not reach this pane",
                e ? ": " : "", e ? e : "");
    }
    cJSON_Delete(r);

    uint64_t started = now_ms();
    pid_t pid = fork();
    if (pid < 0) die("fork: %s", strerror(errno));
    if (pid == 0) {
        execv(agent_path, (char *const *)av.v);
        fprintf(stderr, "nuq: cannot run %s: %s\n", agent_path, strerror(errno));
        _exit(127);
    }
    g_child = pid;

    /* The agent shares this terminal: Ctrl-C and Ctrl-\ are meant for it.
     * (Set only after the fork, because ignored signals survive exec.) */
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = forward;
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int st = 0;
    for (;;) {
        pid_t w = waitpid(pid, &st, WUNTRACED);
        if (w < 0) {
            if (errno == EINTR) continue;
            break;
        }
        /* Nothing above us does job control, so a stopped agent would hang
         * the pane forever. Wake it straight back up. */
        if (WIFSTOPPED(st)) {
            kill(pid, SIGCONT);
            continue;
        }
        break;
    }

    q = cli_req("bye");
    cJSON_AddStringToObject(q, "name", name);
    cJSON_AddStringToObject(q, "token", token);
    cJSON_Delete(cli_rpc(c, q, 1000));
    for (size_t i = 0; i < owned.n; i++) free((char *)owned.v[i]);
    av_free(&owned);
    av_free(&av);

    int code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    /* An agent that dies at once (bad flag, not logged in) would take its
     * error message with it when the pane closes. Hold the pane open. */
    if (code != 0 && now_ms() - started < 5000 && isatty(STDIN_FILENO)) {
        fprintf(stderr, "\n[nuq] %s exited with status %d. Press Enter to close this pane.\n", name,
                code);
        int ch;
        while ((ch = getchar()) != EOF && ch != '\n') {}
    }
    return code;
}
