#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "adapters.h"
#include "cli.h"
#include "daemon.h"
#include "store.h"
#include "tmux.h"
#include "util.h"

extern char **environ;

/* `nuq [-n NAME] [--sub a,b] <agent> [args...]`: start an agent in a new
 * pane of this directory's grid, creating the grid if this is the first. */

/* The pane must see the caller's environment (PATH, API keys, virtualenv),
 * not the tmux server's. It is handed over in a private file rather than
 * on a command line, where other users could read it with ps. */
static char *write_env_file(const nq_ctx *c, const char *name)
{
    sb_t path = {0};
    sb_addf(&path, "%s/env-%s", c->p.rundir, name);
    int fd = open(path.p, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) die("cannot write %s", path.p);
    for (char **e = environ; *e; e++) {
        size_t len = strlen(*e) + 1; /* entries are NUL-separated */
        if (write(fd, *e, len) != (ssize_t)len) die("cannot write %s", path.p);
    }
    close(fd);
    return path.p;
}

/* Splits a config value into words: whitespace separates, single and
 * double quotes group, a backslash outside single quotes escapes. */
static void split_words(const char *s, av_t *out, av_t *owned)
{
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        sb_t w = {0};
        sb_add(&w, "", 0);
        char quote = 0;
        for (; *s && (quote || (*s != ' ' && *s != '\t')); s++) {
            if (quote) {
                if (*s == quote)
                    quote = 0;
                else if (*s == '\\' && quote == '"' && s[1])
                    sb_add(&w, ++s, 1);
                else
                    sb_add(&w, s, 1);
            } else if (*s == '\'' || *s == '"') {
                quote = *s;
            } else if (*s == '\\' && s[1]) {
                sb_add(&w, ++s, 1);
            } else {
                sb_add(&w, s, 1);
            }
        }
        av_add(owned, w.p);
        av_add(out, w.p);
    }
}

/* Default arguments for an agent, from ${XDG_CONFIG_HOME:-~/.config}/nuq/config:
 *
 *     # comment
 *     claude.args = --model opus
 *
 * This is where flags belong that would otherwise live in a shell alias:
 * nuq starts agents without a shell, so aliases are not seen. */
static void config_args(const char *kind, av_t *out, av_t *owned)
{
    char path[PATH_MAX];
    const char *base = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (base && *base)
        snprintf(path, sizeof path, "%s/nuq/config", base);
    else if (home && *home)
        snprintf(path, sizeof path, "%s/.config/nuq/config", home);
    else
        return;
    FILE *f = fopen(path, "r");
    if (!f) return;

    char key[64];
    snprintf(key, sizeof key, "%s.args", kind);
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, f) > 0) {
        char *p = line + strspn(line, " \t");
        if (*p == '#' || *p == '\n' || !*p) continue;
        p[strcspn(p, "\r\n")] = '\0';
        char *eq = strchr(p, '=');
        if (!eq) continue;
        char *end = eq;
        while (end > p && (end[-1] == ' ' || end[-1] == '\t')) end--;
        if ((size_t)(end - p) != strlen(key) || strncmp(p, key, strlen(key))) continue;
        split_words(eq + 1, out, owned);
    }
    free(line);
    fclose(f);
}

static void forget(const nq_ctx *c, const char *name)
{
    cJSON *q = cli_req("bye");
    cJSON_AddStringToObject(q, "name", name);
    cJSON_Delete(cli_rpc(c, q, 1000));
}

/* First line of tmux's output, trimmed: a pane id on success. */
static void first_line(sb_t *out, char *buf, size_t len)
{
    size_t n = out->p ? strcspn(out->p, "\n") : 0;
    snprintf(buf, len, "%.*s", (int)n, out->p ? out->p : "");
}

/* Applies the grid layout with the broker's main agent on the left.
 * first_pane: the pane just created for a new grid (the main agent-to-be,
 * not registered yet). */
static void layout_now(const nq_ctx *c, const char *first_pane)
{
    cJSON *r = first_pane ? NULL : cli_rpc(c, cli_req("ls"), 1000);
    const char *main_pane = first_pane ? first_pane : jstr(r, "main_pane");
    tmux_layout(c->p.session, main_pane);
    cJSON_Delete(r);
}

/* `nuq _layout`, run by tmux when the window is resized. */
int cmd_layout(nq_ctx *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (tmux_has_session(c->p.session)) layout_now(c, NULL);
    return 0;
}

