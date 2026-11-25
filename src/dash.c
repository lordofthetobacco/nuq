/* `nuq` with no arguments (or `nuq dash`): a live view of the grid.
 *
 *   header      directory, agents, work queue, git totals
 *   main        agent roster and selected pane details alongside live messages
 *   bottom      changed files: git's view next to what each agent edited
 *
 * `nuq dash --once` prints the same information once as plain text. */
#define _XOPEN_SOURCE 700  /* wcwidth */
#include <curses.h>
#include <locale.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#include "bus.h"
#include "cli.h"
#include "daemon.h"
#include "proto.h"
#include "tmux.h"
#include "util.h"

#define FLOW_MAX 300
#define FLASH_MS 1500

enum { C_IDLE = 1, C_BUSY, C_BLOCKED, C_ACCENT, C_TOPIC };

typedef struct {
    char *text;
    int kind;
    uint64_t ts;
} flow_line;

typedef struct {
    char name[48];
    char line[256];
} pane_line;

typedef struct {
    char name[48];
    uint64_t until;
    int dir; /* 1: sending, 2: receiving */
} flash_t;

typedef struct {
    nq_ctx *c;
    cJSON *stats;      /* last `stats` reply, NULL when no broker answers */
    int sub, subfd;
    flow_line flow[FLOW_MAX];
    size_t n_flow;
    cJSON *git;        /* [{path, added, removed}] */
    long git_add, git_del;
    int is_git;
    pane_line *panes;
    size_t n_panes;
    flash_t flash[16];
    int sel, top;
    int help, help_top;
    char status[256];
    uint64_t status_until;
    uint64_t t_stats, t_git, t_panes;
} dash_t;

/* ---- data --------------------------------------------------------------- */

static void flow_add(dash_t *d, const char *text, int kind, uint64_t ts)
{
    if (d->n_flow == FLOW_MAX) {
        free(d->flow[0].text);
        memmove(d->flow, d->flow + 1, (FLOW_MAX - 1) * sizeof *d->flow);
        d->n_flow--;
    }
    d->flow[d->n_flow++] = (flow_line){xstrdup(text), kind, ts};
}

static void mark_flash(dash_t *d, const char *name, int dir)
{
    size_t slot = 0;
    for (size_t i = 0; i < 16; i++) {
        if (!strcmp(d->flash[i].name, name)) {
            slot = i;
            break;
        }
        if (d->flash[i].until < d->flash[slot].until) slot = i;
    }
    snprintf(d->flash[slot].name, sizeof d->flash[slot].name, "%s", name);
    d->flash[slot].until = now_ms() + FLASH_MS;
    d->flash[slot].dir = dir;
}

static int flashing(const dash_t *d, const char *name)
{
    uint64_t now = now_ms();
    for (size_t i = 0; i < 16; i++)
        if (d->flash[i].until > now && !strcmp(d->flash[i].name, name)) return d->flash[i].dir;
    return 0;
}

static void disconnect(dash_t *d)
{
    if (d->sub >= 0) bus_close(d->sub);
    d->sub = d->subfd = -1;
    cJSON_Delete(d->stats);
    d->stats = NULL;
}

static void fetch_stats(dash_t *d)
{
    d->t_stats = now_ms();
    cJSON *r = daemon_alive(&d->c->p) ? cli_rpc(d->c, cli_req("stats"), 800) : NULL;
    if (!r || !jbool(r, "ok")) {
        cJSON_Delete(r);
        disconnect(d);
        return;
    }
    cJSON_Delete(d->stats);
    d->stats = r;
    if (d->sub < 0) {
        d->sub = bus_sub_connect(d->c->p.ev_url);
        if (d->sub >= 0) {
            bus_sub_add(d->sub, "msg ");
            bus_sub_add(d->sub, "ev ");
            d->subfd = bus_rcvfd(d->sub);
        }
    }
    /* Seed the flow with recent history the first time. */
    if (d->n_flow == 0) {
        const cJSON *m;
        cJSON_ArrayForEach(m, cJSON_GetObjectItemCaseSensitive(r, "recent"))
        {
            sb_t line = {0};
            proto_fmt_line(&line, m, 200);
            const char *k = jstr(m, "kind");
            int kind = !k || !strcmp(k, "dm") ? EV_DM : !strcmp(k, "all") ? EV_ALL
                       : !strcmp(k, "topic")  ? EV_TOPIC : EV_WORK;
            flow_add(d, line.p, kind, (uint64_t)jnum(m, "ts", 0));
            sb_free(&line);
        }
    }
}

