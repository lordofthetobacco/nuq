/* `nuq` with no arguments (or `nuq dash`): a live view of the grid.
 *
 *   header      directory, agents, work queue, git totals
 *   main        agent roster and selected agent terminal
 *   bottom      changed files: git's view next to what each agent edited
 *
 * `nuq dash --once` prints the same information once as plain text. */
#define _XOPEN_SOURCE 700  /* wcwidth */
#include <curses.h>
#include <limits.h>
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#ifdef _WIN32
#include <signal.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

#include "bus.h"
#include "cli.h"
#include "daemon.h"
#include "proto.h"
#include "tmux.h"
#include "util.h"

#define CHAT_REFRESH_MS 200
#define CHAT_MIN_HEIGHT 10
#define CHAT_MIN_WIDTH 32
#define FLASH_MS 1800
#define EDGE_MAX 128

enum { C_IDLE = 1, C_BUSY, C_BLOCKED, C_ACCENT, C_TOPIC, C_SEP, C_DYN };

/* Separator colour: xterm-256 19 (#0000af), plain blue on 8/16-colour terminals. */
#define SEP_COLOR_256 19

/* Attributes as passed to attrset(). PDCursesMod keeps the colour pair in
 * the upper half of a 64-bit chtype, which an int would cut off. */
#ifdef _WIN32
typedef chtype dattr;
#else
typedef int dattr;
#endif

typedef struct {
    char name[48];
    char line[256];
} pane_line;

typedef struct {
    char name[48];
    uint64_t until;
    int dir; /* 1: sending, 2: receiving */
} flash_t;

/* A captured tmux pane: its text with colour escapes, and its cursor. */
typedef struct {
    sb_t text;
    char pane[48];
    int cursor_x, cursor_y, cursor_visible;
} screen_t;

enum { TAB_FILES, TAB_TERM };

typedef struct {
    nq_ctx *c;
    cJSON *stats;      /* last `stats` reply, NULL when no broker answers */
    int sub, subfd;
    screen_t chat;     /* selected agent's pane */
    const char *own_pane;
    int cursor_screen_x, cursor_screen_y;
    uint64_t t_chat;
    /* Bottom panel: changed files, or a shell in the grid's directory that
     * runs in its own tmux session ("<grid>-sh"). */
    int tab, term_focus;
    screen_t term;
    char term_pane[48];
    int term_w, term_h;      /* size the shell pane was last given */
    int term_screen_x, term_screen_y;
    uint64_t t_term;
    int seeded_edges;
    cJSON *git;        /* [{path, added, removed}] */
    long git_add, git_del;
    int is_git;
    pane_line *panes;
    size_t n_panes;
    flash_t flash[16];
    struct {
        char from[48], to[48];
        uint64_t ts;
    } edges[EDGE_MAX];
    uint64_t animate_until;
    int sel, top;
    int help, help_top;
    char status[256];
    uint64_t status_until;
    uint64_t t_stats, t_git, t_panes;
} dash_t;

/* ---- data --------------------------------------------------------------- */

static const cJSON *selected(const dash_t *d);
static int pane_agents(const dash_t *d, const cJSON **out, int max);

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

/* Keep the most recent direction for each pair, including resolved topic and
 * broadcast recipients. Historical traffic establishes the resting links. */
static void record_edges(dash_t *d, const cJSON *msg)
{
    const char *from = jstr(msg, "from");
    uint64_t ts = (uint64_t)jnum(msg, "ts", 0);
    if (!from || !ts) return;
    const cJSON *r;
    cJSON_ArrayForEach(r, cJSON_GetObjectItemCaseSensitive(msg, "rcpt"))
    {
        if (!cJSON_IsString(r) || !strcmp(from, r->valuestring)) continue;
        const char *to = r->valuestring;
        size_t slot = 0;
        for (size_t i = 0; i < EDGE_MAX; i++) {
            if ((!strcmp(d->edges[i].from, from) && !strcmp(d->edges[i].to, to)) ||
                (!strcmp(d->edges[i].from, to) && !strcmp(d->edges[i].to, from))) {
                slot = i;
                break;
            }
            if (d->edges[i].ts < d->edges[slot].ts) slot = i;
        }
        snprintf(d->edges[slot].from, sizeof d->edges[slot].from, "%s", from);
        snprintf(d->edges[slot].to, sizeof d->edges[slot].to, "%s", to);
        d->edges[slot].ts = ts;
        if (ts + FLASH_MS > d->animate_until) d->animate_until = ts + FLASH_MS;
    }
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
    cJSON *previous = d->stats;
    const char *previous_pane = jstr(selected(d), "pane");
    d->stats = r;
    const cJSON *agents[64];
    int count = pane_agents(d, agents, 64);
    for (int i = 0; previous_pane && i < count; i++) {
        if (strcmp(previous_pane, jstr(agents[i], "pane"))) continue;
        d->sel = i;
        break;
    }
    if (d->sel >= count) d->sel = count ? count - 1 : 0;
    cJSON_Delete(previous);
    if (d->sub < 0) {
        d->sub = bus_sub_connect(d->c->p.ev_url);
        if (d->sub >= 0) {
            bus_sub_add(d->sub, "msg ");
            bus_sub_add(d->sub, "ev ");
            d->subfd = bus_rcvfd(d->sub);
        }
    }
    if (!d->seeded_edges) {
        const cJSON *m;
        cJSON_ArrayForEach(m, cJSON_GetObjectItemCaseSensitive(r, "recent"))
            record_edges(d, m);
        d->seeded_edges = 1;
    }
}

static int drain_events(dash_t *d)
{
    int got = 0;
    for (char *frame; d->sub >= 0 && (frame = bus_recv(d->sub, BUS_DONTWAIT, NULL));) {
        if (!strncmp(frame, "msg ", 4)) {
            cJSON *msg = cJSON_Parse(frame + 4);
            if (msg) {
                record_edges(d, msg);
                const char *from = jstr(msg, "from");
                if (from) mark_flash(d, from, 1);
                const cJSON *r;
                cJSON_ArrayForEach(r, cJSON_GetObjectItemCaseSensitive(msg, "rcpt"))
                    if (cJSON_IsString(r)) mark_flash(d, r->valuestring, 2);
                cJSON_Delete(msg);
            }
        }
        got = 1;
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
    const cJSON *recent = cJSON_GetObjectItemCaseSensitive(d->stats, "recent");
    int skip = cJSON_GetArraySize(recent) - 10;
    cJSON_ArrayForEach(a, recent)
    {
        if (skip-- > 0) continue;
        char clk[16];
        sb_t line = {0};
        fmt_clock(clk, sizeof clk, (uint64_t)jnum(a, "ts", 0));
        proto_fmt_line(&line, a, 200);
        printf("  %s %s\n", clk, line.p ? line.p : "");
        sb_free(&line);
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

#ifdef _WIN32
/* wchar_t is UTF-16 here, so mbrtowc cannot return characters outside the
 * BMP, and there is no wcwidth: decode UTF-8 and measure code points. */
static size_t utf8_decode(const char *s, size_t len, unsigned *cp)
{
    const unsigned char *u = (const unsigned char *)s;
    size_t n = u[0] < 0x80 ? 1 : (u[0] & 0xE0) == 0xC0 ? 2 : (u[0] & 0xF0) == 0xE0 ? 3
             : (u[0] & 0xF8) == 0xF0 ? 4 : 0;
    if (!n || n > len) return 0;
    unsigned c = n == 1 ? u[0] : u[0] & (0x7Fu >> n);
    for (size_t i = 1; i < n; i++) {
        if ((u[i] & 0xC0) != 0x80) return 0;
        c = c << 6 | (u[i] & 0x3Fu);
    }
    static const unsigned min[] = {0, 0, 0x80, 0x800, 0x10000};
    if (c < min[n] || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return 0;
    *cp = c;
    return n;
}

/* Terminal cells for a code point, like wcwidth(): -1 for control
 * characters, 0 for combining marks, 2 for East Asian wide and emoji. */
static int cp_width(unsigned c)
{
    static const unsigned zero[][2] = {
        {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x0610, 0x061A}, {0x064B, 0x065F},
        {0x0E31, 0x0E31}, {0x0E34, 0x0E3A}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x200B, 0x200F},
        {0x2028, 0x202E}, {0x2060, 0x2064}, {0x20D0, 0x20FF}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F},
        {0xFEFF, 0xFEFF}, {0xE0100, 0xE01EF},
    };
    static const unsigned wide[][2] = {
        {0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC}, {0x23F0, 0x23F3},
        {0x25FD, 0x25FE}, {0x2614, 0x2615}, {0x2648, 0x2653}, {0x267F, 0x267F}, {0x2693, 0x2693},
        {0x26A1, 0x26A1}, {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5}, {0x26CE, 0x26CE},
        {0x26D4, 0x26D4}, {0x26EA, 0x26EA}, {0x26F2, 0x26F5}, {0x26FA, 0x26FD}, {0x2705, 0x2705},
        {0x270A, 0x270B}, {0x2728, 0x2728}, {0x274C, 0x274C}, {0x2753, 0x2755}, {0x2757, 0x2757},
        {0x2795, 0x2797}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF}, {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50},
        {0x2B55, 0x2B55}, {0x2E80, 0x303E}, {0x3041, 0x33FF}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF},
        {0xA000, 0xA4CF}, {0xA960, 0xA97F}, {0xAC00, 0xD7A3}, {0xF900, 0xFAFF}, {0xFE10, 0xFE19},
        {0xFE30, 0xFE6F}, {0xFF00, 0xFF60}, {0xFFE0, 0xFFE6}, {0x16FE0, 0x18CFF}, {0x1B000, 0x1B2FF},
        {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A},
        {0x1F200, 0x1F251}, {0x1F300, 0x1F64F}, {0x1F680, 0x1F6FF}, {0x1F7E0, 0x1F7EB},
        {0x1F90C, 0x1F9FF}, {0x1FA70, 0x1FAFF}, {0x20000, 0x3FFFD},
    };
    if (c < 0x20 || (c >= 0x7F && c < 0xA0)) return -1;
    for (size_t i = 0; i < sizeof zero / sizeof *zero; i++)
        if (c >= zero[i][0] && c <= zero[i][1]) return 0;
    for (size_t i = 0; i < sizeof wide / sizeof *wide; i++)
        if (c >= wide[i][0] && c <= wide[i][1]) return 2;
    return 1;
}

