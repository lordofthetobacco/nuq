#include "store.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

static const char *const STATE_NAMES[] = {"-", "starting", "idle", "busy", "blocked"};
static const char *const WSTATE_NAMES[] = {"", "queued", "claimed", "done", "failed"};
static const char *const MODE_NAMES[] = {"pull", "wake", "paste"};

const char *st_state_name(int state)
{
    return state >= 0 && state <= ST_BLOCKED ? STATE_NAMES[state] : "-";
}

int st_state_parse(const char *s)
{
    for (int i = ST_STARTING; s && i <= ST_BLOCKED; i++)
        if (!strcmp(s, STATE_NAMES[i])) return i;
    return -1;
}

const char *st_wstate_name(int wstate)
{
    return wstate >= 0 && wstate <= W_FAILED ? WSTATE_NAMES[wstate] : "";
}

const char *st_mode_name(int mode)
{
    return mode >= 0 && mode <= MODE_PASTE ? MODE_NAMES[mode] : "pull";
}

void st_init(nq_store *s)
{
    memset(s, 0, sizeof *s);
}

static void set_str(char **dst, const char *v)
{
    free(*dst);
    *dst = v ? xstrdup(v) : NULL;
}

static void free_subs(nq_agent *a)
{
    for (size_t i = 0; i < a->n_subs; i++) free(a->subs[i]);
    free(a->subs);
    a->subs = NULL;
    a->n_subs = 0;
}

void st_free(nq_store *s)
{
    for (size_t i = 0; i < s->n_msgs; i++) {
        nq_msg *m = s->msgs[i];
        free(m->from);
        free(m->kind);
        free(m->to);
        free(m->body);
        free(m->claimant);
        free(m->note);
        for (size_t j = 0; j < m->n_rcpt; j++) free(m->rcpt[j]);
        free(m->rcpt);
        free(m);
    }
    free(s->msgs);
    for (size_t i = 0; i < s->n_agents; i++) {
        nq_agent *a = s->agents[i];
        free(a->name);
        free(a->kind);
        free(a->pane);
        free(a->token);
        free(a->inbox);
        free_subs(a);
        for (size_t j = 0; j < a->n_files; j++) free(a->files[j].path);
        free(a->files);
        free(a->last_tool);
        free(a->inj_marker);
        free(a->inj_pre);
        free(a->inj_last);
        free(a);
    }
    free(s->agents);
    if (s->journal) fclose(s->journal);
    memset(s, 0, sizeof *s);
}

static nq_agent *find_any(const nq_store *s, const char *name)
{
    for (size_t i = 0; i < s->n_agents; i++)
        if (!strcmp(s->agents[i]->name, name)) return s->agents[i];
    return NULL;
}

nq_agent *st_find(const nq_store *s, const char *name)
{
    nq_agent *a = name ? find_any(s, name) : NULL;
    return a && a->live ? a : NULL;
}

nq_msg *st_msg(const nq_store *s, uint64_t id)
{
    return id >= 1 && id <= s->n_msgs ? s->msgs[id - 1] : NULL;
}

static void inbox_push(nq_agent *a, uint64_t id)
{
    if (a->n_inbox == a->cap_inbox) {
        a->cap_inbox = a->cap_inbox ? a->cap_inbox * 2 : 8;
        a->inbox = xrealloc(a->inbox, a->cap_inbox * sizeof *a->inbox);
    }
    a->inbox[a->n_inbox++] = id;
}

static int sub_index(const nq_agent *a, const char *topic)
{
    for (size_t i = 0; i < a->n_subs; i++)
        if (!strcmp(a->subs[i], topic)) return (int)i;
    return -1;
}

/* The single place state changes. Must stay deterministic: replay calls it
 * with the records exactly as they were journaled. */
