#include "tmux.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

int tmux_argv(av_t *args, const char *in, sb_t *out)
{
    av_t av = {0};
    const char *sock = getenv("NUQ_TMUX_SOCKET");
#ifdef _WIN32
    /* Otherwise psmux keeps a spare "warm" server and pre-started shells,
     * sitting in the grid directory: they outlive `nuq down` and keep the
     * directory from being deleted. Servers inherit this from the client
     * that starts them. */
    if (!getenv("PSMUX_NO_WARM")) nq_setenv("PSMUX_NO_WARM", "1");
#endif
    av_add(&av, "tmux");
    if (sock && *sock) {
        av_add(&av, "-L");
        av_add(&av, sock);
    }
    for (size_t i = 0; i < args->n; i++) av_add(&av, args->v[i]);
    int rc = run_cmd(av.v, in, in ? strlen(in) : 0, out, RUN_MERGE_STDERR);
    av_free(&av);
    return rc;
}

int tmux(sb_t *out, const char *in, ...)
{
    av_t av = {0};
    va_list ap;
    va_start(ap, in);
    for (const char *a = va_arg(ap, const char *); a; a = va_arg(ap, const char *)) av_add(&av, a);
    va_end(ap);
    int rc = tmux_argv(&av, in, out);
    av_free(&av);
    return rc;
}

int tmux_check(char *err, size_t errlen)
{
    sb_t out = {0};
    const char *argv[] = {"tmux", "-V", NULL};
    int rc = run_cmd(argv, NULL, 0, &out, 0);
    if (rc != 0 || !out.p) {
        snprintf(err, errlen, "tmux not found on PATH (nuq needs tmux 3.2 or newer)");
        sb_free(&out);
        return -1;
    }
    /* "tmux 3.8", "tmux 3.3a", "tmux next-3.5". */
    int major = 0, minor = 0;
    const char *p = out.p;
    while (*p && (*p < '0' || *p > '9')) p++;
    sscanf(p, "%d.%d", &major, &minor);
    if (major < 3 || (major == 3 && minor < 2)) {
        size_t n = strcspn(out.p, "\n");
        snprintf(err, errlen, "%.*s is too old (nuq needs tmux 3.2 or newer)", (int)n, out.p);
        sb_free(&out);
        return -1;
    }
    sb_free(&out);
    return 0;
}

/* "=name" makes tmux match the session name exactly instead of by prefix. */
static void exact(char *buf, size_t len, const char *session)
{
    snprintf(buf, len, "=%s", session);
}

int tmux_has_session(const char *session)
{
    char t[128];
    exact(t, sizeof t, session);
    return tmux(NULL, NULL, "has-session", "-t", t, NULL) == 0;
}

int tmux_list_panes(const char *session, sb_t *out)
{
    char t[128];
    exact(t, sizeof t, session);
    sb_t tmp = {0};
    int rc = tmux(&tmp, NULL, "list-panes", "-s", "-t", t, "-F", "#{pane_id}", NULL);
    if (rc == 0 && tmp.p) sb_add(out, tmp.p, tmp.len);
    sb_free(&tmp);
    return rc == 0 ? 0 : -1;
}

int tmux_capture(const char *pane, sb_t *out)
{
    return tmux(out, NULL, "capture-pane", "-p", "-t", pane, NULL) == 0 ? 0 : -1;
}

int tmux_paste(const char *pane, const char *text)
{
    /* One buffer per pane so concurrent wakes cannot swap their text. */
    char buf[64];
    snprintf(buf, sizeof buf, "nuq-%s", pane[0] == '%' ? pane + 1 : pane);
    if (tmux(NULL, text, "load-buffer", "-b", buf, "-", NULL) != 0) return -1;
    /* -p: bracketed paste when the application asked for it, so newlines are
     * text and not Enter. -d: drop the buffer afterwards. Line feeds become
     * carriage returns, as they do when a person pastes into a terminal. */
    return tmux(NULL, NULL, "paste-buffer", "-p", "-d", "-b", buf, "-t", pane, NULL) == 0 ? 0 : -1;
}

int tmux_enter(const char *pane)
{
    return tmux(NULL, NULL, "send-keys", "-t", pane, "Enter", NULL) == 0 ? 0 : -1;
}

int tmux_set_label(const char *pane, const char *label)
{
    /* A user option rather than the pane title: agents overwrite the title. */
#ifdef _WIN32
    /* psmux draws pane borders with only built-in variables expanded, so
     * the label goes into the title too; psmux does not let the program in
     * the pane change it. */
    tmux(NULL, NULL, "select-pane", "-t", pane, "-T", label, NULL);
#endif
    return tmux(NULL, NULL, "set-option", "-p", "-t", pane, "@nq_label", label, NULL) == 0 ? 0 : -1;
}