static int drain_events(dash_t *d)
{
    int got = 0;
    for (char *frame; d->sub >= 0 && (frame = bus_recv(d->sub, BUS_DONTWAIT, NULL));) {
        sb_t line = {0};
        int kind;
        cJSON *o = cli_fmt_event(frame, &line, &kind);
        if (o) {
            flow_add(d, line.p ? line.p : "", kind, (uint64_t)jnum(o, "ts", (double)now_ms()));
            if (!strncmp(frame, "msg ", 4)) {
                const char *from = jstr(o, "from");
                if (from) mark_flash(d, from, 1);
                const cJSON *r;
                cJSON_ArrayForEach(r, cJSON_GetObjectItemCaseSensitive(o, "rcpt"))
                {
                    if (cJSON_IsString(r)) mark_flash(d, r->valuestring, 2);
                }
            }
            got = 1;
        }
        sb_free(&line);
        cJSON_Delete(o);
        free(frame);
    }
    return got;
}

/* git's view of the working tree, relative to the grid directory. */
static void fetch_git(dash_t *d)
{
    d->t_git = now_ms();
    cJSON_Delete(d->git);
    d->git = cJSON_CreateArray();
    d->git_add = d->git_del = 0;
    const char *probe[] = {"git", "-C", d->c->p.dir, "rev-parse", "--is-inside-work-tree", NULL};
    sb_t out = {0};
    d->is_git = run_cmd(probe, NULL, 0, &out, 0) == 0;
    sb_free(&out);
    if (!d->is_git) return;
    const char *withhead[] = {"git", "-C", d->c->p.dir, "diff", "--numstat", "--relative", "HEAD", NULL};
    const char *nohead[] = {"git", "-C", d->c->p.dir, "diff", "--numstat", "--relative", NULL};
    if (run_cmd(withhead, NULL, 0, &out, 0) != 0) {
        sb_free(&out);
        run_cmd(nohead, NULL, 0, &out, 0);
    }
    char *save = NULL;
    for (char *l = out.p ? strtok_r(out.p, "\n", &save) : NULL; l; l = strtok_r(NULL, "\n", &save)) {
        /* "<added>\t<removed>\t<path>"; binary files show "-". */
        char *t1 = strchr(l, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t2) continue;
        long a = atol(l), r = atol(t1 + 1);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "path", t2 + 1);
        cJSON_AddNumberToObject(o, "added", (double)a);
        cJSON_AddNumberToObject(o, "removed", (double)r);
        cJSON_AddItemToArray(d->git, o);
        d->git_add += a;
        d->git_del += r;
    }
    sb_free(&out);
}

/* The last non-blank line of each agent's pane. */
static void fetch_panes(dash_t *d)
{
    d->t_panes = now_ms();
    free(d->panes);
    d->panes = NULL;
    d->n_panes = 0;
    const cJSON *a;
    cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(d->stats, "agents"))
    {
        const char *pane = jstr(a, "pane"), *name = jstr(a, "name");
        if (!pane || !name) continue;
        sb_t cap = {0};
        tmux_capture(pane, &cap);
        char last[256] = "";
        for (char *l = cap.p, *nl; l && *l; l = nl ? nl + 1 : NULL) {
            nl = strchr(l, '\n');
            size_t len = nl ? (size_t)(nl - l) : strlen(l);
            while (len && (l[len - 1] == ' ' || l[len - 1] == '\r')) len--;
            size_t lead = 0;
            while (lead < len && l[lead] == ' ') lead++;
            if (len > lead) snprintf(last, sizeof last, "%.*s", (int)(len - lead), l + lead);
            if (!nl) break;
        }
        sb_free(&cap);
        d->panes = xrealloc(d->panes, (d->n_panes + 1) * sizeof *d->panes);
        snprintf(d->panes[d->n_panes].name, sizeof d->panes[0].name, "%s", name);
        snprintf(d->panes[d->n_panes].line, sizeof d->panes[0].line, "%s", last);
        d->n_panes++;
    }
}

static const char *pane_text(const dash_t *d, const char *name)
{
    for (size_t i = 0; i < d->n_panes; i++)
        if (!strcmp(d->panes[i].name, name)) return d->panes[i].line;
    return "";
}

/* ---- formatting shared by both views ------------------------------------ */

static void fmt_dur(char *buf, size_t len, uint64_t ms)
{
    uint64_t s = ms / 1000;
    if (s < 60)
        snprintf(buf, len, "%us", (unsigned)s);
    else if (s < 3600)
        snprintf(buf, len, "%um%02us", (unsigned)(s / 60), (unsigned)(s % 60));
    else
        snprintf(buf, len, "%uh%02um", (unsigned)(s / 3600), (unsigned)(s % 3600 / 60));
}