static void apply(nq_store *s, const cJSON *r)
{
    const char *t = jstr(r, "t");
    const char *name = jstr(r, "name");
    uint64_t ts = (uint64_t)jnum(r, "ts", 0);
    if (!t) return;

    if (!strcmp(t, "msg")) {
        nq_msg *m = xcalloc(1, sizeof *m);
        const char *v;
        m->id = s->n_msgs + 1;
        m->ts = ts;
        m->reply_to = (uint64_t)jnum(r, "re", 0);
        m->from = xstrdup((v = jstr(r, "from")) ? v : "?");
        m->kind = xstrdup((v = jstr(r, "kind")) ? v : "dm");
        m->to = xstrdup((v = jstr(r, "to")) ? v : "");
        m->body = xstrdup((v = jstr(r, "body")) ? v : "");
        if (!strcmp(m->kind, "work")) m->wstate = W_QUEUED;
        const cJSON *rc = cJSON_GetObjectItemCaseSensitive(r, "rcpt"), *it;
        m->rcpt = xcalloc((size_t)cJSON_GetArraySize(rc) + 1, sizeof *m->rcpt);
        cJSON_ArrayForEach(it, rc)
        {
            if (!cJSON_IsString(it)) continue;
            m->rcpt[m->n_rcpt++] = xstrdup(it->valuestring);
            nq_agent *a = st_find(s, it->valuestring);
            if (a) inbox_push(a, m->id);
        }
        if (s->n_msgs == s->cap_msgs) {
            s->cap_msgs = s->cap_msgs ? s->cap_msgs * 2 : 64;
            s->msgs = xrealloc(s->msgs, s->cap_msgs * sizeof *s->msgs);
        }
        s->msgs[s->n_msgs++] = m;
        return;
    }

    if (!strcmp(t, "reg")) {
        if (!name) return;
        nq_agent *a = find_any(s, name);
        if (!a) {
            a = xcalloc(1, sizeof *a);
            a->name = xstrdup(name);
            if (s->n_agents == s->cap_agents) {
                s->cap_agents = s->cap_agents ? s->cap_agents * 2 : 8;
                s->agents = xrealloc(s->agents, s->cap_agents * sizeof *s->agents);
            }
            s->agents[s->n_agents++] = a;
        }
        const char *kind = jstr(r, "kind");
        set_str(&a->kind, kind ? kind : "cli");
        set_str(&a->pane, jstr(r, "pane"));
        set_str(&a->token, jstr(r, "token"));
        a->mode = (int)jnum(r, "mode", MODE_PULL);
        if (!a->live) {
            a->live = 1;
            a->state = a->mode == MODE_WAKE ? ST_STARTING : ST_UNKNOWN;
            a->state_ts = ts;
            a->reg_ts = ts;
        }
        a->label_dirty = 1;
        return;
    }

    if (!strcmp(t, "unreg")) {
        nq_agent *a = name ? st_find(s, name) : NULL;
        if (!a) return;
        a->live = 0;
        a->n_inbox = 0;
        a->state = ST_UNKNOWN;
        a->inj_phase = 0;
        set_str(&a->pane, NULL);
        set_str(&a->token, NULL);
        a->waiter_until = 0;
        free_subs(a);
        for (size_t i = 0; i < s->n_msgs; i++) {
            nq_msg *m = s->msgs[i];
            if (m->wstate == W_CLAIMED && m->claimant && !strcmp(m->claimant, name)) {
                m->wstate = W_QUEUED;
                set_str(&m->claimant, NULL);
            }
        }
        return;
    }

    if (!strcmp(t, "sub")) {
        nq_agent *a = name ? st_find(s, name) : NULL;
        const char *topic = jstr(r, "topic");
        if (!a || !topic) return;
        int idx = sub_index(a, topic);
        if (jbool(r, "on")) {
            if (idx >= 0) return;
            a->subs = xrealloc(a->subs, (a->n_subs + 1) * sizeof *a->subs);
            a->subs[a->n_subs++] = xstrdup(topic);
        } else if (idx >= 0) {
            free(a->subs[idx]);
            a->subs[idx] = a->subs[--a->n_subs];
        }
        a->label_dirty = 1;
        return;
    }

    if (!strcmp(t, "dlv")) {
        nq_agent *a = name ? st_find(s, name) : NULL;
        size_t n = (size_t)jnum(r, "n", 0);
        if (!a) return;
        if (n > a->n_inbox) n = a->n_inbox;
        memmove(a->inbox, a->inbox + n, (a->n_inbox - n) * sizeof *a->inbox);
        a->n_inbox -= n;
        return;
    }

    if (!strcmp(t, "edit")) {
        nq_agent *a = name ? find_any(s, name) : NULL;
        const char *file = jstr(r, "file");
        if (!a || !file) return;
        uint64_t added = (uint64_t)jnum(r, "added", 0), removed = (uint64_t)jnum(r, "removed", 0);
        a->added += added;
        a->removed += removed;
        size_t i = 0;
        while (i < a->n_files && strcmp(a->files[i].path, file)) i++;
        if (i == a->n_files) {
            a->files = xrealloc(a->files, (a->n_files + 1) * sizeof *a->files);
            a->files[i].path = xstrdup(file);
            a->files[i].added = a->files[i].removed = 0;
            a->n_files++;
        }
        a->files[i].added += added;
        a->files[i].removed += removed;
        return;
    }

    if (!strcmp(t, "claim")) {
        nq_msg *m = st_msg(s, (uint64_t)jnum(r, "id", 0));
        nq_agent *a = name ? st_find(s, name) : NULL;
        if (!m || !a || m->wstate != W_QUEUED) return;
        m->wstate = W_CLAIMED;
        set_str(&m->claimant, name);
        if (jbool(r, "inbox")) inbox_push(a, m->id);
        return;
    }

    if (!strcmp(t, "fin")) {
        nq_msg *m = st_msg(s, (uint64_t)jnum(r, "id", 0));
        if (!m || m->wstate != W_CLAIMED) return;
        m->wstate = jbool(r, "ok") ? W_DONE : W_FAILED;
        set_str(&m->note, jstr(r, "note"));
        return;
    }
}