/* Set while tmux owns the console during a takeover. */
static volatile sig_atomic_t in_takeover;

/* Ctrl-C and Ctrl-Break reach a console program as signals, and PDCurses,
 * unlike ncurses, installs no handler that restores the console first.
 * During a takeover the keys belong to tmux. */
static void on_console_break(int sig)
{
    if (in_takeover) {
        signal(sig, on_console_break);
        return;
    }
    endwin();
    _exit(130);
}
#endif

/* Clip by terminal cells, and keep control bytes inside the current row. */
static void put(int y, int x, int w, dattr attr, const char *s)
{
    if (w <= 0 || y < 0 || y >= LINES || x < 0 || x >= COLS) return;
    if (w > COLS - x) w = COLS - x;
    /* Leave the bottom-right cell alone so curses never tries to scroll. */
    if (y == LINES - 1 && w == COLS - x) w--;

#ifndef _WIN32
    mbstate_t state = {0};
#endif
    size_t len = strlen(s);
    const char *run = s;
    int cols = 0;
    attrset(attr);
    move(y, x);
    while (len) {
#ifdef _WIN32
        unsigned ch;
        size_t n = utf8_decode(s, len, &ch);
        if (!n) {
            ch = 0;
            n = 1;
        }
        int width = ch ? cp_width(ch) : -1;
#else
        wchar_t ch;
        size_t n = mbrtowc(&ch, s, len, &state);
        if (n == (size_t)-1 || n == (size_t)-2) {
            state = (mbstate_t){0};
            ch = L'\0';
            n = 1;
        }
        int width = ch ? wcwidth(ch) : -1;
#endif
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

NQ_PRINTF(5, 6) static void putf(int y, int x, int w, dattr attr, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    put(y, x, w, attr, buf);
}

static dattr state_attr(const char *state)
{
    if (!state) return A_DIM;
    if (!strcmp(state, "idle")) return COLOR_PAIR(C_IDLE);
    if (!strcmp(state, "busy")) return COLOR_PAIR(C_BUSY) | A_BOLD;
    if (!strcmp(state, "blocked")) return COLOR_PAIR(C_BLOCKED) | A_BOLD;
    return A_DIM;
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

/* ---- separators: heavy dark-blue rules ---------------------------------- */

static dattr sep_attr(void) { return has_colors() ? COLOR_PAIR(C_SEP) | A_BOLD : A_BOLD; }

/* Heavy box-drawing glyphs need a UTF-8 locale; otherwise fall back to ACS. */
static int heavy_ok(void) { return MB_CUR_MAX > 1; }

static void sep_glyph(int y, int x, const char *heavy, chtype acs)
{
    if (heavy_ok())
        put(y, x, 1, sep_attr(), heavy);
    else {
        attrset(sep_attr());
        mvaddch(y, x, acs);
        attrset(A_NORMAL);
    }
}

static void sep_hline(int y, int x, int w)
{
    if (w <= 0) return;
    if (!heavy_ok()) {
        attrset(sep_attr());
        mvhline(y, x, ACS_HLINE, w);
        attrset(A_NORMAL);
        return;
    }
    sb_t line = {0};
    for (int i = 0; i < w; i++) sb_adds(&line, "\xe2\x94\x81"); /* ━ */
    put(y, x, w, sep_attr(), line.p);
    sb_free(&line);
}

static void sep_vline(int y, int x, int h)
{
    for (int i = 0; i < h; i++) sep_glyph(y + i, x, "\xe2\x94\x83", ACS_VLINE); /* ┃ */
}

static void section(int y, int x, int w, const char *label)
{
    sep_hline(y, x, w);
    putf(y, x + 2, w - 4, A_BOLD, " %s ", label);
}

static uint64_t age(const cJSON *a, const char *key, uint64_t now)
{
    uint64_t ts = (uint64_t)jnum(a, key, (double)now);
    return ts && ts < now ? now - ts : 0;
}

static void draw_agent_detail(const dash_t *d, const cJSON *a, int y, int w, int rows, uint64_t now)
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
    if (rows > 1) putf(y + 1, 2, w - 3, A_BOLD, "%s%s%s", state_text, task_text,
         flash == 1 ? "   Sending" : flash == 2 ? "   Receiving" : "");

    fmt_dur(elapsed, sizeof elapsed, age(a, "last_ts", now));
    if (rows <= 2) return;
    if (tool)
        putf(y + 2, 2, w - 3, 0, "Tool %s ago   %s", elapsed, tool);
    else
        put(y + 2, 2, w - 3, A_DIM, "No tool activity reported");
    if (rows <= 3) return;
    putf(y + 3, 2, w - 3, 0, "Mail  %.0f in / %.0f out", jnum(a, "recv", 0), jnum(a, "sent", 0));
    putf(y + 4, 2, w - 3, 0, "Edits %+.0f -%.0f / %.0f files",
         jnum(a, "added", 0), jnum(a, "removed", 0), jnum(a, "files", 0));
    putf(y + 5, 2, w - 3, 0, "Tasks %.0f done / %.0f failed",
         jnum(a, "tasks_done", 0), jnum(a, "tasks_failed", 0));
    putf(y + 6, 2, w - 3, A_DIM, "> %s", *line ? line : "No pane output");
}

static void draw_agents_list(dash_t *d, const cJSON **agents, int n, int y, int h, int w, uint64_t now)
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
        dattr active = i == d->sel ? A_REVERSE : A_NORMAL;
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
    if (detail) draw_agent_detail(d, agents[d->sel], y + 1 + end - d->top, w, 7, now);
}

/* ---- agent graph: cards joined by routed, direction-animated links ------- */

enum { M_N = 1, M_E = 2, M_S = 4, M_W = 8 };

#define GW 3 /* gutter columns between cards */
#define GH 1 /* gutter rows between cards */
#define CARD_MIN_W 14
#define CARD_MAX_W 32
/* A terminal cell is about twice as tall as it is wide, so a tile that looks
 * square is twice as many columns as rows. Below this height tiles go back
 * to flat 3-4 row cards so many agents still fit. */
#define CARD_SQUARE_MIN_H 6

typedef struct {
    int cols, cw, ch, prow, rows, top, left;
} geom_t;

typedef struct {
    int x, y;
} pt_t;

typedef struct {
    unsigned char mask, port, glyph;
    chtype attr, gattr;
} graph_cell;

typedef struct {
    int w, h;
    graph_cell *cells;
} canvas_t;

static int card_x(const geom_t *g, int col) { return g->left + col * (g->cw + GW); }
static int card_y(const geom_t *g, int row) { return GH + row * (g->ch + GH); }

static void seg(pt_t *p, int *n, int cap, int tx, int ty)
{
    int dx = (tx > p[*n - 1].x) - (tx < p[*n - 1].x), dy = (ty > p[*n - 1].y) - (ty < p[*n - 1].y);
    while ((p[*n - 1].x != tx || p[*n - 1].y != ty) && *n < cap) {
        p[*n] = (pt_t){p[*n - 1].x + dx, p[*n - 1].y + dy};
        (*n)++;
    }
}

/* Path from card (ra,ca) to card (rb,cb), card border cell to border cell,
 * travelling only through the gutters between cards. Rows are page-relative. */
static int route(const geom_t *g, pt_t *p, int cap, int ra, int ca, int rb, int cb)
{
    int n = 0, xa = card_x(g, ca), xb = card_x(g, cb), ya = card_y(g, ra), yb = card_y(g, rb);
    if (ra == rb && (ca == cb + 1 || cb == ca + 1)) {
        int east = cb > ca;
        int yy = east ? ya + 1 : ya + g->ch - 2;
        p[n++] = (pt_t){east ? xa + g->cw - 1 : xa, yy};
        seg(p, &n, cap, east ? xb : xb + g->cw - 1, yy);
        return n;
    }
    int px = xa + g->cw / 3, qx = xb + g->cw * 2 / 3;
    int down = rb >= ra;
    int b1 = down ? ya + g->ch - 1 : ya, gy1 = down ? ya + g->ch : ya - 1;
    int below = rb <= ra;
    int b2 = below ? yb + g->ch - 1 : yb, gy2 = below ? yb + g->ch : yb - 1;
    p[n++] = (pt_t){px, b1};
    seg(p, &n, cap, px, gy1);
    if (gy1 != gy2) {
        int gx = cb >= ca ? xb - 2 : xb + g->cw + 1;
        seg(p, &n, cap, gx, gy1);
        seg(p, &n, cap, gx, gy2);
    }
    seg(p, &n, cap, qx, gy2);
    seg(p, &n, cap, qx, b2);
    return n;
}

static int dir_bit(pt_t a, pt_t b) { return b.x > a.x ? M_E : b.x < a.x ? M_W : b.y > a.y ? M_S : M_N; }
static int opp_bit(int m) { return m == M_E ? M_W : m == M_W ? M_E : m == M_N ? M_S : M_N; }

static void canvas_path(canvas_t *c, const pt_t *p, int n, chtype attr, int moving, int phase)
{
    if (n < 3) return;
    for (int i = 0; i < n; i++)
        if (p[i].x < 0 || p[i].y < 0 || p[i].x >= c->w || p[i].y >= c->h) return;
    for (int i = 0; i < n - 1; i++) {
        int m = dir_bit(p[i], p[i + 1]);
        size_t a = (size_t)p[i].y * c->w + p[i].x, b = (size_t)p[i + 1].y * c->w + p[i + 1].x;
        c->cells[a].mask |= m;
        c->cells[b].mask |= opp_bit(m);
        c->cells[a].attr = c->cells[b].attr = attr;
    }
    c->cells[(size_t)p[0].y * c->w + p[0].x].port = 1;
    c->cells[(size_t)p[n - 1].y * c->w + p[n - 1].x].port = 1;
    for (int i = 1; i < n - 1; i++) {
        int head = i == n - 2;
        if (!head && !(moving && ((i + 4 - phase) & 3) == 0)) continue;
        int m = dir_bit(p[i], p[i + 1]);
        size_t a = (size_t)p[i].y * c->w + p[i].x;
        c->cells[a].glyph = m == M_E ? '>' : m == M_W ? '<' : m == M_S ? 'v' : '^';
        c->cells[a].gattr = attr;
    }
}

static chtype line_char(int mask, int port)
{
    if (port) return mask == M_S ? ACS_TTEE : mask == M_N ? ACS_BTEE : mask == M_E ? ACS_LTEE : ACS_RTEE;
    switch (mask) {
    case M_N: case M_S: case M_N | M_S: return ACS_VLINE;
    case M_E: case M_W: case M_E | M_W: return ACS_HLINE;
    case M_N | M_E: return ACS_LLCORNER;
    case M_E | M_S: return ACS_ULCORNER;
    case M_S | M_W: return ACS_URCORNER;
    case M_N | M_W: return ACS_LRCORNER;
    case M_N | M_E | M_S: return ACS_LTEE;
    case M_E | M_S | M_W: return ACS_TTEE;
    case M_N | M_S | M_W: return ACS_RTEE;
    case M_N | M_E | M_W: return ACS_BTEE;
    default: return ACS_PLUS;
    }
}

static void draw_card(const geom_t *g, int y, int x, int idx, const cJSON *a, const char *last_line,
                      int selected, int flash, uint64_t now)
{
    const char *name = jstr(a, "name"), *state = jstr(a, "state"), *kind = jstr(a, "kind");
    int cw = g->cw, ch = g->ch;
    int big = ch >= CARD_SQUARE_MIN_H;
    /* Big tiles mark the selection on the name row only; reversing a whole
     * tile full of text is hard to read. */
    chtype fill = selected && !big ? A_REVERSE : A_NORMAL;
    chtype head = selected ? A_REVERSE : A_NORMAL;
    chtype border = state_attr(state) | (flash ? A_BOLD : 0) | (selected ? A_BOLD : 0);
    if (flash) border = COLOR_PAIR(C_ACCENT) | A_BOLD;
    attrset(fill);
    for (int r = 1; r < ch - 1; r++) mvhline(y + r, x + 1, ' ' | fill, cw - 2);
    attrset(border);
    mvhline(y, x + 1, ACS_HLINE | border, cw - 2);
    mvhline(y + ch - 1, x + 1, ACS_HLINE | border, cw - 2);
    mvvline(y + 1, x, ACS_VLINE | border, ch - 2);
    mvvline(y + 1, x + cw - 1, ACS_VLINE | border, ch - 2);
    mvaddch(y, x, ACS_ULCORNER | border);
    mvaddch(y, x + cw - 1, ACS_URCORNER | border);
    mvaddch(y + ch - 1, x, ACS_LLCORNER | border);
    mvaddch(y + ch - 1, x + cw - 1, ACS_LRCORNER | border);
    attrset(A_NORMAL);

    char dur[16] = "";
    if (jnum(a, "state_ts", 0) > 0) fmt_dur(dur, sizeof dur, age(a, "state_ts", now));
    const char *st = state ? state : "-";
    char tag[16];
    snprintf(tag, sizeof tag, "%c%d%c", selected ? '>' : ' ', idx + 1, jbool(a, "main") ? '*' : ' ');
    int tl = (int)strlen(tag);
    if (big) {
        int iw = cw - 4, r = y + 1, bottom = y + ch - 1;
        attrset(head);
        mvhline(r, x + 1, ' ' | head, cw - 2);
        attrset(A_NORMAL);
        putf(r++, x + 1, cw - 2, head | A_BOLD, "%s %s", tag, name ? name : "-");
        if (flash) put(y + 1, x + cw - 2, 1, head | A_BOLD, flash == 1 ? ">" : "<");
        putf(r++, x + 2, iw, state_attr(state), "%s %s", st, dur);
        double task = jnum(a, "task", 0);
        if (task)
            putf(r++, x + 2, iw, A_DIM, "%s  task #%.0f", kind ? kind : "-", task);
        else
            put(r++, x + 2, iw, A_DIM, kind ? kind : "-");
        /* One line kept for the pane's last line when there is room. */
        int stop = ch - 2 >= 8 ? bottom - 1 : bottom;
        const char *tool = jstr(a, "last_tool");
        char elapsed[16];
        fmt_dur(elapsed, sizeof elapsed, age(a, "last_ts", now));
        if (r < stop) {
            if (tool)
                putf(r++, x + 2, iw, 0, "Tool  %s %s", tool, elapsed);
            else
                put(r++, x + 2, iw, A_DIM, "Tool  -");
        }
        if (r < stop)
            putf(r++, x + 2, iw, 0, "Mail  %.0f in %.0f out", jnum(a, "recv", 0), jnum(a, "sent", 0));
        if (r < stop)
            putf(r++, x + 2, iw, 0, "Edits +%.0f -%.0f %.0ff", jnum(a, "added", 0),
                 jnum(a, "removed", 0), jnum(a, "files", 0));
        if (r < stop)
            putf(r++, x + 2, iw, 0, "Tasks %.0f ok %.0f fail", jnum(a, "tasks_done", 0),
                 jnum(a, "tasks_failed", 0));
        if (stop < bottom && last_line && *last_line) putf(bottom - 1, x + 2, iw, A_DIM, "> %s", last_line);
    } else if (ch >= 4) {
        putf(y + 1, x + 1, cw - 2, fill | A_BOLD, "%s %s", tag, name ? name : "-");
        putf(y + 2, x + 2, cw - 4, fill | state_attr(state), "%s %s", st, dur);
        if (flash) put(y + 2, x + cw - 2, 1, fill | A_BOLD, flash == 1 ? ">" : "<");
    } else {
        int sl = (int)strlen(st);
        if (sl > cw - 2 - tl - 3) sl = cw - 2 - tl - 3;
        if (sl < 0) sl = 0;
        putf(y + 1, x + 1, cw - 3 - sl, fill | A_BOLD, "%s %s", tag, name ? name : "-");
        putf(y + 1, x + cw - 1 - sl, sl, fill | state_attr(state), "%s", st);
    }
}

/* Largest tiles that look square and fit every agent without paging.
 * Returns 0 when they would be too small; the caller then pages flat cards. */
static int square_geom(geom_t *g, int n, int avail, int room)
{
    int best = 0;
    for (int cols = 1; cols <= n; cols++) {
        int rows = (n + cols - 1) / cols;
        int cw = (avail - (cols - 1) * GW) / cols;
        if (cw > CARD_MAX_W) cw = CARD_MAX_W;
        if (cw < CARD_MIN_W) break;
        int ch = (room - GH) / rows - GH;
        if (ch > cw / 2) ch = cw / 2;
        if (ch < CARD_SQUARE_MIN_H || ch <= g->ch) continue;
        g->cols = cols;
        g->rows = g->prow = rows;
        g->ch = ch;
        g->cw = cw < 2 * ch ? cw : 2 * ch;
        if (g->cw < CARD_MIN_W) g->cw = CARD_MIN_W;
        best = 1;
    }
    return best;
}

/* Cards on a grid, with one link per recorded edge. Returns 0 when the area is
 * too small, so the caller can fall back to the plain list. */
static int draw_agents_graph(dash_t *d, const cJSON **agents, int n, int y, int h, int w, int detail, uint64_t now)
{
    int avail = w - 2 * GW;
    if (n < 1 || avail < CARD_MIN_W) return 0;
    geom_t g = {0};
    int room = h - 1 - detail; /* rows below the title */
    if (!square_geom(&g, n, avail, room)) {
        g.ch = 4;
        g.prow = (room - GH) / (g.ch + GH);
        if (g.prow < 1) {
            g.ch = 3;
            g.prow = (room - GH) / (g.ch + GH);
        }
        if (g.prow < 1) return 0;
        g.cols = (avail + GW) / (CARD_MIN_W + GW);
        if (g.cols > n) g.cols = n;
        if (g.cols < 1) g.cols = 1;
        g.cw = (avail - (g.cols - 1) * GW) / g.cols;
        if (g.cw > CARD_MAX_W) g.cw = CARD_MAX_W;
    }
    g.left = GW + (avail - (g.cols * g.cw + (g.cols - 1) * GW)) / 2;
    g.rows = (n + g.cols - 1) / g.cols;
    if (g.prow > g.rows) g.prow = g.rows;

    int selrow = d->sel / g.cols;
    if (selrow < d->top) d->top = selrow;
    if (selrow >= d->top + g.prow) d->top = selrow - g.prow + 1;
    if (d->top > g.rows - g.prow) d->top = g.rows - g.prow;
    if (d->top < 0) d->top = 0;
    g.top = d->top;
    int first = g.top * g.cols, end = (g.top + g.prow) * g.cols;
    if (end > n) end = n;

    char title[64];
    if (end - first < n)
        snprintf(title, sizeof title, "Agents %d-%d/%d", first + 1, end, n);
    else
        snprintf(title, sizeof title, "Agents %d", n);
    section(y, 0, w, title);

    int used = GH + g.prow * (g.ch + GH);
    canvas_t c = {.w = w, .h = used};
    size_t cells = (size_t)c.w * c.h;
    c.cells = xcalloc(cells, sizeof *c.cells);
    int cap = 2 * (c.w + c.h) + 16;
    pt_t *path = xcalloc((size_t)cap, sizeof *path);

    int phase = (int)((now / 120) & 3);
    for (int pass = 0; pass < 2; pass++) /* resting links first, live ones on top */
        for (size_t k = 0; k < sizeof d->edges / sizeof *d->edges; k++) {
            if (!d->edges[k].from[0] || !d->edges[k].ts) continue;
            int live = d->edges[k].ts + FLASH_MS > now;
            if (live != pass) continue;
            int ia = -1, ib = -1;
            for (int i = first; i < end; i++) {
                const char *nm = jstr(agents[i], "name");
                if (!nm) continue;
                if (ia < 0 && !strcmp(nm, d->edges[k].from)) ia = i;
                if (ib < 0 && !strcmp(nm, d->edges[k].to)) ib = i;
            }
            if (ia < 0 || ib < 0 || ia == ib) continue;
            int len = route(&g, path, cap, ia / g.cols - g.top, ia % g.cols, ib / g.cols - g.top, ib % g.cols);
            static const int colors[] = {C_ACCENT, C_TOPIC, C_BUSY, C_IDLE};
            int color = colors[ia % 4];
            canvas_path(&c, path, len, COLOR_PAIR(color) | (live ? A_BOLD : A_DIM), live, phase);
        }

    for (int i = first; i < end; i++) {
        int col = i % g.cols, row = i / g.cols - g.top;
        const char *nm = jstr(agents[i], "name") ? jstr(agents[i], "name") : "";
        draw_card(&g, y + 1 + card_y(&g, row), card_x(&g, col), i, agents[i], pane_text(d, nm),
                  i == d->sel, flashing(d, nm), now);
    }
    for (int cy = 0; cy < c.h; cy++)
        for (int cx = 0; cx < c.w; cx++) {
            size_t k = (size_t)cy * c.w + cx;
            if (!c.cells[k].mask) continue;
            chtype ch = c.cells[k].glyph ? (chtype)c.cells[k].glyph | c.cells[k].gattr : line_char(c.cells[k].mask, c.cells[k].port) | c.cells[k].attr;
            attrset(A_NORMAL);
            mvaddch(y + 1 + cy, cx, ch);
        }
    free(c.cells);
    free(path);
    if (detail) draw_agent_detail(d, agents[d->sel], y + 1 + used, w, detail, now);
    return 1;
}

/* The tiles carry state, tool, mail, edits and tasks themselves, so they get
 * the whole area; the separate detail panel is gone. */
static void draw_agents(dash_t *d, const cJSON **agents, int n, int y, int h, int w, uint64_t now)
{
    if (draw_agents_graph(d, agents, n, y, h, w, 0, now)) return;
    draw_agents_list(d, agents, n, y, h, w, now);
}

/* ---- agent terminal colours ------------------------------------------- *
 * The preview is captured with `capture-pane -e`, so it carries the pane's
 * SGR escapes. tmux only emits the difference from the previous cell, so the
 * state runs on across lines and has to be tracked from the top. */

#define RGB_FLAG 0x1000000 /* colour value is 0xRRGGBB, not a palette index */

typedef struct {
    int fg, bg; /* -1: default */
    dattr attr;
} sgr_t;

static const sgr_t SGR_RESET = {-1, -1, 0};
static short dyn_pairs[256][2];
static int n_dyn = C_DYN;

static void palette_rgb(int idx, int *r, int *g, int *b)
{
    static const unsigned char base[16][3] = {
        {0, 0, 0},       {205, 0, 0},   {0, 205, 0},   {205, 205, 0},
        {0, 0, 238},     {205, 0, 205}, {0, 205, 205}, {229, 229, 229},
        {127, 127, 127}, {255, 0, 0},   {0, 255, 0},   {255, 255, 0},
        {92, 92, 255},   {255, 0, 255}, {0, 255, 255}, {255, 255, 255}};
    static const int level[6] = {0, 95, 135, 175, 215, 255};
    if (idx < 16) {
        *r = base[idx][0], *g = base[idx][1], *b = base[idx][2];
    } else if (idx < 232) {
        idx -= 16;
        *r = level[idx / 36], *g = level[idx / 6 % 6], *b = level[idx % 6];
    } else {
        *r = *g = *b = 8 + 10 * (idx - 232);
    }
}

static int nearest_index(int r, int g, int b, int lo, int hi)
{
    int best = lo;
    long best_d = LONG_MAX;
    for (int i = lo; i < hi; i++) {
        int pr, pg, pb;
        palette_rgb(i, &pr, &pg, &pb);
        long d = (long)(r - pr) * (r - pr) + (long)(g - pg) * (g - pg) + (long)(b - pb) * (b - pb);
        if (d < best_d) best_d = d, best = i;
    }
    return best;
}

/* A pane colour as the closest colour this terminal has. */
static short curses_color(int c)
{
    if (c < 0) return -1;
    int basic = COLORS >= 16 ? 16 : 8;
    int r, g, b;
    if (c & RGB_FLAG) {
        r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
        return (short)(COLORS >= 256 ? nearest_index(r, g, b, 16, 256) : nearest_index(r, g, b, 0, basic));
    }
    if (c < COLORS) return (short)c;
    palette_rgb(c, &r, &g, &b);
    return (short)nearest_index(r, g, b, 0, basic);
}

/* Colour pairs are made on demand. COLOR_PAIR() holds 8 bits, so at most
 * 255 pairs; past that, extra combinations fall back to the default. */
static dattr pair_attr(int fg, int bg)
{
    if (!has_colors() || (fg < 0 && bg < 0)) return 0;
    short f = curses_color(fg), b = curses_color(bg);
    for (int i = C_DYN; i < n_dyn; i++)
        if (dyn_pairs[i][0] == f && dyn_pairs[i][1] == b) return COLOR_PAIR(i);
    if (n_dyn >= 256 || n_dyn >= COLOR_PAIRS) return 0;
    if (init_pair((short)n_dyn, f, b) != OK) return 0;
    dyn_pairs[n_dyn][0] = f;
    dyn_pairs[n_dyn][1] = b;
    return COLOR_PAIR(n_dyn++);
}

static dattr sgr_attr(const sgr_t *st) { return st->attr | pair_attr(st->fg, st->bg); }

static void sgr_apply(sgr_t *st, const char *p, const char *end)
{
    /* colon[i]: v[i] is a ':' sub-parameter of the code before it. */
    int v[32], colon[32], n = 0, sep_colon = 0;
    while (n < 32) {
        int x = 0;
        while (p < end && *p >= '0' && *p <= '9') x = x * 10 + (*p++ - '0');
        colon[n] = sep_colon;
        v[n++] = x;
        if (p >= end) break;
        sep_colon = *p == ':';
        p++;
    }
    for (int i = 0; i < n; i++) {
        int c = v[i];
        /* Sub-parameters (4:3 curly underline and the like) are not codes. */
        int sub = i + 1 < n && colon[i + 1];
        switch (c) {
        case 0: *st = SGR_RESET; break;
        case 1: st->attr |= A_BOLD; break;
        case 2: st->attr |= A_DIM; break;
#ifdef A_ITALIC
        case 3: st->attr |= A_ITALIC; break;
        case 23: st->attr &= ~A_ITALIC; break;
#endif
        case 4: st->attr |= A_UNDERLINE; break;
        case 5: st->attr |= A_BLINK; break;
        case 7: st->attr |= A_REVERSE; break;
        case 22: st->attr &= ~(A_BOLD | A_DIM); break;
        case 24: st->attr &= ~A_UNDERLINE; break;
        case 25: st->attr &= ~A_BLINK; break;
        case 27: st->attr &= ~A_REVERSE; break;
        case 39: st->fg = -1; break;
        case 49: st->bg = -1; break;
        case 38: case 48: case 58: {
            int col = -1;
            if (i + 2 < n && v[i + 1] == 5) {
                col = v[i + 2] & 255;
                i += 2;
            } else if (i + 4 < n && v[i + 1] == 2) {
                /* 38:2:CS:R:G:B carries a colour-space id before the RGB. */
                int k = i + 1;
                while (k + 1 < n && colon[k + 1]) k++;
                int o = colon[i + 1] && k - i >= 5 && i + 5 < n ? 1 : 0;
                col = RGB_FLAG | (v[i + 2 + o] & 255) << 16 | (v[i + 3 + o] & 255) << 8 | (v[i + 4 + o] & 255);
                i += 4 + o;
            }
            if (c == 38) st->fg = col;
            else if (c == 48) st->bg = col;
            continue;
        }
        default:
            if (c >= 30 && c <= 37) st->fg = c - 30;
            else if (c >= 40 && c <= 47) st->bg = c - 40;
            else if (c >= 90 && c <= 97) st->fg = c - 90 + 8;
            else if (c >= 100 && c <= 107) st->bg = c - 100 + 8;
        }
        if (c == 4 && sub) {
            if (v[i + 1] == 0) st->attr &= ~A_UNDERLINE;
            i++;
        }
        while (i + 1 < n && colon[i + 1]) i++;
    }
}

/* s points at ESC. Applies an SGR sequence, skips any other, returns the
 * byte after it. */
static const char *ansi_escape(const char *s, sgr_t *st)
{
    if (s[1] == '[') {
        const char *p = s + 2, *end = p;
        while (*end && !(*end >= 0x40 && *end <= 0x7e)) end++;
        if (!*end) return end;
        if (*end == 'm') sgr_apply(st, p, end);
        return end + 1;
    }
    if (s[1] == ']') { /* OSC, e.g. hyperlinks: ends at BEL or ESC \ */
        for (const char *p = s + 2; *p; p++) {
            if (*p == '\a') return p + 1;
            if (*p == '\033' && p[1] == '\\') return p + 2;
        }
        return s + strlen(s);
    }
    return s[1] ? s + 2 : s + 1;
}

/* Cells a character takes, the way put() counts them. */
static int char_cells(const char *s, size_t len, size_t *used)
{
#ifdef _WIN32
    unsigned ch;
    size_t n = utf8_decode(s, len, &ch);
    if (n == 0) {
        *used = 1;
        return 1;
    }
    *used = n;
    int w = cp_width(ch);
#else
    mbstate_t state = {0};
    wchar_t ch;
    size_t n = mbrtowc(&ch, s, len, &state);
    if (n == (size_t)-1 || n == (size_t)-2 || n == 0) {
        *used = 1;
        return 1;
    }
    *used = n;
    int w = wcwidth(ch);
#endif
    return w < 0 ? 1 : w;
}

/* Draw one captured line, colours included, dropping its first `skip`
 * cells. With w <= 0 it only advances the colour state. */
static void put_ansi(int y, int x, int w, int skip, char *line, sgr_t *st)
{
    int col = 0;
    for (char *s = line; *s;) {
        if (*s == '\033') {
            s = (char *)ansi_escape(s, st);
            continue;
        }
        char *run = s;
        while (*s && *s != '\033') s++;
        if (w <= 0) continue;
        while (skip > 0 && run < s) {
            size_t used;
            skip -= char_cells(run, (size_t)(s - run), &used);
            run += used;
        }
        if (run == s || col >= w) continue;
        int cells = 0;
        for (const char *q = run; q < s;) {
            size_t used;
            cells += char_cells(q, (size_t)(s - q), &used);
            q += used;
        }
        char saved = *s;
        *s = '\0';
        put(y, x + col, w - col, sgr_attr(st), run);
        *s = saved;
        col += cells;
    }
    /* Fill the rest of the row with the running background, if any. */
    if (w > 0 && col < w && st->bg >= 0) {
        attrset(pair_attr(-1, st->bg));
        mvhline(y, x + col, ' ', w - col);
        attrset(A_NORMAL);
    }
}

/* The tmux pane stays in place. Capture its output and cursor together.
 * Returns 1 when anything on it changed. */
static int fetch_screen(screen_t *sc, const char *pane)
{
    if (!pane) {
        int changed = sc->pane[0] != '\0';
        sb_free(&sc->text);
        sc->pane[0] = '\0';
        return changed;
    }

    sb_t next = {0};
    int x = 0, y = 0, visible = 0;
    int rc = tmux(&next, NULL, "display-message", "-p", "-t", pane,
                  "#{cursor_x} #{cursor_y} #{cursor_flag}", ";",
                  "capture-pane", "-p", "-e", "-t", pane, NULL);
    char *body = next.p ? strchr(next.p, '\n') : NULL;
    if (rc || !body || sscanf(next.p, "%d %d %d", &x, &y, &visible) != 3) {
        sb_free(&next);
    } else {
        size_t header = (size_t)(body + 1 - next.p);
        memmove(next.p, next.p + header, next.len - header + 1);
        next.len -= header;
    }

    int changed = strcmp(sc->pane, pane) || !!sc->text.p != !!next.p;
    if (next.p) {
        changed |= !sc->text.p || strcmp(sc->text.p, next.p) ||
                   x != sc->cursor_x || y != sc->cursor_y || visible != sc->cursor_visible;
        sc->cursor_x = x;
        sc->cursor_y = y;
        sc->cursor_visible = visible;
    }
    snprintf(sc->pane, sizeof sc->pane, "%s", pane);
    sb_free(&sc->text);
    sc->text = next;
    return changed;
}

static int fetch_chat(dash_t *d)
{
    d->t_chat = now_ms();
    const char *pane = jstr(selected(d), "pane");
    if (pane && d->own_pane && !strcmp(pane, d->own_pane)) pane = NULL;
    return fetch_screen(&d->chat, pane);
}

/* Draws the tail of a captured screen into h rows at (y, x), w cells wide,
 * keeping the cursor row in view. *cy is set to the cursor's screen row
 * (-1 when it is not shown). */
static void draw_screen(screen_t *sc, int y, int h, int x, int w, int *cy, int *cx)
{
    *cy = -1;
    if (h < 1 || w < 1 || !sc->text.p) return;
    int last = 0, row = 0;
    for (const char *line = sc->text.p; *line; row++) {
        size_t len = strcspn(line, "\n");
        if (len) last = row;
        line += len + (line[len] == '\n');
    }
    if (sc->cursor_visible && last < sc->cursor_y) last = sc->cursor_y;
    int first = last - (h - 1);
    if (first > sc->cursor_y && sc->cursor_visible) first = sc->cursor_y;
    if (first < 0) first = 0;

    char *line = sc->text.p;
    sgr_t st = SGR_RESET;
    for (row = 0; *line && row < first + h; row++) {
        char *end = strchr(line, '\n');
        if (end) *end = '\0';
        if (row < first) {
            put_ansi(0, 0, 0, 0, line, &st); /* colour state only */
        } else {
            int cursor = sc->cursor_x, skip = 0;
            if (row == sc->cursor_y && sc->cursor_visible && cursor >= w) {
                /* Keep the typing position visible even if the pane is wider. */
                skip = cursor - w + 1;
                cursor -= skip;
            }
            int screen_y = y + row - first;
            put_ansi(screen_y, x, w, skip, line, &st);
            if (row == sc->cursor_y && sc->cursor_visible && cursor >= 0 && cursor < w) {
                *cy = screen_y;
                *cx = x + cursor;
            }
        }
        if (!end) break;
        *end = '\n';
        line = end + 1;
    }
}

static void draw_chat(dash_t *d, const cJSON *a, int y, int h, int x, int w)
{
    char title[80] = "Agent terminal";
    if (a) snprintf(title, sizeof title, "Agent terminal / %s", jstr(a, "name"));
    section(y, x, w, title);
    if (h < 2) return;
    if (!a || !d->chat.text.p || strcmp(d->chat.pane, jstr(a, "pane"))) {
        put(y + 1, x + 2, w - 3, A_DIM, a ? "Pane unavailable" : "No agent selected");
        return;
    }
    draw_screen(&d->chat, y + 1, h - 1, x + 2, w - 3, &d->cursor_screen_y, &d->cursor_screen_x);
}

/* ---- shell tab ----------------------------------------------------------- */

static void term_session(const dash_t *d, char *out, size_t len)
{
    snprintf(out, len, "%s-sh", d->c->p.session);
}

/* The shell pane, started on first use in the grid's directory. Its own
 * session keeps it out of the agents' layout. It survives takeovers and
 * ends when the dashboard quits (or with `nuq nuke`). */
static const char *term_pane(dash_t *d, int w, int h)
{
    if (w < 2 || h < 1) return NULL;
    char name[128], target[132];
    term_session(d, name, sizeof name);
    snprintf(target, sizeof target, "=%s", name);
    if (d->term_pane[0]) {
        sb_t out = {0};
        int rc = tmux(&out, NULL, "display-message", "-p", "-t", d->term_pane, "#{pane_dead}", NULL);
        int ok = rc == 0 && out.p && out.p[0] == '0';
        sb_free(&out);
        if (!ok) d->term_pane[0] = '\0';
    }
    if (!d->term_pane[0]) {
        char ws[16], hs[16];
        snprintf(ws, sizeof ws, "%d", w);
        snprintf(hs, sizeof hs, "%d", h);
        sb_t out = {0};
        if (tmux_has_session(name))
            tmux(&out, NULL, "display-message", "-p", "-t", target, "#{pane_id}", NULL);
        else
            tmux(&out, NULL, "new-session", "-d", "-s", name, "-c", d->c->p.dir, "-x", ws, "-y", hs,
                 "-P", "-F", "#{pane_id}", NULL);
        if (out.p && out.p[0] == '%') {
            out.p[strcspn(out.p, "\n")] = '\0';
            snprintf(d->term_pane, sizeof d->term_pane, "%s", out.p);
            /* No status line: the pane is exactly what the tab shows. */
            tmux(NULL, NULL, "set-option", "-t", target, "status", "off", NULL);
        }
        sb_free(&out);
        d->term_w = d->term_h = 0;
        if (!d->term_pane[0]) return NULL;
    }
    if (w != d->term_w || h != d->term_h) {
        char ws[16], hs[16];
        snprintf(ws, sizeof ws, "%d", w);
        snprintf(hs, sizeof hs, "%d", h);
        tmux(NULL, NULL, "resize-window", "-t", d->term_pane, "-x", ws, "-y", hs, NULL);
        d->term_w = w;
        d->term_h = h;
    }
    return d->term_pane;
}

static int fetch_term(dash_t *d)
{
    d->t_term = now_ms();
    if (d->tab != TAB_TERM || !d->term_w) return 0;
    return fetch_screen(&d->term, term_pane(d, d->term_w, d->term_h));
}

/* Forwards one key from the dashboard to the shell. Text is batched in
 * `lit` and sent by term_flush(); anything else goes as a tmux key name. */
static void term_flush(dash_t *d, sb_t *lit)
{
    if (lit->len && d->term_pane[0])
        tmux(NULL, NULL, "send-keys", "-l", "-t", d->term_pane, "--", lit->p, NULL);
    sb_free(lit);
}

static void term_key(dash_t *d, int ch, sb_t *lit)
{
    if (!d->term_pane[0]) return;
    char key[16] = "";
    switch (ch) {
    case '\n': case '\r': case KEY_ENTER: snprintf(key, sizeof key, "Enter"); break;
    case 127: case 8: case KEY_BACKSPACE: snprintf(key, sizeof key, "BSpace"); break;
    case '\t': snprintf(key, sizeof key, "Tab"); break;
    case KEY_BTAB: snprintf(key, sizeof key, "BTab"); break;
    case 27: snprintf(key, sizeof key, "Escape"); break;
    case KEY_UP: snprintf(key, sizeof key, "Up"); break;
    case KEY_DOWN: snprintf(key, sizeof key, "Down"); break;
    case KEY_LEFT: snprintf(key, sizeof key, "Left"); break;
    case KEY_RIGHT: snprintf(key, sizeof key, "Right"); break;
    case KEY_HOME: snprintf(key, sizeof key, "Home"); break;
    case KEY_END: snprintf(key, sizeof key, "End"); break;
    case KEY_DC: snprintf(key, sizeof key, "DC"); break;
    case KEY_IC: snprintf(key, sizeof key, "IC"); break;
    case KEY_PPAGE: snprintf(key, sizeof key, "PPage"); break;
    case KEY_NPAGE: snprintf(key, sizeof key, "NPage"); break;
    case 0: snprintf(key, sizeof key, "C-Space"); break;
    default:
        if (ch >= KEY_F(1) && ch <= KEY_F(24))
            snprintf(key, sizeof key, "F%d", ch - KEY_F(0));
        else if (ch >= 1 && ch <= 26)
            snprintf(key, sizeof key, "C-%c", 'a' + ch - 1);
        else if (ch >= 32 && ch < 256) {
            char c = (char)ch;
            sb_add(lit, &c, 1);
        }
    }
    if (key[0]) {
        term_flush(d, lit);
        tmux(NULL, NULL, "send-keys", "-t", d->term_pane, key, NULL);
    }
}

static void draw_tabs(const dash_t *d, int y, int w, size_t nfiles)
{
    sep_hline(y, 0, w);
    char files[48];
    snprintf(files, sizeof files, " Changed files %zu ", nfiles);
    const char *term = d->term_focus ? " Terminal (typing) " : " Terminal ";
    dattr on = A_BOLD | A_REVERSE, off = A_DIM;
    int x = 2;
    put(y, x, w - x - 1, d->tab == TAB_FILES ? on : off, files);
    x += (int)strlen(files) + 1;
    put(y, x, w - x - 1, d->tab == TAB_TERM ? on : off, term);
    x += (int)strlen(term) + 1;
    const char *hint = d->term_focus ? " F12 stop typing " : d->tab == TAB_TERM ? " i type   Tab files " : " Tab terminal ";
    int hl = (int)strlen(hint);
    if (w - hl - 2 > x) put(y, w - hl - 2, hl, A_DIM, hint);
}

static void draw_term(dash_t *d, int y, int h, int w)
{
    if (h < 2) return;
    int tw = w - 2, th = h - 1;
    if (tw != d->term_w || th != d->term_h) {
        term_pane(d, tw, th); /* resizes it; the next capture shows the result */
        d->t_term = 0;
    }
    if (!d->term.text.p) {
        put(y + 1, 2, w - 3, A_DIM, "Starting shell...");
        return;
    }
    draw_screen(&d->term, y + 1, th, 1, tw, &d->term_screen_y, &d->term_screen_x);
}

static void draw_files(const file_row *rows, size_t n, int y, int h, int w)
{
    char title[64];
    size_t shown = h > 2 ? (size_t)(h - 2) : 0;
    if (shown > n) shown = n;
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
    "Enter, t or F4         Take over selected agent (to type into it)",
    "s                      Send a message to the selected agent",
    ": or F2                Open command prompt",
    "r or F5                Refresh",
    "Tab                    Switch bottom panel: changed files / terminal",
    "i                      Type into the terminal (F12 or Ctrl-q stops)",
    "F12 or Ctrl-q          Return from takeover",
    "q or Ctrl-q            Leave dashboard",
    "",
    ":takeover N|name        Show an agent",
    ":send N|name|all text   Send a message",
    ":spawn agent [args]     Start an agent",
    ":kill N|name            Stop an agent",
    ":grid                  Show whole grid",
    ":nuke!                 Stop every agent",
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
    put(h - 1, 1, w - 2, A_BOLD, "Esc / F1  Close help    Up/Down  Scroll");
}

/* Bottom shortcut strip. Bordered (3 rows) when there is room, otherwise a
 * single plain row. Status messages take over the text row. Returns rows used. */
static int draw_command_bar(const dash_t *d, int h, int w, uint64_t now)
{
    static const char *const keys[] = {
        "Up/Down Select   Enter Take over   s Send   Tab Terminal   : Command   ? Help   q Quit",
        "Up/Down Select   Enter Take over   s Send   : Cmd   ? Help",
        "Up/Down   Enter Take over   : Cmd",
        ": Cmd   ? Help   q Quit",
    };
    int boxed = h >= 16 && w >= 24;
    int inner = boxed ? w - 4 : w - 2;
    int ti = 3;
    for (int i = 0; i < 4; i++)
        if ((int)strlen(keys[i]) <= inner) { ti = i; break; }
    int y = boxed ? h - 2 : h - 1;
    if (boxed) {
        attrset(A_DIM);
        mvaddch(h - 3, 0, ACS_ULCORNER);
        mvhline(h - 3, 1, ACS_HLINE, w - 2);
        mvaddch(h - 3, w - 1, ACS_URCORNER);
        mvaddch(y, 0, ACS_VLINE);
        mvaddch(y, w - 1, ACS_VLINE);
        mvaddch(h - 1, 0, ACS_LLCORNER);
        mvhline(h - 1, 1, ACS_HLINE, w - 2);
        mvinsch(h - 1, w - 1, ACS_LRCORNER);  /* no cursor advance */
        attrset(A_NORMAL);
    }
    int x = boxed ? 2 : 1;
    if (now < d->status_until)
        put(y, x, inner, COLOR_PAIR(C_BUSY) | A_BOLD, d->status);
    else {
        const char *keys_line = keys[ti];
        put(y, x, inner, A_NORMAL, keys_line);
        for (const char *key = keys_line; key && *key;) {
            size_t width = strcspn(key, " ");
            int offset = (int)(key - keys_line);
            if (offset >= inner) break;
            put(y, x + offset, (int)width, A_BOLD | COLOR_PAIR(C_ACCENT), key);
            key = strstr(key + width, "  ");
            if (key) key += strspn(key, " ");
        }
    }
    return boxed ? 3 : 1;
}

static void draw(dash_t *d)
{
    erase();
    d->cursor_screen_y = d->term_screen_y = -1;
    int h = LINES, w = COLS;
    uint64_t now = now_ms();
    const char *base = strrchr(d->c->p.dir, '/');
    put(0, 1, 5, COLOR_PAIR(C_ACCENT) | A_BOLD, "nuq");
    put(0, 8, w - 9, A_BOLD, base && base[1] ? base + 1 : d->c->p.dir);

    if (d->help) {
        draw_help(d, h, w);
    } else {
        int bar = draw_command_bar(d, h, w, now);

        if (!d->stats) {
            if (h >= 3) put(1, 1, w - 2, A_DIM, d->c->p.dir);
            if (h >= 6) {
                section(3, 0, w, "No grid running");
                put(4, 2, w - 3, 0, "Press F2 and enter spawn claude");
            }
        } else if (h < CHAT_MIN_HEIGHT || w < CHAT_MIN_WIDTH) {
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
            /* Bottom panel: tab row plus changed files or the shell. */
            int files = 0;
            if (h >= 16) {
                if (d->tab == TAB_TERM) {
                    files = h * 2 / 5;
                    if (files < 8) files = 8;
                } else {
                    files = (int)(nr > 6 ? 6 : nr) + 2;
                    if (files > h / 3) files = h / 3;
                }
                if (h - 3 - bar - files < 8) files = h - 3 - bar - 8;
                if (files < 2) files = 0;
            }
            int body = h - 3 - bar - files;
            int roster = wide ? body : body * 2 / 3;
            if (!wide && n && body >= 12 && roster < 9) roster = 9;
            if (!wide && roster > n + 8) roster = n + 8;
            if (!wide && roster > body - 2) roster = body - 2;
            int split = wide ? w * 46 / 100 : w;
            draw_agents(d, agents, n, 3, roster, split, now);
            if (wide) {
                sep_vline(3, split, body);
                sep_glyph(3, split, "\xe2\x94\xb3", ACS_TTEE); /* ┳ */
                draw_chat(d, n ? agents[d->sel] : NULL, 3, body, split + 1, w - split - 1);
            } else {
                draw_chat(d, n ? agents[d->sel] : NULL, 3 + roster, body - roster, 0, w);
            }
            if (files) {
                if (d->tab == TAB_TERM) {
                    draw_tabs(d, 3 + body, w, nr);
                    draw_term(d, 3 + body, files, w);
                } else {
                    draw_files(rows, nr, 3 + body, files, w);
                    draw_tabs(d, 3 + body, w, nr);
                    if (!nr && files > 1) put(4 + body, 2, w - 3, A_DIM, "No changes");
                }
                if (wide) sep_glyph(3 + body, split, "\xe2\x94\xbb", ACS_BTEE); /* ┻ */
            }
            free(rows);
        }
    }
    /* Show the agent's cursor as a highlighted cell, not the terminal cursor:
     * the tile is display only and must not look like it takes input. */
    if (d->cursor_screen_y >= 0 && !d->term_focus)
        mvchgat(d->cursor_screen_y, d->cursor_screen_x, 1, A_REVERSE, 0, NULL);
    if (d->term_screen_y >= 0 && d->tab == TAB_TERM)
        mvchgat(d->term_screen_y, d->term_screen_x, 1, d->term_focus ? A_REVERSE : A_DIM, 0, NULL);
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

/* Bracketed paste lets the dashboard drop pasted text instead of reading it
 * as shortcuts. It is off while a prompt line reads text. */
enum { KEY_PASTE_START = KEY_MAX + 1, KEY_PASTE_END };
static int paste_supported;

static void paste_mode(int enabled)
{
    if (!paste_supported) return;
    fputs(enabled ? "\033[?2004h" : "\033[?2004l", stdout);
    fflush(stdout);
}

/* Reads one line at the bottom of the screen. Empty on Escape. */
static void prompt_line(const char *label, char *buf, size_t len)
{
    buf[0] = '\0';
    paste_mode(0);
    move(LINES - 1, 0);
    clrtoeol();
    put(LINES - 1, 0, COLS, A_BOLD, label);
    echo();
    curs_set(1);
    nodelay(stdscr, FALSE);
#ifdef _WIN32
    /* Undo the main loop's timeout(100) as well: PDCurses' getnstr does not
     * expect its reads to time out and stores a junk character each time. */
    timeout(-1);
#endif
    getnstr(buf, (int)len - 1);
    nodelay(stdscr, TRUE);
    curs_set(0);
    noecho();
    paste_mode(1);
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
    paste_mode(0);
    def_prog_mode();
    endwin();
#ifdef _WIN32
    in_takeover = 1;
    tmux_takeover(d->c->p.session, pane);
    in_takeover = 0;
    reset_prog_mode();
    /* The console may have been resized while tmux had it. */
    resize_term(0, 0);
#else
    tmux_takeover(d->c->p.session, pane);
    reset_prog_mode();
#endif
    paste_mode(1);
    clearok(stdscr, TRUE);
    flushinp();
    d->t_stats = d->t_git = d->t_panes = d->t_chat = 0;
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
    } else if (!strcmp(cmd, "nuke!")) {
        cJSON_Delete(cli_rpc(d->c, cli_req("nuke"), 2000));
        set_status(d, "grid stopped");
    } else if (!strcmp(cmd, "nuke")) {
        set_status(d, ":nuke! stops every agent in this grid (with the !)");
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

#ifdef _WIN32
    /* "" would select the ANSI code page; all text here is UTF-8. */
    setlocale(LC_ALL, ".UTF-8");
#else
    setlocale(LC_ALL, "");
#endif
    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);
#ifndef _WIN32
    /* Esc on its own must reach the shell promptly, not after a second.
     * (PDCurses reads console key events and has no such delay.) */
    set_escdelay(25);
#endif
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(C_IDLE, COLOR_GREEN, -1);
        init_pair(C_BUSY, COLOR_YELLOW, -1);
        init_pair(C_BLOCKED, COLOR_RED, -1);
        init_pair(C_ACCENT, COLOR_CYAN, -1);
        init_pair(C_TOPIC, COLOR_MAGENTA, -1);
        init_pair(C_SEP, COLORS >= 256 ? SEP_COLOR_256 : COLOR_BLUE, -1);
    }
#ifdef _WIN32
    signal(SIGINT, on_console_break);
    signal(SIGBREAK, on_console_break);
#endif

    if (tmux_inside_session(c->p.session)) d.own_pane = getenv("TMUX_PANE");
#ifndef _WIN32
    /* PDCurses has no define_key; the Windows console pastes as key events. */
    paste_supported = define_key("\033[200~", KEY_PASTE_START) == OK &&
                      define_key("\033[201~", KEY_PASTE_END) == OK;
#endif
    paste_mode(1);

    int quit = 0, dirty = 1, pasting = 0;
    uint64_t last_draw = 0;
    while (!quit) {
        uint64_t now = now_ms();
        if (now - d.t_stats >= 1000) {
            fetch_stats(&d);
            dirty = 1;
        }
        if (d.stats && now - d.t_panes >= 2000) fetch_panes(&d);
        if (now - d.t_git >= 3000) fetch_git(&d);
        if (!d.help && now - d.t_chat >= CHAT_REFRESH_MS) dirty |= fetch_chat(&d);
        if (!d.help && d.tab == TAB_TERM && now - d.t_term >= (d.term_focus ? 60 : CHAT_REFRESH_MS))
            dirty |= fetch_term(&d);
        if (dirty || last_draw < d.animate_until || now - last_draw >= 1000) {
            draw(&d);
            last_draw = now;
            dirty = 0;
        }

#ifdef _WIN32
        /* Console input and the SUB socket cannot be waited on together:
         * wait briefly for a key, then drain the socket without blocking. */
        timeout(d.term_focus ? 60 : 100);
        int key = getch();
        nodelay(stdscr, TRUE);
        if (key != ERR) ungetch(key);
#else
        struct pollfd pfd[2] = {{STDIN_FILENO, POLLIN, 0}, {d.subfd, POLLIN, 0}};
        poll(pfd, d.subfd >= 0 ? 2 : 1, d.term_focus ? 60 : now < d.animate_until ? 90 : CHAT_REFRESH_MS);
#endif
        /* Refresh the roster before drawing again, so input in this frame
         * still targets the pane already on screen. */
        if (drain_events(&d)) d.t_stats = 0;

        /* The agent tile is display only: keys drive the dashboard and never
         * reach the agent. Typing into an agent needs a takeover. */
        sb_t lit = {0}; /* text typed into the shell this frame */
        for (int ch; !quit && (ch = getch()) != ERR;) {
            if (ch == KEY_PASTE_START || ch == KEY_PASTE_END) {
                pasting = ch == KEY_PASTE_START;
                continue;
            }
            if (d.term_focus) {
                /* Typing mode: every key goes to the shell except the two
                 * that leave it, the same ones that end a takeover. */
                dirty = 1;
                if (!pasting && (ch == KEY_F(12) || ch == 17)) {
                    d.term_focus = 0;
                } else if (pasting && (ch == '\n' || ch == '\r')) {
                    char nl = '\r';
                    sb_add(&lit, &nl, 1);
                } else {
                    term_key(&d, ch, &lit);
                }
                d.t_term = 0;
                continue;
            }
            if (pasting) continue;
            dirty = 1;
#ifdef _WIN32
            /* PDCurses reports a resized console but leaves the resize to us. */
            if (ch == KEY_RESIZE) {
                /* resize_term() flushes the console's input buffer, which
                 * would lose keys typed meanwhile; the ungetch stack survives. */
                int keep[64], n = 0;
                for (int k; n < 64 && (k = getch()) != ERR;)
                    if (k != KEY_RESIZE) keep[n++] = k;
                resize_term(0, 0);
                while (n > 0) ungetch(keep[--n]);
                continue;
            }
#endif
            if (d.help) {
                switch (ch) {
                case KEY_UP: case 'k':
                    if (d.help_top > 0) d.help_top--;
                    break;
                case KEY_DOWN: case 'j':
                    d.help_top++;
                    break;
                case 17: quit = 1; break; /* Ctrl-q */
                case 27: case '?': case 'q': case KEY_F(1): d.help = 0; break;
                }
                continue;
            }
            const cJSON *a = selected(&d);
            switch (ch) {
            case 'q': case 'Q': case 17: /* Ctrl-q */
                quit = 1;
                break;
            case KEY_UP: case 'k':
                if (d.sel > 0) d.sel--;
                d.t_chat = 0;
                break;
            case KEY_DOWN: case 'j':
                d.sel++;
                if (!selected(&d)) d.sel--;
                d.t_chat = 0;
                break;
            case '?': case KEY_F(1):
                d.help = 1;
                d.help_top = 0;
                break;
            case ':': case KEY_F(2): {
                char line[1024];
                prompt_line(":", line, sizeof line);
                if (run_command(&d, line)) quit = 1;
                break;
            }
            case 't': case '\n': case '\r': case KEY_ENTER: case KEY_F(4):
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
            case 'r': case KEY_F(5):
                d.t_stats = d.t_git = d.t_panes = d.t_chat = d.t_term = 0;
                break;
            case '\t':
                d.tab = d.tab == TAB_TERM ? TAB_FILES : TAB_TERM;
                d.t_term = 0;
                break;
            case 'i':
                d.tab = TAB_TERM;
                d.term_focus = 1;
                d.t_term = 0;
                break;
            default: break;
            }
        }
        term_flush(&d, &lit);
    }
    paste_mode(0);
    endwin();
    if (d.term_pane[0]) {
        char name[128], target[132];
        term_session(&d, name, sizeof name);
        snprintf(target, sizeof target, "=%s", name);
        tmux(NULL, NULL, "kill-session", "-t", target, NULL);
    }
    disconnect(&d);
    sb_free(&d.chat.text);
    sb_free(&d.term.text);
    cJSON_Delete(d.git);
    free(d.panes);
    return 0;
}