static void fmt_clock(char *buf, size_t len, uint64_t ts)
{
    time_t t = (time_t)(ts / 1000);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(buf, len, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void fmt_doing(char *buf, size_t len, const cJSON *a, uint64_t now)
{
    const char *tool = jstr(a, "last_tool");
    if (!tool) {
        snprintf(buf, len, "doing: -");
        return;
    }
    char ago[16];
    fmt_dur(ago, sizeof ago, now - (uint64_t)jnum(a, "last_ts", (double)now));
    snprintf(buf, len, "doing: %s, %s ago", tool, ago);
}

static void fmt_counts(char *buf, size_t len, const cJSON *a)
{
    snprintf(buf, len, "msgs in %.0f out %.0f   lines +%.0f -%.0f in %.0f file%s   tasks done %.0f failed %.0f",
             jnum(a, "recv", 0), jnum(a, "sent", 0), jnum(a, "added", 0), jnum(a, "removed", 0),
             jnum(a, "files", 0), jnum(a, "files", 0) == 1 ? "" : "s", jnum(a, "tasks_done", 0),
             jnum(a, "tasks_failed", 0));
}

/* Rows for the files table: the union of git's files and agents' edits. */
typedef struct {
    const char *path;
    long gadd, gdel, aadd, adel;
    char who[96];
} file_row;

static size_t file_rows(const dash_t *d, file_row **out)
{
    size_t n = 0;
    file_row *rows = NULL;
    const cJSON *g;
    cJSON_ArrayForEach(g, d->git)
    {
        rows = xrealloc(rows, (n + 1) * sizeof *rows);
        rows[n] = (file_row){jstr(g, "path"), (long)jnum(g, "added", 0), (long)jnum(g, "removed", 0), -1, -1, ""};
        n++;
    }
    const cJSON *f;
    cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(d->stats, "files"))
    {
        const char *path = jstr(f, "path");
        size_t i = 0;
        while (i < n && strcmp(rows[i].path, path)) i++;
        if (i == n) {
            rows = xrealloc(rows, (n + 1) * sizeof *rows);
            rows[n] = (file_row){path, -1, -1, 0, 0, ""};
            n++;
        }
        rows[i].aadd = (long)jnum(f, "added", 0);
        rows[i].adel = (long)jnum(f, "removed", 0);
        sb_t who = {0};
        const cJSON *w;
        cJSON_ArrayForEach(w, cJSON_GetObjectItemCaseSensitive(f, "who"))
        {
            if (who.len) sb_adds(&who, ",");
            sb_adds(&who, w->valuestring);
        }
        snprintf(rows[i].who, sizeof rows[i].who, "%s", who.p ? who.p : "");
        sb_free(&who);
    }
    /* Busiest first. */
    for (size_t i = 1; i < n; i++)
        for (size_t j = i; j > 0; j--) {
            long a = (rows[j].gadd > 0 ? rows[j].gadd + rows[j].gdel : 0) + (rows[j].aadd > 0 ? rows[j].aadd + rows[j].adel : 0);
            long b = (rows[j - 1].gadd > 0 ? rows[j - 1].gadd + rows[j - 1].gdel : 0) +
                     (rows[j - 1].aadd > 0 ? rows[j - 1].aadd + rows[j - 1].adel : 0);
            if (a <= b) break;
            file_row t = rows[j];
            rows[j] = rows[j - 1];
            rows[j - 1] = t;
        }
    *out = rows;
    return n;
}

static void fmt_file_row(char *buf, size_t len, const file_row *r)
{
    char g[32] = "", a[32] = "";
    if (r->gadd >= 0) snprintf(g, sizeof g, "+%ld -%ld", r->gadd, r->gdel);
    if (r->aadd >= 0) snprintf(a, sizeof a, "+%ld -%ld", r->aadd, r->adel);
    snprintf(buf, len, "%-14s %-14s %s%s%s%s", g[0] ? g : "-", a[0] ? a : "-", r->path,
             r->who[0] ? "  (" : "", r->who, r->who[0] ? ")" : "");
}

/* ---- plain-text view ---------------------------------------------------- */

static int print_once(dash_t *d)
{
    fetch_stats(d);
    if (!d->stats) {
        printf("no grid is running for %s\n", d->c->p.dir);
        return 1;
    }
    fetch_git(d);
    fetch_panes(d);
    uint64_t now = now_ms();
    const cJSON *agents = cJSON_GetObjectItemCaseSensitive(d->stats, "agents"), *a;
    int n = 0;
    cJSON_ArrayForEach(a, agents)
    {
        if (jstr(a, "pane")) n++;
    }
    printf("nuq grid %s\n", d->c->p.dir);
    printf("agents %d   queue %.0f queued, %.0f in progress", n, jnum(d->stats, "queued", 0),
           jnum(d->stats, "claimed", 0));
    if (d->is_git) printf("   git +%ld -%ld in %d files", d->git_add, d->git_del, cJSON_GetArraySize(d->git));
    printf("\n\n");
    cJSON_ArrayForEach(a, agents)
    {
        if (!jstr(a, "pane")) continue;
        char dur[16], doing[160], counts[256];
        fmt_dur(dur, sizeof dur, now - (uint64_t)jnum(a, "state_ts", (double)now));
        fmt_doing(doing, sizeof doing, a, now);
        fmt_counts(counts, sizeof counts, a);
        printf("%s %s (%s)  %s %s", jbool(a, "main") ? "*" : " ", jstr(a, "name"), jstr(a, "kind"),
               jstr(a, "state"), dur);
        if (jnum(a, "task", 0)) printf("  task #%.0f", jnum(a, "task", 0));
        printf("\n    %s\n    %s\n    | %s\n", doing, counts, pane_text(d, jstr(a, "name")));
    }
    printf("\nmessage flow:\n");
    for (size_t i = d->n_flow > 10 ? d->n_flow - 10 : 0; i < d->n_flow; i++) {
        char clk[16];
        fmt_clock(clk, sizeof clk, d->flow[i].ts);
        printf("  %s %s\n", clk, d->flow[i].text);
    }
    file_row *rows;
    size_t nr = file_rows(d, &rows);
    printf("\nchanged files:  %-14s %-14s %s\n", "git", "agents", "path");
    for (size_t i = 0; i < nr; i++) {
        char line[512];
        fmt_file_row(line, sizeof line, &rows[i]);
        printf("                %s\n", line);
    }
    free(rows);
    return 0;
}

/* ---- curses view -------------------------------------------------------- */

/* Clip by terminal cells, and keep control bytes inside the current row. */
static void put(int y, int x, int w, int attr, const char *s)
{
    if (w <= 0 || y < 0 || y >= LINES || x < 0 || x >= COLS) return;
    if (w > COLS - x) w = COLS - x;
    /* Leave the bottom-right cell alone so curses never tries to scroll. */
    if (y == LINES - 1 && w == COLS - x) w--;

    mbstate_t state = {0};
    size_t len = strlen(s);
    const char *run = s;
    int cols = 0;
    attrset(attr);
    move(y, x);
    while (len) {
        wchar_t ch;
        size_t n = mbrtowc(&ch, s, len, &state);
        if (n == (size_t)-1 || n == (size_t)-2) {
            state = (mbstate_t){0};
            ch = L'\0';
            n = 1;
        }
        int width = ch ? wcwidth(ch) : -1;
        if (width == 0 && cols == 0) width = -1;
        if (width > w - cols || (width < 0 && cols == w)) break;
        if (width < 0) {
            if (s > run) addnstr(run, (int)(s - run));
            addch(ch == L'\t' ? ' ' : '?');
            run = s + n;
            width = 1;
        }
        cols += width;
        s += n;
        len -= n;
    }
    if (s > run) addnstr(run, (int)(s - run));
    attrset(A_NORMAL);
}

__attribute__((format(printf, 5, 6))) static void putf(int y, int x, int w, int attr, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    put(y, x, w, attr, buf);
}

static int state_attr(const char *state)
{
    if (!state) return A_DIM;
    if (!strcmp(state, "idle")) return COLOR_PAIR(C_IDLE);
    if (!strcmp(state, "busy")) return COLOR_PAIR(C_BUSY) | A_BOLD;
    if (!strcmp(state, "blocked")) return COLOR_PAIR(C_BLOCKED) | A_BOLD;
    return A_DIM;
}

static int event_attr(int kind)
{
    switch (kind) {
    case EV_DM: case EV_ALL: return COLOR_PAIR(C_ACCENT);
    case EV_TOPIC: return COLOR_PAIR(C_TOPIC);
    case EV_WORK: return COLOR_PAIR(C_BUSY);
    case EV_EDIT: return COLOR_PAIR(C_IDLE);
    default: return A_DIM;
    }
}

/* Agents with a pane, in the order the broker lists them. */
static int pane_agents(const dash_t *d, const cJSON **out, int max)
{
    int n = 0;
    const cJSON *a;
    cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(d->stats, "agents"))
    {
        if (jstr(a, "pane") && n < max) out[n++] = a;
    }
    return n;
}