static cJSON *record(const char *t)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "t", t);
    cJSON_AddNumberToObject(r, "ts", (double)now_ms());
    return r;
}

static void commit(nq_store *s, cJSON *r)
{
    apply(s, r);
    if (s->journal) {
        char *text = cJSON_PrintUnformatted(r);
        fputs(text, s->journal);
        fputc('\n', s->journal);
        fflush(s->journal);
        free(text);
    }
    cJSON_Delete(r);
}

#ifdef _WIN32
/* Binary: the journal is byte-exact JSON lines and is read back with a
 * seek from the end. N: the handle is not inherited by processes the broker
 * starts. */
#define JOURNAL_READ "rbN"
#define JOURNAL_APPEND "a+bN"
#else
#define JOURNAL_READ "r"
#define JOURNAL_APPEND "a+"
#endif

int st_replay(nq_store *s, const char *path)
{
    FILE *f = fopen(path, JOURNAL_READ);
    if (!f) return errno == ENOENT ? 0 : -1;
    char *line = NULL;
    size_t cap = 0;
    int count = 0;
    while (getline(&line, &cap, f) > 0) {
        /* A line cut short by a crash does not parse and is skipped. */
        cJSON *r = cJSON_Parse(line);
        if (!r) continue;
        apply(s, r);
        cJSON_Delete(r);
        count++;
    }
    free(line);
    fclose(f);
    return count;
}

int st_open_journal(nq_store *s, const char *path)
{
    FILE *f = fopen(path, JOURNAL_APPEND);
    if (!f) return -1;
    /* Start on a fresh line in case the last write was interrupted. */
    if (fseek(f, -1, SEEK_END) == 0) {
        int c = fgetc(f);
        if (c != '\n' && c != EOF) {
            fseek(f, 0, SEEK_END);
            fputc('\n', f);
        }
    }
    fseek(f, 0, SEEK_END);
    s->journal = f;
    return 0;
}

