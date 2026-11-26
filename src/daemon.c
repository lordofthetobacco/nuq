#include "daemon.h"

#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bus.h"
#include "inject.h"
#include "proto.h"
#include "store.h"
#include "tmux.h"
#include "util.h"

/* Most text pasted into a pane in one go. */
#define PASTE_MAX 8000
/* How long after its last check-in a waiting hook is still trusted to
 * deliver. Waiters check in at least every 5 seconds. */
#define WAITER_GRACE_MS 12000
/* How long a newly started agent with no state signal is left alone. */
#define STARTUP_GRACE_MS 5000

typedef struct {
    const nq_paths *p;
    nq_store st;
    int rep, pub;
    int stop;
    int kill_session;    /* `nuq nuke`: take the tmux session with us */
    int session_ended;
    int had_panes;       /* a pane agent has registered during this run */
    size_t pane_count;   /* panes seen at the last check, to know when to re-tile */
    char main_pane[32];  /* pane of the main agent at the last layout */
    uint64_t last_check;
    uint64_t wake_seq;
} daemon_t;

static volatile sig_atomic_t g_signal;

static void on_signal(int sig)
{
    g_signal = sig;
}

/* ---- replies and events ------------------------------------------------ */

static cJSON *reply_ok(void)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", 1);
    return r;
}

__attribute__((format(printf, 1, 2))) static cJSON *reply_err(const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", 0);
    cJSON_AddStringToObject(r, "error", msg);
    return r;
}

/* Publishes "<prefix><json>" and frees obj. Subscribers filter on prefix. */
static void publish(daemon_t *d, const char *prefix, cJSON *obj)
{
    char *js = cJSON_PrintUnformatted(obj);
    sb_t frame = {0};
    sb_adds(&frame, prefix);
    sb_adds(&frame, js);
    bus_send(d->pub, frame.p, frame.len, BUS_DONTWAIT);
    sb_free(&frame);
    free(js);
    cJSON_Delete(obj);
}

static void event(daemon_t *d, const char *type, const char *name, uint64_t id, const char *note)
{
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "t", type);
    cJSON_AddNumberToObject(e, "ts", (double)now_ms());
    if (name) cJSON_AddStringToObject(e, "name", name);
    if (id) cJSON_AddNumberToObject(e, "id", (double)id);
    if (note) cJSON_AddStringToObject(e, "note", note);
    publish(d, "ev ", e);
}

/* A hint for `nuq listen`: something new is in this mailbox. */
static void nudge(daemon_t *d, const char *name, uint64_t id)
{
    char prefix[64];
    snprintf(prefix, sizeof prefix, "to/%s ", name);
    publish(d, prefix, cJSON_CreateNumber((double)id));
}

static void announce(daemon_t *d, const nq_msg *m)
{
    publish(d, "msg ", st_msg_json(m));
    for (size_t i = 0; i < m->n_rcpt; i++) nudge(d, m->rcpt[i], m->id);
}

static void set_state(daemon_t *d, nq_agent *a, int state)
{
    if (a->state == state) return;
    a->state = state;
    a->state_ts = now_ms();
    a->label_dirty = 1;
    char prefix[64];
    snprintf(prefix, sizeof prefix, "st/%s ", a->name);
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "name", a->name);
    cJSON_AddStringToObject(e, "state", st_state_name(state));
    cJSON_AddNumberToObject(e, "ts", (double)a->state_ts);
    publish(d, prefix, e);
}

/* ---- request handling -------------------------------------------------- */

/* The caller's mailbox, created on first use for people and scripts. */
static nq_agent *mailbox(daemon_t *d, const char *name, char *err, size_t errlen)
{
    if (!name) {
        snprintf(err, errlen, "missing name");
        return NULL;
    }
    nq_agent *a = st_find(&d->st, name);
    if (a) return a;
    a = st_register(&d->st, name, "cli", NULL, NULL, MODE_PULL, 0, err, errlen);
    if (a) event(d, "join", a->name, 0, a->kind);
    return a;
}

/* Adds the first inbox entries to arr, stopping before max_bytes of body
 * text is exceeded (but always including one). Returns how many. */
static size_t peek_inbox(daemon_t *d, const nq_agent *a, cJSON *arr, size_t max_bytes)
{
    size_t n = 0, bytes = 0;
    for (; n < a->n_inbox; n++) {
        const nq_msg *m = st_msg(&d->st, a->inbox[n]);
        if (!m) continue;
        size_t len = strlen(m->body);
        if (n > 0 && max_bytes && bytes + len > max_bytes) break;
        bytes += len;
        cJSON_AddItemToArray(arr, st_msg_json(m));
    }
    return n;
}