static void section(int y, int x, int w, const char *label)
{
    attrset(A_DIM);
    mvhline(y, x, ACS_HLINE, w);
    attrset(A_NORMAL);
    putf(y, x + 2, w - 4, A_BOLD, " %s ", label);
}

static uint64_t age(const cJSON *a, const char *key, uint64_t now)
{
    uint64_t ts = (uint64_t)jnum(a, key, (double)now);
    return ts && ts < now ? now - ts : 0;
}

static void draw_agent_detail(const dash_t *d, const cJSON *a, int y, int w, uint64_t now)
{
    const char *name = jstr(a, "name"), *kind = jstr(a, "kind");
    const char *tool = jstr(a, "last_tool"), *line = pane_text(d, name);
    char title[80], elapsed[16];
    snprintf(title, sizeof title, "%s / %s", name, kind ? kind : "-");
    section(y, 0, w, title);

    fmt_dur(elapsed, sizeof elapsed, age(a, "state_ts", now));
    int flash = flashing(d, name);
    double task = jnum(a, "task", 0);
    char task_text[32] = "";
    if (task) snprintf(task_text, sizeof task_text, "   Task #%.0f", task);
    char state_text[32];
    if (jnum(a, "state_ts", 0) > 0)
        snprintf(state_text, sizeof state_text, "State for %s", elapsed);
    else
        snprintf(state_text, sizeof state_text, "State unreported");
    putf(y + 1, 2, w - 3, A_BOLD, "%s%s%s", state_text, task_text,
         flash == 1 ? "   Sending" : flash == 2 ? "   Receiving" : "");

    fmt_dur(elapsed, sizeof elapsed, age(a, "last_ts", now));
    if (tool)
        putf(y + 2, 2, w - 3, 0, "Tool %s ago   %s", elapsed, tool);
    else
        put(y + 2, 2, w - 3, A_DIM, "No tool activity reported");
    putf(y + 3, 2, w - 3, 0, "Mail  %.0f in / %.0f out", jnum(a, "recv", 0), jnum(a, "sent", 0));
    putf(y + 4, 2, w - 3, 0, "Edits %+.0f -%.0f / %.0f files",
         jnum(a, "added", 0), jnum(a, "removed", 0), jnum(a, "files", 0));
    putf(y + 5, 2, w - 3, 0, "Tasks %.0f done / %.0f failed",
         jnum(a, "tasks_done", 0), jnum(a, "tasks_failed", 0));
    putf(y + 6, 2, w - 3, A_DIM, "> %s", *line ? line : "No pane output");
}