int st_token_ok(const nq_agent *a, const char *token)
{
    return !token || !a->token || !strcmp(a->token, token);
}

nq_agent *st_register(nq_store *s, const char *name, const char *kind, const char *pane,
                      const char *token, int mode, int update, char *err, size_t errlen)
{
    char autoname[48];
    if (!kind || !*kind) kind = "cli";
    if (!name) {
        /* "<kind>-<n>", with characters a name cannot hold replaced. */
        char base[24];
        size_t j = 0;
        for (const char *c = kind; *c && j < sizeof base - 1; c++) {
            char one[2] = {*c, '\0'};
            base[j++] = valid_name(one) ? *c : '_';
        }
        base[j] = '\0';
        if (base[0] == '-') base[0] = '_';
        for (int n = 1;; n++) {
            snprintf(autoname, sizeof autoname, "%s-%d", base, n);
            if (!st_find(s, autoname)) break;
        }
        name = autoname;
    }
    if (!valid_name(name) || !strcmp(name, "all")) {
        snprintf(err, errlen, "invalid name '%s' (use 1-32 of A-Z a-z 0-9 _ . -)", name);
        return NULL;
    }
    nq_agent *a = st_find(s, name);
    if (a && !update) {
        if (!strcmp(a->kind, "cli") && !strcmp(kind, "cli")) return a;
        snprintf(err, errlen, "name '%s' is already in use", name);
        return NULL;
    }
    cJSON *r = record("reg");
    cJSON_AddStringToObject(r, "name", name);
    cJSON_AddStringToObject(r, "kind", kind);
    if (pane) cJSON_AddStringToObject(r, "pane", pane);
    if (token) cJSON_AddStringToObject(r, "token", token);
    cJSON_AddNumberToObject(r, "mode", mode);
    char keep[48];
    snprintf(keep, sizeof keep, "%s", name);
    commit(s, r);
    return st_find(s, keep);
}

void st_unregister(nq_store *s, const char *name)
{
    if (!st_find(s, name)) return;
    cJSON *r = record("unreg");
    cJSON_AddStringToObject(r, "name", name);
    commit(s, r);
}

int st_sub(nq_store *s, const char *name, const char *topic, int on)
{
    if (!st_find(s, name) || !valid_name(topic)) return -1;
    cJSON *r = record("sub");
    cJSON_AddStringToObject(r, "name", name);
    cJSON_AddStringToObject(r, "topic", topic);
    cJSON_AddBoolToObject(r, "on", on);
    commit(s, r);
    return 0;
}

static int array_has(const cJSON *arr, const char *s)
{
    const cJSON *it;
    cJSON_ArrayForEach(it, arr)
    {
        if (cJSON_IsString(it) && !strcmp(it->valuestring, s)) return 1;
    }
    return 0;
}

static void known_names(const nq_store *s, sb_t *out)
{
    for (size_t i = 0; i < s->n_agents; i++) {
        if (!s->agents[i]->live) continue;
        if (out->len) sb_adds(out, ", ");
        sb_adds(out, s->agents[i]->name);
    }
    if (!out->len) sb_adds(out, "(none)");
}