int cmd_launch(nq_ctx *c, const nq_launch_opts *o, int argc, char **argv)
{
    char err[512], agent_path[PATH_MAX], self[PATH_MAX];
    if (argc < 1) die("usage: nuq [-n NAME] [--sub TOPICS] <agent> [agent args...]");

    const char *kind = strrchr(argv[0], '/');
    kind = kind ? kind + 1 : argv[0];
    if (find_on_path(argv[0], agent_path, sizeof agent_path) < 0)
        die("'%s' was not found on your PATH", argv[0]);
    if (tmux_check(err, sizeof err) < 0) die("%s", err);
    if (self_exe(self, sizeof self) < 0) die("cannot locate the nuq executable");
    if (daemon_ensure(&c->p, err, sizeof err) < 0) die("%s", err);

    int mode = adapter_mode(kind, o->inject);
    cJSON *q = cli_req("alloc");
    if (o->name) cJSON_AddStringToObject(q, "name", o->name);
    cJSON_AddStringToObject(q, "kind", kind);
    cJSON_AddNumberToObject(q, "mode", mode);
    cJSON *r = cli_call(c, q);
    char name[48];
    snprintf(name, sizeof name, "%s", jstr(r, "name"));
    cJSON_Delete(r);

    char *envfile = write_env_file(c, name);
    char modestr[8];
    snprintf(modestr, sizeof modestr, "%d", mode);

    /* What runs in the pane: nuq's own wrapper, which starts the agent. */
    av_t cmd = {0};
    av_add(&cmd, self);
    av_add(&cmd, "-C");
    av_add(&cmd, c->p.dir);
    av_add(&cmd, "_pane");
    av_add(&cmd, name);
    av_add(&cmd, kind);
    av_add(&cmd, modestr);
    av_add(&cmd, envfile);
    av_add(&cmd, o->subs && *o->subs ? o->subs : "-");
    av_add(&cmd, "--");
    av_add(&cmd, agent_path);
    av_t owned = {0};
    config_args(kind, &cmd, &owned);
    for (int i = 1; i < argc; i++) av_add(&cmd, argv[i]);

    char target[128], pane[32] = "";
    snprintf(target, sizeof target, "=%s", c->p.session);
    sb_t out = {0};
    int created = 0, fresh = 0;

    if (!tmux_has_session(c->p.session)) {
        av_t a = {0};
        char cols[16], rows[16];
        struct winsize ws;
        av_add(&a, "new-session");
        av_add(&a, "-d");
        av_add(&a, "-s");
        av_add(&a, c->p.session);
        av_add(&a, "-n");
        av_add(&a, "grid");
        av_add(&a, "-c");
        av_add(&a, c->p.dir);
        /* Size the detached session like the terminal that will attach. */
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
            snprintf(cols, sizeof cols, "%d", ws.ws_col);
            snprintf(rows, sizeof rows, "%d", ws.ws_row);
            av_add(&a, "-x");
            av_add(&a, cols);
            av_add(&a, "-y");
            av_add(&a, rows);
        }
        av_add(&a, "-P");
        av_add(&a, "-F");
        av_add(&a, "#{pane_id}");
        av_add(&a, "--");
        for (size_t i = 0; i < cmd.n; i++) av_add(&a, cmd.v[i]);
        int rc = tmux_argv(&a, NULL, &out);
        av_free(&a);
        if (rc != 0) {
            forget(c, name);
            die("tmux could not create the grid: %s", out.p ? out.p : "(no output)");
        }
        first_line(&out, pane, sizeof pane);
        created = fresh = 1;
    } else {
        /* Split whichever window still has room; open another if none has. */
        sb_t wins = {0};
        tmux(&wins, NULL, "list-windows", "-t", target, "-F", "#{window_id}", NULL);
        char *save = NULL;
        int first_window = 1;
        for (char *w = wins.p ? strtok_r(wins.p, "\n", &save) : NULL; w && !pane[0];
             w = strtok_r(NULL, "\n", &save), first_window = 0) {
            /* The main window keeps the main agent at half width, so its
             * right column fills up after a few agents. */
            if (first_window && tmux_main_window_full(w)) continue;
            for (int attempt = 0; attempt < 2 && !pane[0]; attempt++) {
                if (attempt == 1) {
                    if (first_window)
                        tmux_layout(c->p.session, NULL);
                    else
                        tmux(NULL, NULL, "select-layout", "-t", w, "tiled", NULL);
                }
                /* Split the window's last pane, so agents stack in the
                 * order they joined. */
                char last[32] = "";
                sb_t pl = {0};
                if (tmux(&pl, NULL, "list-panes", "-t", w, "-F", "#{pane_id}", NULL) == 0 && pl.p) {
                    char *nl, *line = pl.p;
                    while ((nl = strchr(line, '\n')) && nl[1]) line = nl + 1;
                    snprintf(last, sizeof last, "%.*s", (int)strcspn(line, "\n"), line);
                }
                sb_free(&pl);
                av_t a = {0};
                av_add(&a, "split-window");
                av_add(&a, "-d");
                av_add(&a, "-v");
                av_add(&a, "-t");
                av_add(&a, last[0] ? last : w);
                av_add(&a, "-c");
                av_add(&a, c->p.dir);
                av_add(&a, "-P");
                av_add(&a, "-F");
                av_add(&a, "#{pane_id}");
                av_add(&a, "--");
                for (size_t i = 0; i < cmd.n; i++) av_add(&a, cmd.v[i]);
                sb_free(&out);
                if (tmux_argv(&a, NULL, &out) == 0) first_line(&out, pane, sizeof pane);
                av_free(&a);
            }
        }
        sb_free(&wins);
        if (!pane[0]) {
            av_t a = {0};
            char sess[130];
            snprintf(sess, sizeof sess, "%s:", target);
            av_add(&a, "new-window");
            av_add(&a, "-d");
            av_add(&a, "-t");
            av_add(&a, sess);
            av_add(&a, "-n");
            av_add(&a, "grid");
            av_add(&a, "-c");
            av_add(&a, c->p.dir);
            av_add(&a, "-P");
            av_add(&a, "-F");
            av_add(&a, "#{pane_id}");
            av_add(&a, "--");
            for (size_t i = 0; i < cmd.n; i++) av_add(&a, cmd.v[i]);
            sb_free(&out);
            int rc = tmux_argv(&a, NULL, &out);
            av_free(&a);
            if (rc != 0) {
                forget(c, name);
                die("tmux could not add a pane: %s", out.p ? out.p : "(no output)");
            }
            first_line(&out, pane, sizeof pane);
            created = 1;
        }
    }
    sb_free(&out);
    av_free(&cmd);
    for (size_t i = 0; i < owned.n; i++) free((char *)owned.v[i]);
    av_free(&owned);
    free(envfile);

    if (fresh) {
        /* Keep the main agent at half the width when the terminal is
         * resized. A session hook, so other sessions are untouched. */
        sb_t hook = {0};
        sb_adds(&hook, "run-shell -b \"");
        sb_add_shq(&hook, self);
        sb_adds(&hook, " -C ");
        sb_add_shq(&hook, c->p.dir);
        sb_adds(&hook, " _layout\"");
        tmux(NULL, NULL, "set-hook", "-t", target, "window-resized", hook.p, NULL);
        sb_free(&hook);
    }
    if (created) {
        /* Name each pane in its border. Set on the window, not globally, so
         * the user's other tmux sessions are untouched. */
        tmux(NULL, NULL, "set-option", "-w", "-t", pane, "pane-border-status", "top", NULL);
        tmux(NULL, NULL, "set-option", "-w", "-t", pane, "pane-border-format",
             " #{?@nq_label,#{@nq_label},#{pane_title}} ", NULL);
    }
    layout_now(c, fresh ? pane : NULL);

    printf("%s started in pane %s of tmux session %s\n", name, pane, c->p.session);
    fflush(stdout);
    tmux_grid_keys(c->p.session);
    if (o->detach || !isatty(STDOUT_FILENO) || !isatty(STDIN_FILENO)) return 0;
    /* An agent (or anyone working in a pane of the grid) starting another
     * agent stays where it is. */
    if (getenv("NUQ_AGENT") || tmux_inside_session(c->p.session)) return 0;

    /* Everyone else gets the nuq UI rather than raw tmux: agents are
     * reached with :takeover and left again with F12 or Ctrl-q. */
    int rc = cmd_dash(c, 0, NULL);
    printf("agents keep running; reopen the UI with: nuq\n");
    return rc;
}