static void draw_agents(dash_t *d, const cJSON **agents, int n, int y, int h, int w, uint64_t now)
{
    int detail = n && h >= 9 ? 7 : 0;
    int fit = h - detail - 1;
    if (d->sel < d->top) d->top = d->sel;
    if (d->sel >= d->top + fit) d->top = d->sel - fit + 1;
    if (d->top > n - fit) d->top = n > fit ? n - fit : 0;
    int end = d->top + fit < n ? d->top + fit : n;
    char title[64];
    if (fit < n)
        snprintf(title, sizeof title, "Agents %d-%d/%d", d->top + 1, end, n);
    else
        snprintf(title, sizeof title, "Agents %d", n);
    section(y, 0, w, title);
    if (!n) put(y + 1, 2, w - 3, A_DIM, "No agents yet");
    for (int i = d->top; i < end; i++) {
        const cJSON *a = agents[i];
        const char *name = jstr(a, "name"), *state = jstr(a, "state");
        int row = y + 1 + i - d->top;
        int active = i == d->sel ? A_REVERSE : A_NORMAL;
        attrset(active);
        mvhline(row, 1, ' ', w - 2);
        attrset(A_NORMAL);
        putf(row, 1, 7, active | A_BOLD, "%c%2d %c", active ? '>' : ' ', i + 1,
             jbool(a, "main") ? '*' : ' ');
        int flash = flashing(d, name);
        put(row, 7, w - 18, active, name);
        put(row, w - 10, 8, active | state_attr(state), state ? state : "-");
        if (flash) put(row, w - 2, 1, active | A_BOLD, flash == 1 ? ">" : "<");
    }
    if (detail) draw_agent_detail(d, agents[d->sel], y + 1 + end - d->top, w, now);
}

static void draw_flow(const dash_t *d, int y, int h, int x, int w)
{
    section(y, x, w, "Message flow");
    if (!d->n_flow) {
        put(y + 1, x + 2, w - 3, A_DIM, "No messages yet");
        return;
    }
    size_t room = (size_t)(h - 1);
    size_t first = d->n_flow > room ? d->n_flow - room : 0;
    for (size_t i = first; i < d->n_flow; i++) {
        char clk[16];
        fmt_clock(clk, sizeof clk, d->flow[i].ts);
        int row = y + 1 + (int)(i - first);
        put(row, x + 2, 8, A_DIM, clk);
        put(row, x + 12, w - 13, event_attr(d->flow[i].kind), d->flow[i].text);
    }
}

static void draw_files(const file_row *rows, size_t n, int y, int h, int w)
{
    char title[64];
    size_t shown = (size_t)(h - 2);
    if (shown < n)
        snprintf(title, sizeof title, "Changed files %zu/%zu", shown, n);
    else
        snprintf(title, sizeof title, "Changed files %zu", n);
    section(y, 0, w, title);

    int delta = w < 48 ? 10 : w < 72 ? 12 : 14;
    int git = w - delta * 2 - 2, agents = git + delta;
    put(y + 1, 2, git - 3, A_DIM, "Path / contributors");
    put(y + 1, git, delta - 1, A_DIM, "Git");
    put(y + 1, agents, delta - 1, A_DIM, "Agents");
    for (size_t i = 0; i < shown; i++) {
        const file_row *r = &rows[i];
        int row = y + 2 + (int)i;
        putf(row, 2, git - 3, 0, "%s%s%s%s", r->path, r->who[0] ? "  [" : "",
             r->who, r->who[0] ? "]" : "");
        if (r->gadd >= 0) putf(row, git, delta - 1, 0, "+%ld -%ld", r->gadd, r->gdel);
        if (r->aadd >= 0) putf(row, agents, delta - 1, 0, "+%ld -%ld", r->aadd, r->adel);
    }
}

static const char *const help_lines[] = {
    "Up/Down or j/k         Select an agent",
    "Enter or t             Take over selected agent",
    "F12 or Ctrl-q          Return from takeover",
    "s                      Message selected agent",
    "r                      Refresh",
    ":                      Open command prompt",
    "q                      Leave dashboard",
    "",
    ":takeover N|name        Show an agent",
    ":send N|name|all text   Send a message",
    ":spawn agent [args]     Start an agent",
    ":kill N|name            Stop an agent",
    ":grid                  Show whole grid",
    ":down!                 Stop every agent",
    ":q                     Leave dashboard",
};