/* First line of a tmux query, trimmed. */
static void query(char *buf, size_t len, const char *target, const char *format)
{
    sb_t out = {0};
    buf[0] = '\0';
    if (tmux(&out, NULL, "display-message", "-p", "-t", target, format, NULL) == 0 && out.p)
        snprintf(buf, len, "%.*s", (int)strcspn(out.p, "\n"), out.p);
    sb_free(&out);
}

static void main_vertical(const char *window)
{
    tmux(NULL, NULL, "set-option", "-w", "-t", window, "main-pane-width", "50%", NULL);
    tmux(NULL, NULL, "select-layout", "-t", window, "main-vertical", NULL);
}

void tmux_layout(const char *session, const char *main_pane)
{
    char t[128], main_win[32] = "";
    exact(t, sizeof t, session);
    sb_t wins = {0};
    if (tmux(&wins, NULL, "list-windows", "-t", t, "-F", "#{window_id}", NULL) != 0 || !wins.p) {
        sb_free(&wins);
        return;
    }
    if (main_pane) query(main_win, sizeof main_win, main_pane, "#{window_id}");
    if (!main_win[0]) snprintf(main_win, sizeof main_win, "%.*s", (int)strcspn(wins.p, "\n"), wins.p);

    if (main_pane) {
        /* main-vertical puts the window's first pane on the left. */
        sb_t panes = {0};
        if (tmux(&panes, NULL, "list-panes", "-t", main_win, "-F", "#{pane_id}", NULL) == 0 && panes.p) {
            char first[32];
            snprintf(first, sizeof first, "%.*s", (int)strcspn(panes.p, "\n"), panes.p);
            if (strcmp(first, main_pane))
                tmux(NULL, NULL, "swap-pane", "-d", "-s", main_pane, "-t", first, NULL);
        }
        sb_free(&panes);
    }

    char *save = NULL;
    for (char *w = strtok_r(wins.p, "\n", &save); w; w = strtok_r(NULL, "\n", &save)) {
        if (!strcmp(w, main_win))
            main_vertical(w);
        else
            tmux(NULL, NULL, "select-layout", "-t", w, "tiled", NULL);
    }
    sb_free(&wins);
}

int tmux_main_window_full(const char *window)
{
    char h[16], n[16];
    query(h, sizeof h, window, "#{window_height}");
    query(n, sizeof n, window, "#{window_panes}");
    /* Each pane on the right needs about 6 rows plus its border line. */
    int rows = atoi(h), panes = atoi(n), right = rows / 7;
    if (right < 1) right = 1;
    return panes >= 1 + right;
}

#ifdef _WIN32
/* psmux stores the session options of the session a client last attached
 * to in the global options, so setting them would change every session on
 * the server. The grid's options are put in place only around an attach,
 * and the global values restored afterwards. */
#define GRID_OPTS 3
static const char *const grid_opt[GRID_OPTS] = {"key-table", "status-right", "status-right-length"};

static void global_opts(char vals[GRID_OPTS][128])
{
    for (int i = 0; i < GRID_OPTS; i++) {
        sb_t out = {0};
        vals[i][0] = '\0';
        if (tmux(&out, NULL, "show-options", "-g", "-v", grid_opt[i], NULL) == 0 && out.p)
            snprintf(vals[i], sizeof vals[i], "%.*s", (int)strcspn(out.p, "\r\n"), out.p);
        sb_free(&out);
    }
}

static void restore_global_opts(char vals[GRID_OPTS][128])
{
    char now[GRID_OPTS][128];
    global_opts(now);
    for (int i = 0; i < GRID_OPTS; i++)
        if (strcmp(now[i], vals[i])) tmux(NULL, NULL, "set-option", "-g", grid_opt[i], vals[i], NULL);
}
#endif

static void grid_session_opts(const char *t)
{
    tmux(NULL, NULL, "set-option", "-t", t, "key-table", "nuq", NULL);
    tmux(NULL, NULL, "set-option", "-t", t, "status-right", " F12 / Ctrl-q: back to nuq ", NULL);
    tmux(NULL, NULL, "set-option", "-t", t, "status-right-length", "40", NULL);
}