static cJSON *do_post(daemon_t *d, const char *kind, const cJSON *q)
{
    char err[512] = "";
    const char *from = jstr(q, "from"), *body = jstr(q, "body");
    if (!mailbox(d, from, err, sizeof err)) return reply_err("%s", err);
    if (!body) return reply_err("missing body");
    char *clean = xstrdup(body);
    sanitize_text(clean);
    uint64_t id = st_post(&d->st, kind, from, jstr(q, "to"), clean, (uint64_t)jnum(q, "re", 0), err,
                          sizeof err);
    free(clean);
    if (!id) return reply_err("%s", err);
    const nq_msg *m = st_msg(&d->st, id);
    announce(d, m);
    cJSON *r = reply_ok();
    cJSON_AddNumberToObject(r, "id", (double)id);
    cJSON_AddStringToObject(r, "to", m->to);
    cJSON_AddNumberToObject(r, "rcpt", (double)m->n_rcpt);
    return r;
}

static cJSON *do_finish(daemon_t *d, const cJSON *q, int ok)
{
    char err[512] = "";
    const char *name = jstr(q, "name"), *note = jstr(q, "note");
    uint64_t id = (uint64_t)jnum(q, "id", 0);
    if (!name) return reply_err("missing name");
    char *clean = note ? xstrdup(note) : NULL;
    if (clean) sanitize_text(clean);
    if (st_finish(&d->st, name, id, ok, clean, err, sizeof err) < 0) {
        free(clean);
        return reply_err("%s", err);
    }
    event(d, ok ? "done" : "fail", name, id, clean);

    /* Tell whoever queued the task how it ended. */
    const nq_msg *m = st_msg(&d->st, id);
    if (m && strcmp(m->from, name) && st_find(&d->st, m->from)) {
        sb_t body = {0};
        sb_addf(&body, "task #%llu %s", (unsigned long long)id, ok ? "done" : "failed");
        if (clean && *clean) sb_addf(&body, ": %s", clean);
        uint64_t nid = st_post(&d->st, "dm", name, m->from, body.p, id, err, sizeof err);
        if (nid) announce(d, st_msg(&d->st, nid));
        sb_free(&body);
    }
    free(clean);
    return reply_ok();
}

static cJSON *do_ls(daemon_t *d);

/* realpath() for a path that may not exist (yet, or any more): resolves
 * the deepest existing ancestor and appends the rest unchanged. */
static void resolve_path(const char *path, char *out, size_t len)
{
    snprintf(out, len, "%s", path);
    if (path[0] != '/') return;
    char head[PATH_MAX], real[PATH_MAX];
    snprintf(head, sizeof head, "%s", path);
    for (;;) {
        if (realpath(head, real)) {
            snprintf(out, len, "%s%s", real, path + strlen(head));
            return;
        }
        char *slash = strrchr(head, '/');
        if (!slash || slash == head) return;
        *slash = '\0';
    }
}

/* What a hook reported the agent did: the tool it ran and the lines it
 * changed. Edits are journaled; the tool is only kept for the dashboard. */
static void record_activity(daemon_t *d, nq_agent *a, const cJSON *q)
{
    const char *tool = jstr(q, "tool");
    if (tool) {
        free(a->last_tool);
        a->last_tool = xstrdup(tool);
        a->last_ts = now_ms();
    }
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(q, "edits"))
    {
        const char *file = jstr(e, "file");
        double added = jnum(e, "added", 0), removed = jnum(e, "removed", 0);
        if (!file || added < 0 || removed < 0) continue;
        /* Paths inside the grid directory are shown relative to it, as git
         * shows them. Resolve symlinks first (/tmp is /private/tmp on
         * macOS); a deleted file is resolved through its directory. */
        char real[PATH_MAX];
        resolve_path(file, real, sizeof real);
        file = real;
        size_t dl = strlen(d->p->dir);
        if (!strncmp(file, d->p->dir, dl) && file[dl] == '/') file += dl + 1;
        st_edit(&d->st, a->name, file, (uint64_t)added, (uint64_t)removed);
        cJSON *ev = cJSON_CreateObject();
        cJSON_AddStringToObject(ev, "t", "edit");
        cJSON_AddNumberToObject(ev, "ts", (double)now_ms());
        cJSON_AddStringToObject(ev, "name", a->name);
        cJSON_AddStringToObject(ev, "note", file);
        cJSON_AddNumberToObject(ev, "added", added);
        cJSON_AddNumberToObject(ev, "removed", removed);
        publish(d, "ev ", ev);
    }
}