static void draw_help(dash_t *d, int h, int w)
{
    int count = (int)(sizeof help_lines / sizeof *help_lines);
    int room = h > 4 ? h - 4 : 0;
    int max_top = count > room ? count - room : 0;
    if (d->help_top > max_top) d->help_top = max_top;
    if (h >= 4) section(2, 0, w, "Dashboard help");
    for (int i = 0; i < room && d->help_top + i < count; i++)
        put(3 + i, 2, w - 3, 0, help_lines[d->help_top + i]);
    put(h - 1, 1, w - 2, A_BOLD, "Esc / ?  Close help    Up/Down  Scroll");
}

static void draw(dash_t *d)
{
    erase();
    int h = LINES, w = COLS;
    uint64_t now = now_ms();
    const char *base = strrchr(d->c->p.dir, '/');
    put(0, 1, 5, COLOR_PAIR(C_ACCENT) | A_BOLD, "nuq");
    put(0, 8, w - 9, A_BOLD, base && base[1] ? base + 1 : d->c->p.dir);

    if (d->help) {
        draw_help(d, h, w);
    } else {
        if (now < d->status_until)
            put(h - 1, 1, w - 2, COLOR_PAIR(C_BUSY) | A_BOLD, d->status);
        else if (w >= 70)
            put(h - 1, 1, w - 2, A_BOLD,
                "j/k Select   Enter Take over   s Send   : Command   ? Help   q Quit");
        else if (w >= 48)
            put(h - 1, 1, w - 2, A_BOLD, "j/k   Enter   s Send   : Cmd   ? Help   q Quit");
        else if (w >= 32)
            put(h - 1, 1, w - 2, A_BOLD, "j/k  Enter  :  ? Help  q Quit");
        else
            put(h - 1, 1, w - 2, A_BOLD, ": Cmd  ? Help  q Quit");

        if (!d->stats) {
            if (h >= 3) put(1, 1, w - 2, A_DIM, d->c->p.dir);
            if (h >= 6) {
                section(3, 0, w, "No grid running");
                put(4, 2, w - 3, 0, "Start an agent with :spawn claude");
            }
        } else if (h < 10 || w < 32) {
            if (h >= 3) put(1, 1, w - 2, A_DIM, "Resize for dashboard");
        } else {
            const cJSON *agents[64];
            int n = pane_agents(d, agents, 64);
            if (d->sel >= n) d->sel = n ? n - 1 : 0;

            char git[64] = "";
            if (d->is_git)
                snprintf(git, sizeof git, "    Git +%ld -%ld", d->git_add, d->git_del);
            putf(1, 1, w - 2, A_DIM, "Queue %.0f waiting / %.0f active%s",
                 jnum(d->stats, "queued", 0), jnum(d->stats, "claimed", 0), git);

            sb_t people = {0};
            const cJSON *a;
            cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(d->stats, "agents"))
            {
                if (jstr(a, "pane")) continue;
                sb_adds(&people, people.len ? ", " : "People  ");
                sb_adds(&people, jstr(a, "name"));
            }
            if (people.len) put(2, 1, w - 2, A_DIM, people.p);
            sb_free(&people);

            file_row *rows;
            size_t nr = file_rows(d, &rows);
            int wide = w >= 80;
            int files = h >= 16 && nr ? (int)(nr > 6 ? 6 : nr) + 2 : 0;
            if (files > h / 3) files = h / 3;
            int body = h - 4 - files;
            int roster = wide ? body : body * 2 / 3;
            if (!wide && n && body >= 12 && roster < 9) roster = 9;
            if (!wide && roster > n + 8) roster = n + 8;
            int split = wide ? w * 46 / 100 : w;
            draw_agents(d, agents, n, 3, roster, split, now);
            if (wide) {
                attrset(A_DIM);
                mvvline(3, split, ACS_VLINE, body);
                mvaddch(3, split, ACS_TTEE);
                attrset(A_NORMAL);
                draw_flow(d, 3, body, split + 1, w - split - 1);
            } else {
                draw_flow(d, 3 + roster, body - roster, 0, w);
            }
            if (files) {
                draw_files(rows, nr, 3 + body, files, w);
                if (wide) {
                    attrset(A_DIM);
                    mvaddch(3 + body, split, ACS_BTEE);
                    attrset(A_NORMAL);
                }
            }
            free(rows);
        }
    }
    refresh();
}

static void set_status(dash_t *d, const char *msg)
{
    snprintf(d->status, sizeof d->status, "%s", msg);
    d->status_until = now_ms() + (strlen(msg) > 80 ? 10000 : 4000);
}

static const cJSON *selected(const dash_t *d)
{
    const cJSON *cards[64];
    int n = d->stats ? pane_agents(d, cards, 64) : 0;
    return d->sel < n ? cards[d->sel] : NULL;
}