uint64_t st_post(nq_store *s, const char *kind, const char *from, const char *to, const char *body,
                 uint64_t reply_to, char *err, size_t errlen)
{
    if (!valid_name(from)) {
        snprintf(err, errlen, "invalid sender name");
        return 0;
    }
    size_t blen = body ? strlen(body) : 0;
    if (blen == 0) {
        snprintf(err, errlen, "empty message");
        return 0;
    }
    if (blen > NQ_MAX_BODY) {
        snprintf(err, errlen, "message too large (%zu bytes, limit %d)", blen, NQ_MAX_BODY);
        return 0;
    }
    if (!to) to = "";
    if (!strcmp(kind, "dm") && (!strcmp(to, "all") || !strcmp(to, "@all"))) kind = "all";

    cJSON *rcpt = cJSON_CreateArray();
    sb_t norm = {0};
    if (!strcmp(kind, "dm")) {
        char *copy = xstrdup(to), *save = NULL;
        for (char *tok = strtok_r(copy, ", ", &save); tok; tok = strtok_r(NULL, ", ", &save)) {
            if (*tok == '@') tok++;
            if (!*tok) continue;
            if (!st_find(s, tok)) {
                sb_t known = {0};
                known_names(s, &known);
                snprintf(err, errlen, "no agent named '%s'; known: %s", tok, known.p);
                sb_free(&known);
                free(copy);
                sb_free(&norm);
                cJSON_Delete(rcpt);
                return 0;
            }
            if (array_has(rcpt, tok)) continue;
            cJSON_AddItemToArray(rcpt, cJSON_CreateString(tok));
            if (norm.len) sb_adds(&norm, ",");
            sb_adds(&norm, tok);
        }
        free(copy);
        if (cJSON_GetArraySize(rcpt) == 0) {
            snprintf(err, errlen, "no recipient given");
            sb_free(&norm);
            cJSON_Delete(rcpt);
            return 0;
        }
    } else if (!strcmp(kind, "all")) {
        sb_adds(&norm, "all");
        for (size_t i = 0; i < s->n_agents; i++) {
            nq_agent *a = s->agents[i];
            if (a->live && strcmp(a->name, from)) cJSON_AddItemToArray(rcpt, cJSON_CreateString(a->name));
        }
    } else if (!strcmp(kind, "topic")) {
        if (!valid_name(to)) {
            snprintf(err, errlen, "invalid topic name '%s'", to);
            cJSON_Delete(rcpt);
            return 0;
        }
        sb_adds(&norm, to);
        for (size_t i = 0; i < s->n_agents; i++) {
            nq_agent *a = s->agents[i];
            if (a->live && strcmp(a->name, from) && sub_index(a, to) >= 0)
                cJSON_AddItemToArray(rcpt, cJSON_CreateString(a->name));
        }
    } else if (!strcmp(kind, "work")) {
        sb_adds(&norm, "");
    } else {
        snprintf(err, errlen, "unknown message kind '%s'", kind);
        cJSON_Delete(rcpt);
        return 0;
    }

    cJSON *r = record("msg");
    cJSON_AddNumberToObject(r, "id", (double)(s->n_msgs + 1));
    cJSON_AddStringToObject(r, "from", from);
    cJSON_AddStringToObject(r, "kind", kind);
    cJSON_AddStringToObject(r, "to", norm.p ? norm.p : "");
    cJSON_AddStringToObject(r, "body", body);
    if (reply_to) cJSON_AddNumberToObject(r, "re", (double)reply_to);
    cJSON_AddItemToObject(r, "rcpt", rcpt);
    sb_free(&norm);
    commit(s, r);
    return s->n_msgs;
}

void st_edit(nq_store *s, const char *name, const char *file, uint64_t added, uint64_t removed)
{
    if (!st_find(s, name) || !file || !*file || (!added && !removed)) return;
    cJSON *r = record("edit");
    cJSON_AddStringToObject(r, "name", name);
    cJSON_AddStringToObject(r, "file", file);
    cJSON_AddNumberToObject(r, "added", (double)added);
    cJSON_AddNumberToObject(r, "removed", (double)removed);
    commit(s, r);
}

void st_take_inbox(nq_store *s, nq_agent *a, size_t n)
{
    if (n == 0) return;
    cJSON *r = record("dlv");
    cJSON_AddStringToObject(r, "name", a->name);
    cJSON_AddNumberToObject(r, "n", (double)n);
    commit(s, r);
}