/* Everything the dashboard shows, in one reply. */
static cJSON *do_stats(daemon_t *d)
{
    cJSON *r = do_ls(d);
    cJSON_AddNumberToObject(r, "pid", (double)getpid());
    cJSON_AddNumberToObject(r, "now", (double)now_ms());
    const nq_agent *m = st_main_agent(&d->st);
    cJSON *agents = cJSON_GetObjectItemCaseSensitive(r, "agents"), *o;
    cJSON_ArrayForEach(o, agents)
    {
        const nq_agent *a = st_find(&d->st, jstr(o, "name"));
        if (!a) continue;
        double sent = 0, recv = 0, bout = 0, bin = 0, done = 0, failed = 0;
        for (size_t i = 0; i < d->st.n_msgs; i++) {
            const nq_msg *msg = d->st.msgs[i];
            double len = (double)strlen(msg->body);
            if (!strcmp(msg->from, a->name)) {
                sent++;
                bout += len;
            }
            for (size_t j = 0; j < msg->n_rcpt; j++)
                if (!strcmp(msg->rcpt[j], a->name)) {
                    recv++;
                    bin += len;
                }
            if (msg->claimant && !strcmp(msg->claimant, a->name)) {
                if (msg->wstate == W_DONE) done++;
                if (msg->wstate == W_FAILED) failed++;
            }
        }
        cJSON_AddBoolToObject(o, "main", a == m);
        cJSON_AddNumberToObject(o, "state_ts", (double)a->state_ts);
        cJSON_AddNumberToObject(o, "sent", sent);
        cJSON_AddNumberToObject(o, "recv", recv);
        cJSON_AddNumberToObject(o, "bytes_out", bout);
        cJSON_AddNumberToObject(o, "bytes_in", bin);
        cJSON_AddNumberToObject(o, "tasks_done", done);
        cJSON_AddNumberToObject(o, "tasks_failed", failed);
        cJSON_AddNumberToObject(o, "added", (double)a->added);
        cJSON_AddNumberToObject(o, "removed", (double)a->removed);
        cJSON_AddNumberToObject(o, "files", (double)a->n_files);
        if (a->last_tool) {
            cJSON_AddStringToObject(o, "last_tool", a->last_tool);
            cJSON_AddNumberToObject(o, "last_ts", (double)a->last_ts);
        }
    }

    /* Files changed by any agent (also ones that have left), busiest first. */
    cJSON *files = cJSON_AddArrayToObject(r, "files");
    for (size_t i = 0; i < d->st.n_agents; i++) {
        const nq_agent *a = d->st.agents[i];
        for (size_t j = 0; j < a->n_files; j++) {
            const nq_file *f = &a->files[j];
            cJSON *row = NULL, *it;
            cJSON_ArrayForEach(it, files)
            {
                if (!strcmp(jstr(it, "path"), f->path)) row = it;
            }
            if (!row) {
                row = cJSON_CreateObject();
                cJSON_AddStringToObject(row, "path", f->path);
                cJSON_AddNumberToObject(row, "added", 0);
                cJSON_AddNumberToObject(row, "removed", 0);
                cJSON_AddArrayToObject(row, "who");
                cJSON_AddItemToArray(files, row);
            }
            cJSON_ReplaceItemInObject(row, "added", cJSON_CreateNumber(jnum(row, "added", 0) + (double)f->added));
            cJSON_ReplaceItemInObject(row, "removed",
                                      cJSON_CreateNumber(jnum(row, "removed", 0) + (double)f->removed));
            cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(row, "who"), cJSON_CreateString(a->name));
        }
    }

    /* The latest messages, so a dashboard that just opened has history. */
    cJSON *recent = cJSON_AddArrayToObject(r, "recent");
    size_t from = d->st.n_msgs > 40 ? d->st.n_msgs - 40 : 0;
    for (size_t i = from; i < d->st.n_msgs; i++) cJSON_AddItemToArray(recent, st_msg_json(d->st.msgs[i]));
    return r;
}