/* Reads one line at the bottom of the screen. Empty on Escape. */
static void prompt_line(const char *label, char *buf, size_t len)
{
    buf[0] = '\0';
    move(LINES - 1, 0);
    clrtoeol();
    put(LINES - 1, 0, COLS, A_BOLD, label);
    echo();
    curs_set(1);
    nodelay(stdscr, FALSE);
    getnstr(buf, (int)len - 1);
    nodelay(stdscr, TRUE);
    curs_set(0);
    noecho();
    if (strchr(buf, 27)) buf[0] = '\0';
}

/* An agent by number in the list (1-based), by name, or by @name. */
static const cJSON *find_agent(const dash_t *d, const char *ref)
{
    const cJSON *cards[64];
    int n = d->stats ? pane_agents(d, cards, 64) : 0;
    if (!ref || !*ref) return NULL;
    if (*ref == '@') ref++;
    char *end;
    long idx = strtol(ref, &end, 10);
    if (!*end && idx >= 1 && idx <= n) return cards[idx - 1];
    for (int i = 0; i < n; i++)
        if (!strcmp(jstr(cards[i], "name"), ref)) return cards[i];
    return NULL;
}

static void send_to(dash_t *d, const char *to, const char *text)
{
    cJSON *q = cli_req("send");
    cJSON_AddStringToObject(q, "from", d->c->me);
    cJSON_AddStringToObject(q, "to", to);
    cJSON_AddStringToObject(q, "body", text);
    cJSON *r = cli_rpc(d->c, q, 2000);
    char msg[512];
    if (r && jbool(r, "ok"))
        snprintf(msg, sizeof msg, "sent #%.0f to %s", jnum(r, "id", 0), jstr(r, "to"));
    else
        snprintf(msg, sizeof msg, "send failed: %s", r && jstr(r, "error") ? jstr(r, "error") : "no broker");
    set_status(d, msg);
    cJSON_Delete(r);
}

/* Hands the terminal to tmux until the user presses F12 or Ctrl-q. */
static void takeover(dash_t *d, const cJSON *a)
{
    const char *pane = a ? jstr(a, "pane") : NULL;
    if (tmux_inside_session(d->c->p.session)) {
        /* The UI itself runs in a pane of the grid: just move there. */
        if (pane) {
            tmux(NULL, NULL, "select-window", "-t", pane, NULL);
            tmux(NULL, NULL, "select-pane", "-t", pane, NULL);
            tmux(NULL, NULL, "resize-pane", "-Z", "-t", pane, NULL);
        }
        return;
    }
    def_prog_mode();
    endwin();
    tmux_takeover(d->c->p.session, pane);
    reset_prog_mode();
    clearok(stdscr, TRUE);
    flushinp();
    d->t_stats = d->t_git = d->t_panes = 0;
    if (a) {
        char msg[128];
        snprintf(msg, sizeof msg, "back from %s", jstr(a, "name"));
        set_status(d, msg);
    }
}

/* Runs one ':' command. Returns 1 to quit the UI. */
static int run_command(dash_t *d, char *line)
{
    char *save = NULL;
    char *cmd = strtok_r(line, " \t", &save);
    char *rest = save ? save + strspn(save, " \t") : NULL;
    if (!cmd) return 0;
    if (!strcmp(cmd, "q") || !strcmp(cmd, "quit") || !strcmp(cmd, "q!")) return 1;
    if (!strcmp(cmd, "help") || !strcmp(cmd, "h") || !strcmp(cmd, "?")) {
        d->help = 1;
        d->help_top = 0;
        return 0;
    }
    if (!d->stats) {
        set_status(d, "no grid is running here; start an agent with :spawn claude");
        if (strcmp(cmd, "spawn") && strcmp(cmd, "new")) return 0;
    }
    if (!strcmp(cmd, "takeover") || !strcmp(cmd, "t") || !strcmp(cmd, "to")) {
        const cJSON *a = find_agent(d, rest ? strtok_r(NULL, " \t", &save) : NULL);
        if (a)
            takeover(d, a);
        else
            set_status(d, "usage: :takeover N|name   (N is the agent's number in the list)");
    } else if (!strcmp(cmd, "grid")) {
        takeover(d, NULL);
    } else if (!strcmp(cmd, "send") || !strcmp(cmd, "s")) {
        char *who = strtok_r(NULL, " \t", &save);
        char *text = save ? save + strspn(save, " \t") : NULL;
        const cJSON *a = find_agent(d, who);
        if (!who || !text || !*text)
            set_status(d, "usage: :send N|name|all text");
        else
            send_to(d, a ? jstr(a, "name") : who, text);
    } else if (!strcmp(cmd, "kill")) {
        const cJSON *a = find_agent(d, rest ? strtok_r(NULL, " \t", &save) : NULL);
        if (!a) {
            set_status(d, "usage: :kill N|name");
        } else {
            cJSON *q = cli_req("kill");
            cJSON_AddStringToObject(q, "name", jstr(a, "name"));
            cJSON_Delete(cli_rpc(d->c, q, 2000));
            set_status(d, "stopped");
        }
    } else if (!strcmp(cmd, "spawn") || !strcmp(cmd, "new")) {
        if (!rest || !*rest) {
            set_status(d, "usage: :spawn agent [args]   e.g. :spawn codex --sandbox workspace-write");
            return 0;
        }
        char self[PATH_MAX];
        if (self_exe(self, sizeof self) < 0) return 0;
        av_t av = {0};
        av_add(&av, self);
        av_add(&av, "-C");
        av_add(&av, d->c->p.dir);
        av_add(&av, "-d");
        for (char *w = strtok_r(NULL, " \t", &save); w; w = strtok_r(NULL, " \t", &save)) av_add(&av, w);
        sb_t out = {0};
        int rc = run_cmd(av.v, NULL, 0, &out, RUN_MERGE_STDERR);
        av_free(&av);
        if (out.p) out.p[strcspn(out.p, "\n")] = '\0';
        set_status(d, out.p && *out.p ? out.p : rc == 0 ? "started" : "could not start it");
        sb_free(&out);
        d->t_stats = 0;
    } else if (!strcmp(cmd, "down!")) {
        cJSON_Delete(cli_rpc(d->c, cli_req("down"), 2000));
        set_status(d, "grid stopped");
    } else if (!strcmp(cmd, "down")) {
        set_status(d, ":down! stops every agent in this grid (with the !)");
    } else {
        char msg[160];
        snprintf(msg, sizeof msg, "unknown command :%s   (:help lists them)", cmd);
        set_status(d, msg);
    }
    return 0;
}