nq_msg *st_claim(nq_store *s, nq_agent *a, int to_inbox)
{
    for (size_t i = 0; i < s->n_msgs; i++) {
        nq_msg *m = s->msgs[i];
        if (m->wstate != W_QUEUED) continue;
        cJSON *r = record("claim");
        cJSON_AddNumberToObject(r, "id", (double)m->id);
        cJSON_AddStringToObject(r, "name", a->name);
        cJSON_AddBoolToObject(r, "inbox", to_inbox);
        commit(s, r);
        return m;
    }
    return NULL;
}

nq_msg *st_claimed_by(const nq_store *s, const char *name)
{
    for (size_t i = 0; i < s->n_msgs; i++) {
        nq_msg *m = s->msgs[i];
        if (m->wstate == W_CLAIMED && m->claimant && !strcmp(m->claimant, name)) return m;
    }
    return NULL;
}

int st_finish(nq_store *s, const char *name, uint64_t id, int ok, const char *note, char *err,
              size_t errlen)
{
    nq_msg *m = st_msg(s, id);
    if (!m || m->wstate == W_NONE) {
        snprintf(err, errlen, "#%llu is not a work-queue task", (unsigned long long)id);
        return -1;
    }
    if (m->wstate != W_CLAIMED) {
        snprintf(err, errlen, "task #%llu is %s, not claimed", (unsigned long long)id,
                 st_wstate_name(m->wstate));
        return -1;
    }
    if (!m->claimant || strcmp(m->claimant, name)) {
        snprintf(err, errlen, "task #%llu is claimed by %s", (unsigned long long)id,
                 m->claimant ? m->claimant : "?");
        return -1;
    }
    cJSON *r = record("fin");
    cJSON_AddNumberToObject(r, "id", (double)id);
    cJSON_AddBoolToObject(r, "ok", ok);
    if (note && *note) cJSON_AddStringToObject(r, "note", note);
    commit(s, r);
    return 0;
}

size_t st_count_work(const nq_store *s, int wstate)
{
    size_t n = 0;
    for (size_t i = 0; i < s->n_msgs; i++)
        if (s->msgs[i]->wstate == wstate) n++;
    return n;
}

nq_agent *st_main_agent(const nq_store *s)
{
    nq_agent *best = NULL;
    for (size_t i = 0; i < s->n_agents; i++) {
        nq_agent *a = s->agents[i];
        if (!a->live || !a->pane) continue;
        if (!best || a->reg_ts < best->reg_ts) best = a;
    }
    return best;
}

nq_agent *st_idle_worker(const nq_store *s)
{
    nq_agent *best = NULL;
    for (size_t i = 0; i < s->n_agents; i++) {
        nq_agent *a = s->agents[i];
        if (!a->live || !a->pane || a->mode == MODE_PULL || a->state != ST_IDLE) continue;
        if (a->n_inbox || a->inj_phase || st_claimed_by(s, a->name)) continue;
        if (!best || a->state_ts < best->state_ts) best = a;
    }
    return best;
}

cJSON *st_msg_json(const nq_msg *m)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", (double)m->id);
    cJSON_AddNumberToObject(o, "ts", (double)m->ts);
    cJSON_AddStringToObject(o, "from", m->from);
    cJSON_AddStringToObject(o, "kind", m->kind);
    cJSON_AddStringToObject(o, "to", m->to);
    cJSON_AddStringToObject(o, "body", m->body);
    if (m->reply_to) cJSON_AddNumberToObject(o, "re", (double)m->reply_to);
    cJSON *rc = cJSON_AddArrayToObject(o, "rcpt");
    for (size_t i = 0; i < m->n_rcpt; i++) cJSON_AddItemToArray(rc, cJSON_CreateString(m->rcpt[i]));
    if (m->wstate != W_NONE) {
        cJSON_AddStringToObject(o, "wstate", st_wstate_name(m->wstate));
        if (m->claimant) cJSON_AddStringToObject(o, "claimant", m->claimant);
        if (m->note) cJSON_AddStringToObject(o, "note", m->note);
    }
    return o;
}