void tmux_grid_keys(const char *session)
{
    /* "=name:" is the exact-match form set-option accepts. */
    char t[130];
    snprintf(t, sizeof t, "=%s:", session);
    tmux(NULL, NULL, "bind-key", "-T", "nuq", "F12", "detach-client", NULL);
    tmux(NULL, NULL, "bind-key", "-T", "nuq", "C-q", "detach-client", NULL);
#ifdef _WIN32
    char saved[GRID_OPTS][128];
    global_opts(saved);
    grid_session_opts(t);
    restore_global_opts(saved);
#else
    grid_session_opts(t);
#endif
}

int tmux_inside_session(const char *session)
{
    const char *env = getenv("TMUX"), *pane = getenv("TMUX_PANE");
    if (!env || !*env || !pane || !*pane) return 0;
    char t[130], sock[1024];
    snprintf(t, sizeof t, "=%s:", session);
    query(sock, sizeof sock, t, "#{socket_path}");
    /* $TMUX is "<socket path>,<pid>,<session index>". */
    size_t n = strcspn(env, ",");
#ifdef _WIN32
    /* psmux puts a made-up "/tmp/psmux-<pid>/<name>" in $TMUX, while
     * #{socket_path} is "C:\Users\x\.psmux/<name>": only the server name
     * (the -L name) is common to both. */
    char envsock[1024];
    snprintf(envsock, sizeof envsock, "%.*s", (int)n, env);
    if (!sock[0] || _stricmp(nq_basename(sock), nq_basename(envsock))) return 0;
#else
    if (!sock[0] || strlen(sock) != n || strncmp(sock, env, n)) return 0;
#endif
    char name[128];
    query(name, sizeof name, pane, "#{session_name}");
    return !strcmp(name, session);
}

int tmux_takeover(const char *session, const char *pane)
{
    char t[128];
    exact(t, sizeof t, session);
    tmux_grid_keys(session);
    int zoomed = 0;
    if (pane) {
        char flag[8], count[8];
        tmux(NULL, NULL, "select-window", "-t", pane, NULL);
        tmux(NULL, NULL, "select-pane", "-t", pane, NULL);
        query(flag, sizeof flag, pane, "#{window_zoomed_flag}");
        query(count, sizeof count, pane, "#{window_panes}");
        if (strcmp(flag, "1") && atoi(count) > 1 &&
            tmux(NULL, NULL, "resize-pane", "-Z", "-t", pane, NULL) == 0)
            zoomed = 1;
    }

    fflush(stdout);
#ifdef _WIN32
    /* Attaching from inside another psmux is refused while TMUX or
     * PSMUX_SESSION is set ("sessions should be nested with care"); nesting
     * is what we want here. The child inherits our environment, so they
     * are taken out of it for the duration. */
    static const char *const nest[] = {"TMUX", "PSMUX_SESSION", "PSMUX_TARGET_SESSION"};
    char *saved[3];
    const char *sock = getenv("NUQ_TMUX_SOCKET");
    const char *argv[7];
    int k = 0;
    argv[k++] = "tmux";
    if (sock && *sock) {
        argv[k++] = "-L";
        argv[k++] = sock;
    }
    argv[k++] = "attach-session";
    argv[k++] = "-t";
    argv[k++] = t;
    argv[k] = NULL;
    for (int i = 0; i < 3; i++) {
        const char *v = getenv(nest[i]);
        saved[i] = v ? xstrdup(v) : NULL;
        if (v) nq_unsetenv(nest[i]);
    }
    char st[130], gvals[GRID_OPTS][128];
    snprintf(st, sizeof st, "=%s:", session);
    global_opts(gvals);
    grid_session_opts(st);
    int rc = nq_spawn_wait(argv);
    restore_global_opts(gvals);
    for (int i = 0; i < 3; i++) {
        if (!saved[i]) continue;
        nq_setenv(nest[i], saved[i]);
        free(saved[i]);
    }
#else
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        /* Attaching from inside another tmux is refused while $TMUX is set;
         * nesting is what we want here. */
        unsetenv("TMUX");
        const char *sock = getenv("NUQ_TMUX_SOCKET");
        if (sock && *sock)
            execlp("tmux", "tmux", "-L", sock, "attach-session", "-t", t, (char *)NULL);
        else
            execlp("tmux", "tmux", "attach-session", "-t", t, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    int rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#endif

    if (zoomed) {
        char flag[8];
        query(flag, sizeof flag, pane, "#{window_zoomed_flag}");
        if (!strcmp(flag, "1")) tmux(NULL, NULL, "resize-pane", "-Z", "-t", pane, NULL);
    }
    return rc;
}