int cmd_dash(nq_ctx *c, int argc, char **argv)
{
    dash_t d;
    memset(&d, 0, sizeof d);
    d.c = c;
    d.sub = d.subfd = -1;
    if (argc > 0 && !strcmp(argv[0], "--once")) return print_once(&d);
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) die("the dashboard needs a terminal (try: nuq dash --once)");

    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(C_IDLE, COLOR_GREEN, -1);
        init_pair(C_BUSY, COLOR_YELLOW, -1);
        init_pair(C_BLOCKED, COLOR_RED, -1);
        init_pair(C_ACCENT, COLOR_CYAN, -1);
        init_pair(C_TOPIC, COLOR_MAGENTA, -1);
    }

    int quit = 0, dirty = 1;
    uint64_t last_draw = 0;
    while (!quit) {
        uint64_t now = now_ms();
        if (now - d.t_stats >= 1000) {
            fetch_stats(&d);
            dirty = 1;
        }
        if (d.stats && now - d.t_panes >= 2000) fetch_panes(&d);
        if (now - d.t_git >= 3000) fetch_git(&d);
        if (dirty || now - last_draw >= 1000) {
            draw(&d);
            last_draw = now;
            dirty = 0;
        }

        struct pollfd pfd[2] = {{STDIN_FILENO, POLLIN, 0}, {d.subfd, POLLIN, 0}};
        poll(pfd, d.subfd >= 0 ? 2 : 1, 250);
        if (drain_events(&d)) {
            /* Something happened: counts and states have moved too. */
            fetch_stats(&d);
            dirty = 1;
        }

        for (int ch; (ch = getch()) != ERR;) {
            dirty = 1;
            if (d.help) {
                switch (ch) {
                case KEY_UP: case 'k':
                    if (d.help_top > 0) d.help_top--;
                    break;
                case KEY_DOWN: case 'j':
                    if (d.help_top + 1 < (int)(sizeof help_lines / sizeof *help_lines)) d.help_top++;
                    break;
                case 27: case '?': case 'q': d.help = 0; break;
                }
                continue;
            }
            const cJSON *a = selected(&d);
            switch (ch) {
            case 'q':
            case 'Q': quit = 1; break;
            case KEY_UP:
            case 'k':
                if (d.sel > 0) d.sel--;
                break;
            case KEY_DOWN:
            case 'j':
                d.sel++;
                if (!selected(&d)) d.sel--;
                break;
            case ':': {
                char line[1024];
                prompt_line(":", line, sizeof line);
                if (run_command(&d, line)) quit = 1;
                break;
            }
            case '?': d.help = 1; d.help_top = 0; break;
            case 't':
            case '\n':
            case '\r':
            case KEY_ENTER:
                if (a) takeover(&d, a);
                break;
            case 's':
                if (a) {
                    char label[96], text[1024];
                    snprintf(label, sizeof label, " send to @%s: ", jstr(a, "name"));
                    prompt_line(label, text, sizeof text);
                    if (text[0]) send_to(&d, jstr(a, "name"), text);
                }
                break;
            case 'r':
                d.t_stats = d.t_git = d.t_panes = 0;
                break;
            default: break;
            }
        }
    }
    endwin();
    disconnect(&d);
    for (size_t i = 0; i < d.n_flow; i++) free(d.flow[i].text);
    cJSON_Delete(d.git);
    free(d.panes);
    return 0;
}