static cJSON *do_ls(daemon_t *d)
{
    cJSON *r = reply_ok();
    cJSON *arr = cJSON_AddArrayToObject(r, "agents");
    for (size_t i = 0; i < d->st.n_agents; i++) {
        const nq_agent *a = d->st.agents[i];
        if (!a->live) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", a->name);
        cJSON_AddStringToObject(o, "kind", a->kind);
        cJSON_AddStringToObject(o, "state", st_state_name(a->state));
        cJSON_AddStringToObject(o, "mode", st_mode_name(a->mode));
        if (a->pane) cJSON_AddStringToObject(o, "pane", a->pane);
        cJSON_AddNumberToObject(o, "inbox", (double)a->n_inbox);
        cJSON *subs = cJSON_AddArrayToObject(o, "subs");
        for (size_t j = 0; j < a->n_subs; j++) cJSON_AddItemToArray(subs, cJSON_CreateString(a->subs[j]));
        const nq_msg *w = st_claimed_by(&d->st, a->name);
        if (w) cJSON_AddNumberToObject(o, "task", (double)w->id);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddNumberToObject(r, "queued", (double)st_count_work(&d->st, W_QUEUED));
    cJSON_AddNumberToObject(r, "claimed", (double)st_count_work(&d->st, W_CLAIMED));
    cJSON_AddStringToObject(r, "dir", d->p->dir);
    cJSON_AddStringToObject(r, "session", d->p->session);
    const nq_agent *m = st_main_agent(&d->st);
    if (m) {
        cJSON_AddStringToObject(r, "main", m->name);
        cJSON_AddStringToObject(r, "main_pane", m->pane);
    }
    return r;
}

static cJSON *handle(daemon_t *d, const cJSON *q)
{
    char err[512] = "";
    const char *op = jstr(q, "op");
    const char *name = jstr(q, "name");
    if (!op) return reply_err("missing op");

    if (!strcmp(op, "ping")) {
        cJSON *r = reply_ok();
        cJSON_AddNumberToObject(r, "pid", (double)getpid());
        cJSON_AddStringToObject(r, "version", NQ_VERSION);
        cJSON_AddStringToObject(r, "dir", d->p->dir);
        return r;
    }

    /* Launcher: reserve a name before the pane exists. */
    if (!strcmp(op, "alloc")) {
        nq_agent *a = st_register(&d->st, name, jstr(q, "kind"), NULL, NULL,
                                  (int)jnum(q, "mode", MODE_PULL), 0, err, sizeof err);
        if (!a) return reply_err("%s", err);
        cJSON *r = reply_ok();
        cJSON_AddStringToObject(r, "name", a->name);
        return r;
    }

    /* Pane wrapper: the agent is about to start in this pane. */
    if (!strcmp(op, "hello")) {
        if (!name) return reply_err("missing name");
        const char *pane = jstr(q, "pane");
        nq_agent *a = st_register(&d->st, name, jstr(q, "kind"), pane, jstr(q, "token"),
                                  (int)jnum(q, "mode", MODE_PULL), 1, err, sizeof err);
        if (!a) return reply_err("%s", err);
        const cJSON *subs = cJSON_GetObjectItemCaseSensitive(q, "subs"), *it;
        cJSON_ArrayForEach(it, subs)
        {
            if (cJSON_IsString(it)) st_sub(&d->st, a->name, it->valuestring, 1);
        }
        if (pane) d->had_panes = 1;
        event(d, "join", a->name, 0, a->kind);
        return reply_ok();
    }

    if (!strcmp(op, "bye")) {
        /* A wrapper from an earlier run may still be shutting down after
         * its name has been given to a new agent; its goodbye is not ours. */
        const nq_agent *a = st_find(&d->st, name);
        const char *token = jstr(q, "token");
        if (a && (!a->token || (token && !strcmp(a->token, token)))) {
            st_unregister(&d->st, name);
            event(d, "leave", name, 0, NULL);
        }
        return reply_ok();
    }

    if (!strcmp(op, "state")) {
        nq_agent *a = st_find(&d->st, name);
        int state = st_state_parse(jstr(q, "state"));
        if (!a) return reply_err("no agent named '%s'", name ? name : "");
        if (!st_token_ok(a, jstr(q, "token"))) return reply_err("stale agent instance");
        if (state < 0) return reply_err("unknown state");
        set_state(d, a, state);
        return reply_ok();
    }

    /* Agent hooks: report a state and collect waiting messages in one trip. */
    if (!strcmp(op, "hook")) {
        nq_agent *a = st_find(&d->st, name);
        if (!a) return reply_err("no agent named '%s'", name ? name : "");
        if (!st_token_ok(a, jstr(q, "token"))) return reply_err("stale agent instance");
        int state = st_state_parse(jstr(q, "state"));
        cJSON *r = reply_ok();
        cJSON *arr = cJSON_AddArrayToObject(r, "msgs");
        if (jbool(q, "drain")) {
            if (jbool(q, "claim") && a->n_inbox == 0 && !st_claimed_by(&d->st, a->name)) {
                const nq_msg *m = st_claim(&d->st, a, 1);
                if (m) event(d, "claim", a->name, m->id, NULL);
            }
            size_t n = peek_inbox(d, a, arr, (size_t)jnum(q, "max", 0));
            st_take_inbox(&d->st, a, n);
            /* Handing over messages at the end of a turn starts another one. */
            if (n > 0 && state == ST_IDLE) state = ST_BUSY;
        }
        if (state >= 0) set_state(d, a, state);
        /* A waiter is a background hook that will wake the agent itself, so
         * nothing needs typing into the pane while it keeps checking in. */
        if (jbool(q, "waiter")) a->waiter_until = now_ms() + WAITER_GRACE_MS;
        record_activity(d, a, q);
        cJSON_AddNumberToObject(r, "left", (double)a->n_inbox);
        cJSON_AddStringToObject(r, "state", st_state_name(a->state));
        return r;
    }

    if (!strcmp(op, "send")) return do_post(d, "dm", q);
    if (!strcmp(op, "pub")) return do_post(d, "topic", q);
    if (!strcmp(op, "push")) return do_post(d, "work", q);

    if (!strcmp(op, "sub") || !strcmp(op, "unsub")) {
        const char *topic = jstr(q, "topic");
        nq_agent *a = mailbox(d, name, err, sizeof err);
        if (!a) return reply_err("%s", err);
        if (!topic || !valid_name(topic)) return reply_err("invalid topic name");
        st_sub(&d->st, a->name, topic, !strcmp(op, "sub"));
        return reply_ok();
    }

    if (!strcmp(op, "inbox")) {
        nq_agent *a = mailbox(d, name, err, sizeof err);
        if (!a) return reply_err("%s", err);
        cJSON *r = reply_ok();
        cJSON *arr = cJSON_AddArrayToObject(r, "msgs");
        size_t n = peek_inbox(d, a, arr, (size_t)jnum(q, "max", 0));
        if (!jbool(q, "peek")) st_take_inbox(&d->st, a, n);
        cJSON_AddNumberToObject(r, "left", (double)(jbool(q, "peek") ? a->n_inbox - n : a->n_inbox));
        return r;
    }

    if (!strcmp(op, "take")) {
        nq_agent *a = mailbox(d, name, err, sizeof err);
        if (!a) return reply_err("%s", err);
        const nq_msg *cur = st_claimed_by(&d->st, a->name);
        if (cur)
            return reply_err("you still hold task #%llu; finish it with: nuq done %llu",
                             (unsigned long long)cur->id, (unsigned long long)cur->id);
        cJSON *r = reply_ok();
        const nq_msg *m = st_claim(&d->st, a, 0);
        if (m) {
            event(d, "claim", a->name, m->id, NULL);
            cJSON_AddItemToObject(r, "msg", st_msg_json(m));
        }
        return r;
    }

    if (!strcmp(op, "done")) return do_finish(d, q, 1);
    if (!strcmp(op, "fail")) return do_finish(d, q, 0);

    if (!strcmp(op, "show")) {
        const nq_msg *m = st_msg(&d->st, (uint64_t)jnum(q, "id", 0));
        if (!m) return reply_err("no message with that id");
        cJSON *r = reply_ok();
        cJSON_AddItemToObject(r, "msg", st_msg_json(m));
        return r;
    }

    if (!strcmp(op, "ls")) return do_ls(d);
    if (!strcmp(op, "stats")) return do_stats(d);

    if (!strcmp(op, "work")) {
        cJSON *r = reply_ok();
        cJSON *arr = cJSON_AddArrayToObject(r, "tasks");
        int all = jbool(q, "all");
        for (size_t i = 0; i < d->st.n_msgs; i++) {
            const nq_msg *m = d->st.msgs[i];
            if (m->wstate == W_NONE) continue;
            if (!all && m->wstate != W_QUEUED && m->wstate != W_CLAIMED) continue;
            cJSON_AddItemToArray(arr, st_msg_json(m));
        }
        return r;
    }

    if (!strcmp(op, "kill")) {
        nq_agent *a = st_find(&d->st, name);
        if (!a) return reply_err("no agent named '%s'", name ? name : "");
        if (a->pane) tmux(NULL, NULL, "kill-pane", "-t", a->pane, NULL);
        st_unregister(&d->st, name);
        event(d, "leave", name, 0, NULL);
        /* Killing the last pane ends the tmux session. That was asked for,
         * not a sign the grid is over: forget the panes so check_panes()
         * does not stop the broker and drop the queue with it. */
        int panes_left = 0;
        for (size_t i = 0; i < d->st.n_agents; i++)
            if (d->st.agents[i]->live && d->st.agents[i]->pane) panes_left = 1;
        if (!panes_left) d->had_panes = 0;
        return reply_ok();
    }

    if (!strcmp(op, "nuke")) {
        d->stop = 1;
        d->kill_session = 1;
        return reply_ok();
    }

    return reply_err("unknown op '%s'", op);
}

/* ---- delivery and housekeeping ----------------------------------------- */

static void maybe_wake(daemon_t *d, nq_agent *a, uint64_t now)
{
    if (!a->pane || a->mode == MODE_PULL || a->inj_phase || a->n_inbox == 0) return;
    if (now < a->inj_retry_at) return;
    /* Hook-driven agents are woken only when they are known to be idle.
     * Agents with no state signal are pasted to whenever mail arrives. */
    if (a->state != ST_IDLE && !(a->mode == MODE_PASTE && a->state == ST_UNKNOWN)) return;
    /* With no signal that the agent is ready, at least let it start: text
     * pasted before its interface is up lands in a plain terminal. */
    if (a->state == ST_UNKNOWN && now < a->reg_ts + STARTUP_GRACE_MS) return;
    if (now < a->waiter_until) return;

    sb_t text = {0};
    char marker[64];
    size_t count = 0;
    if (a->mode == MODE_WAKE) {
        /* The agent's prompt hook attaches the real messages. */
        snprintf(marker, sizeof marker, "[nuq #%llu]", (unsigned long long)++d->wake_seq);
        sb_addf(&text, "%s %zu new message%s for %s", marker, a->n_inbox, a->n_inbox == 1 ? "" : "s",
                a->name);
    } else {
        cJSON *arr = cJSON_CreateArray();
        count = peek_inbox(d, a, arr, PASTE_MAX);
        proto_fmt_msgs(&text, arr, PASTE_MAX);
        /* The agent may have had no other introduction to the queue. */
        sb_adds(&text, "\n(nuq message; to answer run: nuq send @NAME -- 'text')");
        snprintf(marker, sizeof marker, "<nuq id=\"%llu\"",
                 (unsigned long long)jnum(cJSON_GetArrayItem(arr, 0), "id", 0));
        cJSON_Delete(arr);
    }
    inject_start(a, text.p, marker, count, now);
    sb_free(&text);
}

static void deliver(daemon_t *d, uint64_t now)
{
    /* Hand queued tasks to agents that have nothing to do. */
    while (st_count_work(&d->st, W_QUEUED) > 0) {
        nq_agent *a = st_idle_worker(&d->st);
        if (!a) break;
        const nq_msg *m = st_claim(&d->st, a, 1);
        if (!m) break;
        event(d, "claim", a->name, m->id, NULL);
        nudge(d, a->name, m->id);
    }

    for (size_t i = 0; i < d->st.n_agents; i++) {
        nq_agent *a = d->st.agents[i];
        if (!a->live || !a->pane) continue;

        if (a->label_dirty) {
            sb_t label = {0};
            sb_addf(&label, "%s [%s] %s", a->name, a->kind, st_state_name(a->state));
            tmux_set_label(a->pane, label.p);
            sb_free(&label);
            a->label_dirty = 0;
        }

        if (!a->inj_phase) {
            maybe_wake(d, a, now);
            continue;
        }
        switch (inject_tick(a, now)) {
        case INJ_DONE:
            if (a->mode == MODE_PASTE) {
                st_take_inbox(&d->st, a, a->inj_count);
                if (a->state == ST_IDLE) set_state(d, a, ST_BUSY);
                a->inj_retry_at = now + 1000;
            } else {
                /* If the prompt hook never fires, try again later. */
                a->inj_retry_at = now + 5000;
            }
            break;
        case INJ_FAILED:
            nq_log("could not deliver to %s (pane %s): pasted text never appeared", a->name, a->pane);
            a->inj_retry_at = now + 10000;
            break;
        default:
            break;
        }
    }
}

static int in_flight(const daemon_t *d)
{
    for (size_t i = 0; i < d->st.n_agents; i++)
        if (d->st.agents[i]->live && d->st.agents[i]->inj_phase) return 1;
    return 0;
}

/* Reconciles the registry with the panes that really exist. */
static void check_panes(daemon_t *d, uint64_t now)
{
    int pane_agents = 0;
    for (size_t i = 0; i < d->st.n_agents; i++) {
        nq_agent *a = d->st.agents[i];
        if (!a->live) continue;
        if (a->pane) {
            pane_agents++;
        } else if (strcmp(a->kind, "cli") && now - a->reg_ts > 30000) {
            /* A name was reserved but its pane never started. */
            nq_log("%s never started; dropping it", a->name);
            st_unregister(&d->st, a->name);
            event(d, "leave", a->name, 0, NULL);
        }
    }
    if (!pane_agents && !d->had_panes) return;

    sb_t panes = {0};
    int alive = tmux_list_panes(d->p->session, &panes) == 0;
    size_t count = 0;
    for (const char *c = panes.p; c && *c; c++)
        if (*c == '\n') count++;

    for (size_t i = 0; i < d->st.n_agents; i++) {
        nq_agent *a = d->st.agents[i];
        if (!a->live || !a->pane) continue;
        int found = 0;
        if (alive && panes.p) {
            size_t plen = strlen(a->pane);
            for (const char *line = panes.p; *line; line += strcspn(line, "\n"), line += *line == '\n') {
                if (!strncmp(line, a->pane, plen) && (line[plen] == '\n' || line[plen] == '\0')) {
                    found = 1;
                    break;
                }
            }
        }
        if (!found) {
            nq_log("%s is gone (pane %s closed)", a->name, a->pane);
            char gone[48];
            snprintf(gone, sizeof gone, "%s", a->name);
            st_unregister(&d->st, gone);
            event(d, "leave", gone, 0, NULL);
        }
    }
    sb_free(&panes);

    if (!alive) {
        if (d->had_panes) {
            nq_log("tmux session %s ended; stopping", d->p->session);
            d->stop = 1;
            d->session_ended = 1;
        }
        return;
    }
    const nq_agent *m = st_main_agent(&d->st);
    const char *mp = m ? m->pane : "";
    if (count != d->pane_count || strcmp(mp, d->main_pane)) {
        /* While an agent is taken over (zoomed) the layout waits. */
        if (tmux_layout(d->p->session, m ? m->pane : NULL) < 0) return;
        d->pane_count = count;
        snprintf(d->main_pane, sizeof d->main_pane, "%s", mp);
    }
}

/* ---- lifecycle --------------------------------------------------------- */

int daemon_alive(const nq_paths *p)
{
    int fd = open(p->lockfile, O_RDONLY);
    if (fd < 0) return 0;
    int alive = flock(fd, LOCK_SH | LOCK_NB) < 0 && errno == EWOULDBLOCK;
    if (!alive) flock(fd, LOCK_UN);
    close(fd);
    return alive;
}

static int ping(const nq_paths *p, int timeout_ms)
{
    char *reply = bus_request(p->rpc_url, "{\"op\":\"ping\"}", timeout_ms);
    if (!reply) return 0;
    free(reply);
    return 1;
}

int daemon_ensure(const nq_paths *p, char *err, size_t errlen)
{
    if (daemon_alive(p)) return 0;
    if (paths_mkdirs(p, err, errlen) < 0) return -1;

    char self[PATH_MAX];
    if (self_exe(self, sizeof self) < 0) {
        snprintf(err, errlen, "cannot locate the nuq executable");
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, errlen, "fork: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        /* Detach: new session, then a second fork so the broker is not a
         * session leader and is re-parented to init. Only async-signal-safe
         * calls from here to exec. */
        setsid();
        if (fork() != 0) _exit(0);
        int null = open("/dev/null", O_RDONLY);
        int log = open(p->logfile, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (null >= 0) dup2(null, 0);
        if (log >= 0) {
            dup2(log, 1);
            dup2(log, 2);
        }
        for (int fd = 3; fd < 256; fd++) close(fd);
        execl(self, "nuq", "-C", p->dir, "daemon", (char *)NULL);
        _exit(127);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}

    for (int i = 0; i < 40; i++) {
        if (daemon_alive(p) && ping(p, 250)) return 0;
        sleep_ms(50);
    }
    snprintf(err, errlen, "the broker did not start; see %s", p->logfile);
    return -1;
}

int cmd_daemon(const nq_paths *p)
{
    char err[512];
    umask(077);
    if (paths_mkdirs(p, err, sizeof err) < 0) die("%s", err);

    int lockfd = open(p->lockfile, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lockfd < 0) die("%s: %s", p->lockfile, strerror(errno));
    int locked = 0;
    /* Clients probe the lock briefly, so a failure may be momentary. */
    for (int i = 0; i < 25 && !locked; i++) {
        if (flock(lockfd, LOCK_EX | LOCK_NB) == 0)
            locked = 1;
        else
            sleep_ms(20);
    }
    if (!locked) {
        nq_warn("a broker is already running for %s", p->dir);
        close(lockfd);
        return 0;
    }
    char pidtext[32];
    int pidlen = snprintf(pidtext, sizeof pidtext, "%d\n", (int)getpid());
    if (ftruncate(lockfd, 0) < 0 || write(lockfd, pidtext, (size_t)pidlen) < 0) {
        /* The pid in the lock file is only a convenience for people. */
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    daemon_t d;
    memset(&d, 0, sizeof d);
    d.p = p;
    d.wake_seq = (now_ms() / 1000) % 100000;
    st_init(&d.st);
    int replayed = st_replay(&d.st, p->journal);
    if (replayed < 0) die("cannot read %s: %s", p->journal, strerror(errno));
    if (st_open_journal(&d.st, p->journal) < 0) die("cannot open %s: %s", p->journal, strerror(errno));
    fcntl(fileno(d.st.journal), F_SETFD, FD_CLOEXEC);

    /* All NNG use starts here, after any forking is over. */
    d.rep = bus_rep_bind(p->rpc_url);
    if (d.rep < 0) die("bind %s: %s", p->rpc_url, bus_error());
    d.pub = bus_pub_bind(p->ev_url);
    if (d.pub < 0) die("bind %s: %s", p->ev_url, bus_error());
    int rcvfd = bus_rcvfd(d.rep);
    if (rcvfd < 0) die("NNG gave no receive descriptor: %s", bus_error());

    nq_log("broker %s up for %s (pid %d, %d journal records, %zu messages)", NQ_VERSION, p->dir,
           (int)getpid(), replayed, d.st.n_msgs);

    /* Agents still registered from a previous broker are checked against the
     * panes that exist now. If none survived, this is a fresh grid and the
     * "session ended" verdict from that check does not apply to it. */
    for (size_t i = 0; i < d.st.n_agents; i++)
        if (d.st.agents[i]->live && d.st.agents[i]->pane) d.had_panes = 1;
    check_panes(&d, now_ms());
    d.stop = d.session_ended = d.had_panes = 0;
    for (size_t i = 0; i < d.st.n_agents; i++)
        if (d.st.agents[i]->live && d.st.agents[i]->pane) d.had_panes = 1;

    while (!d.stop && !g_signal) {
        struct pollfd pfd = {.fd = rcvfd, .events = POLLIN};
        poll(&pfd, 1, in_flight(&d) ? 50 : 1000);

        for (;;) {
            char *req = bus_recv(d.rep, BUS_DONTWAIT, NULL);
            if (!req) break;
            cJSON *q = cJSON_Parse(req);
            cJSON *r = q ? handle(&d, q) : reply_err("request is not valid JSON");
            char *out = cJSON_PrintUnformatted(r);
            bus_send(d.rep, out, strlen(out), 0);
            free(out);
            cJSON_Delete(r);
            cJSON_Delete(q);
            free(req);
        }

        uint64_t now = now_ms();
        if (now - d.last_check >= 2000) {
            d.last_check = now;
            check_panes(&d, now);
        }
        deliver(&d, now);
    }

    /* On a signal the panes are left alone: the agents keep running and the
     * next broker picks them up again from the journal. */
    if (d.kill_session && tmux_has_session(p->session)) {
        char target[128];
        snprintf(target, sizeof target, "=%s", p->session);
        tmux(NULL, NULL, "kill-session", "-t", target, NULL);
        /* The dashboard's shell tab lives in its own session next to it. */
        snprintf(target, sizeof target, "=%s-sh", p->session);
        tmux(NULL, NULL, "kill-session", "-t", target, NULL);
    }
    if (d.kill_session || d.session_ended) {
        for (size_t i = 0; i < d.st.n_agents; i++) {
            nq_agent *a = d.st.agents[i];
            if (a->live && a->pane) st_unregister(&d.st, a->name);
        }
    }
    nq_log("broker stopping%s", g_signal ? " (signal)" : "");

    /* NNG has no linger: give the last reply a moment to leave. */
    sleep_ms(150);
    bus_close(d.rep);
    bus_close(d.pub);
    st_free(&d.st);
    close(lockfd);
    return 0;
}
