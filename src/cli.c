#include "cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "bus.h"
#include "daemon.h"
#include "proto.h"
#include "tmux.h"
#include "util.h"

cJSON *cli_req(const char *op)
{
    cJSON *q = cJSON_CreateObject();
    cJSON_AddStringToObject(q, "op", op);
    return q;
}

cJSON *cli_rpc(const nq_ctx *c, cJSON *req, int timeout_ms)
{
    char *text = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    /* Checking the lock first makes "no broker" instant instead of a timeout. */
    char *reply = daemon_alive(&c->p) ? bus_request(c->p.rpc_url, text, timeout_ms) : NULL;
    free(text);
    if (!reply) return NULL;
    cJSON *r = cJSON_Parse(reply);
    free(reply);
    return r;
}

cJSON *cli_call(const nq_ctx *c, cJSON *req)
{
    if (!daemon_alive(&c->p)) {
        cJSON_Delete(req);
        die("no grid is running for %s\n  start one with: nuq <agent>   (or: nuq up)", c->p.dir);
    }
    cJSON *r = cli_rpc(c, req, 3000);
    if (!r)
        die("the broker did not answer\n  if this ran inside an agent's command sandbox, the sandbox is "
            "probably blocking local sockets\n  broker log: %s",
            c->p.logfile);
    if (!jbool(r, "ok")) {
        const char *e = jstr(r, "error");
        die("%s", e ? e : "request failed");
    }
    return r;
}

/* Joins argv[i..] with spaces; with no arguments left, reads stdin. */
static char *collect_body(int argc, char **argv, int i)
{
    sb_t sb = {0};
    if (i < argc) {
        for (; i < argc; i++) {
            if (sb.len) sb_adds(&sb, " ");
            sb_adds(&sb, argv[i]);
        }
        return sb.p;
    }
    if (isatty(STDIN_FILENO)) die("no message text (pass it as arguments or on stdin)");
#ifdef _WIN32
    /* Text mode would turn CRLF into LF and stop at a Ctrl-Z byte. */
    _setmode(STDIN_FILENO, _O_BINARY);
#endif
    size_t len = 0;
    char *body = read_fd_all(STDIN_FILENO, 2 * NQ_MAX_BODY, &len);
    if (!body) die("message too large (limit %d bytes)", NQ_MAX_BODY);
    while (len > 0 && (body[len - 1] == '\n' || body[len - 1] == '\r')) body[--len] = '\0';
#ifdef _WIN32
    /* PowerShell starts what it pipes to a program with a UTF-8 BOM when
     * $OutputEncoding is UTF-8. */
    if (len >= 3 && !memcmp(body, "\xEF\xBB\xBF", 3)) memmove(body, body + 3, len - 2);
#endif
    return body;
}

static void print_msgs(const cJSON *msgs)
{
    sb_t out = {0};
    proto_fmt_msgs(&out, msgs, 0);
    if (out.p) puts(out.p);
    sb_free(&out);
}

int cmd_up(nq_ctx *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    char err[512];
    if (daemon_ensure(&c->p, err, sizeof err) < 0) die("%s", err);
    printf("broker running for %s\n", c->p.dir);
    return 0;
}

int cmd_down(nq_ctx *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!daemon_alive(&c->p)) {
        printf("no grid is running for %s\n", c->p.dir);
        return 0;
    }
    cJSON_Delete(cli_call(c, cli_req("down")));
    for (int i = 0; i < 60 && daemon_alive(&c->p); i++) sleep_ms(50);
    printf("grid stopped for %s\n", c->p.dir);
    return 0;
}

int cmd_send(nq_ctx *c, int argc, char **argv)
{
    const char *to = NULL;
    unsigned long long re = 0;
    int i = 0;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        if (!strcmp(argv[i], "-r") && i + 1 < argc) {
            re = strtoull(argv[++i], NULL, 10);
            continue;
        }
        if (to) break;
        to = argv[i];
    }
    if (!to) die("usage: nuq send @name[,@name...]|@all [-r ID] [--] TEXT");
    char *body = collect_body(argc, argv, i);

    cJSON *q = cli_req("send");
    cJSON_AddStringToObject(q, "from", c->me);
    cJSON_AddStringToObject(q, "to", to);
    cJSON_AddStringToObject(q, "body", body);
    if (re) cJSON_AddNumberToObject(q, "re", (double)re);
    free(body);
    cJSON *r = cli_call(c, q);
    const char *dest = jstr(r, "to");
    printf("sent #%.0f to %s%s\n", jnum(r, "id", 0), dest ? dest : "?",
           jnum(r, "rcpt", 0) == 0 ? " (nobody else is in the grid)" : "");
    cJSON_Delete(r);
    return 0;
}

int cmd_pub(nq_ctx *c, int argc, char **argv)
{
    if (argc < 1) die("usage: nuq pub TOPIC [--] TEXT");
    int i = 1;
    if (i < argc && !strcmp(argv[i], "--")) i++;
    char *body = collect_body(argc, argv, i);
    cJSON *q = cli_req("pub");
    cJSON_AddStringToObject(q, "from", c->me);
    cJSON_AddStringToObject(q, "to", argv[0]);
    cJSON_AddStringToObject(q, "body", body);
    free(body);
    cJSON *r = cli_call(c, q);
    int n = (int)jnum(r, "rcpt", 0);
    printf("published #%.0f to topic %s (%d subscriber%s)\n", jnum(r, "id", 0), argv[0], n,
           n == 1 ? "" : "s");
    cJSON_Delete(r);
    return 0;
}

static int sub_cmd(nq_ctx *c, int argc, char **argv, const char *op)
{
    if (argc != 1) die("usage: nuq %s TOPIC", op);
    cJSON *q = cli_req(op);
    cJSON_AddStringToObject(q, "name", c->me);
    cJSON_AddStringToObject(q, "topic", argv[0]);
    cJSON_Delete(cli_call(c, q));
    printf("%s %s topic %s\n", c->me, !strcmp(op, "sub") ? "subscribed to" : "left", argv[0]);
    return 0;
}

int cmd_sub(nq_ctx *c, int argc, char **argv)
{
    return sub_cmd(c, argc, argv, "sub");
}

int cmd_unsub(nq_ctx *c, int argc, char **argv)
{
    return sub_cmd(c, argc, argv, "unsub");
}

int cmd_push(nq_ctx *c, int argc, char **argv)
{
    int i = 0;
    if (i < argc && !strcmp(argv[i], "--")) i++;
    char *body = collect_body(argc, argv, i);
    cJSON *q = cli_req("push");
    cJSON_AddStringToObject(q, "from", c->me);
    cJSON_AddStringToObject(q, "body", body);
    free(body);
    cJSON *r = cli_call(c, q);
    printf("queued task #%.0f\n", jnum(r, "id", 0));
    cJSON_Delete(r);
    return 0;
}

int cmd_take(nq_ctx *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    cJSON *q = cli_req("take");
    cJSON_AddStringToObject(q, "name", c->me);
    cJSON *r = cli_call(c, q);
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(r, "msg");
    int rc = 0;
    if (cJSON_IsObject(m)) {
        sb_t out = {0};
        proto_fmt_msg(&out, m, 0);
        puts(out.p);
        sb_free(&out);
    } else {
        puts("no tasks queued");
        rc = 1;
    }
    cJSON_Delete(r);
    return rc;
}

static int finish_cmd(nq_ctx *c, int argc, char **argv, const char *op)
{
    unsigned long long id = 0;
    sb_t note = {0};
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-m") && i + 1 < argc) {
            /* Everything after -m is the note, so it needs no quoting. */
            for (i++; i < argc; i++) {
                if (note.len) sb_adds(&note, " ");
                sb_adds(&note, argv[i]);
            }
        } else if (!id) {
            const char *s = argv[i][0] == '#' ? argv[i] + 1 : argv[i];
            id = strtoull(s, NULL, 10);
        }
    }
    if (!id) die("usage: nuq %s ID [-m NOTE]", op);
    cJSON *q = cli_req(op);
    cJSON_AddStringToObject(q, "name", c->me);
    cJSON_AddNumberToObject(q, "id", (double)id);
    if (note.p) cJSON_AddStringToObject(q, "note", note.p);
    sb_free(&note);
    cJSON_Delete(cli_call(c, q));
    printf("task #%llu marked %s\n", id, !strcmp(op, "done") ? "done" : "failed");
    return 0;
}

int cmd_done(nq_ctx *c, int argc, char **argv)
{
    return finish_cmd(c, argc, argv, "done");
}

int cmd_fail(nq_ctx *c, int argc, char **argv)
{
    return finish_cmd(c, argc, argv, "fail");
}

int cmd_inbox(nq_ctx *c, int argc, char **argv)
{
    cJSON *q = cli_req("inbox");
    cJSON_AddStringToObject(q, "name", c->me);
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], "--peek")) cJSON_AddBoolToObject(q, "peek", 1);
    cJSON *r = cli_call(c, q);
    const cJSON *msgs = cJSON_GetObjectItemCaseSensitive(r, "msgs");
    if (cJSON_GetArraySize(msgs) > 0)
        print_msgs(msgs);
    else
        puts("no new messages");
    cJSON_Delete(r);
    return 0;
}

int cmd_show(nq_ctx *c, int argc, char **argv)
{
    if (argc != 1) die("usage: nuq show ID");
    const char *s = argv[0][0] == '#' ? argv[0] + 1 : argv[0];
    cJSON *q = cli_req("show");
    cJSON_AddNumberToObject(q, "id", (double)strtoull(s, NULL, 10));
    cJSON *r = cli_call(c, q);
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(r, "msg");
    sb_t out = {0};
    proto_fmt_msg(&out, m, 0);
    puts(out.p);
    const char *ws = jstr(m, "wstate");
    if (ws) {
        const char *who = jstr(m, "claimant"), *note = jstr(m, "note");
        printf("status: %s%s%s%s%s\n", ws, who ? " by " : "", who ? who : "", note ? ": " : "",
               note ? note : "");
    }
    sb_free(&out);
    cJSON_Delete(r);
    return 0;
}

int cmd_listen(nq_ctx *c, int argc, char **argv)
{
    long timeout_s = -1;
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], "-t") && i + 1 < argc) timeout_s = strtol(argv[++i], NULL, 10);

    /* Make sure the mailbox exists, then subscribe before looking in it, so
     * a message arriving in between still produces a wake-up. */
    cJSON *q = cli_req("inbox");
    cJSON_AddStringToObject(q, "name", c->me);
    cJSON_AddBoolToObject(q, "peek", 1);
    cJSON_Delete(cli_call(c, q));

    int sub = bus_sub_connect(c->p.ev_url);
    if (sub < 0) die("cannot subscribe: %s", bus_error());
    char prefix[64];
    snprintf(prefix, sizeof prefix, "to/%s ", c->me);
    bus_sub_add(sub, prefix);

    uint64_t deadline = timeout_s >= 0 ? now_ms() + (uint64_t)timeout_s * 1000 : 0;
    int rc = 1;
    for (;;) {
        q = cli_req("inbox");
        cJSON_AddStringToObject(q, "name", c->me);
        cJSON *r = cli_rpc(c, q, 3000);
        if (!r) {
            nq_warn("the grid has stopped");
            break;
        }
        const cJSON *msgs = cJSON_GetObjectItemCaseSensitive(r, "msgs");
        if (cJSON_GetArraySize(msgs) > 0) {
            print_msgs(msgs);
            cJSON_Delete(r);
            rc = 0;
            break;
        }
        cJSON_Delete(r);

        uint64_t now = now_ms();
        if (deadline && now >= deadline) break;
        /* Events are only hints, so look again every few seconds regardless. */
        int wait = 5000;
        if (deadline && deadline - now < (uint64_t)wait) wait = (int)(deadline - now);
        bus_set_rcvtimeo(sub, wait);
        free(bus_recv(sub, 0, NULL));
    }
    bus_close(sub);
    return rc;
}

int cmd_ls(nq_ctx *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    cJSON *r = cli_call(c, cli_req("ls"));
    const cJSON *agents = cJSON_GetObjectItemCaseSensitive(r, "agents"), *a;
    /* One word per column, so the output is easy to cut with awk. */
    printf("%-16s %-14s %-9s %-5s %-5s %-5s %s\n", "NAME", "AGENT", "STATE", "TASK", "INBOX", "PANE",
           "TOPICS");
    cJSON_ArrayForEach(a, agents)
    {
        sb_t topics = {0};
        const cJSON *subs = cJSON_GetObjectItemCaseSensitive(a, "subs"), *s;
        cJSON_ArrayForEach(s, subs)
        {
            if (topics.len) sb_adds(&topics, ",");
            sb_adds(&topics, s->valuestring);
        }
        const char *name = jstr(a, "name"), *kind = jstr(a, "kind"), *state = jstr(a, "state");
        const char *pane = jstr(a, "pane");
        int task = (int)jnum(a, "task", 0);
        char tk[16] = "-";
        if (task) snprintf(tk, sizeof tk, "#%d", task);
        printf("%-16s %-14s %-9s %-5s %-5.0f %-5s %s\n", name ? name : "?", kind ? kind : "?",
               state ? state : "-", tk, jnum(a, "inbox", 0), pane ? pane : "-",
               topics.p ? topics.p : "");
        sb_free(&topics);
    }
    printf("work queue: %.0f queued, %.0f in progress\n", jnum(r, "queued", 0), jnum(r, "claimed", 0));
    cJSON_Delete(r);
    return 0;
}

static void print_time(uint64_t ts)
{
    time_t t = (time_t)(ts / 1000);
    struct tm tm;
    localtime_r(&t, &tm);
    printf("%02d:%02d:%02d ", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

cJSON *cli_fmt_event(const char *frame, sb_t *out, int *kind)
{
    const char *json = strchr(frame, ' ');
    cJSON *o = json ? cJSON_Parse(json + 1) : NULL;
    if (!o) return NULL;
    const char *name = jstr(o, "name");
    if (!name) name = "?";
    if (!strncmp(frame, "msg ", 4)) {
        const char *k = jstr(o, "kind");
        *kind = !k || !strcmp(k, "dm") ? EV_DM : !strcmp(k, "all") ? EV_ALL : !strcmp(k, "topic") ? EV_TOPIC : EV_WORK;
        proto_fmt_line(out, o, 200);
    } else if (!strncmp(frame, "st/", 3)) {
        const char *state = jstr(o, "state");
        *kind = EV_STATE;
        sb_addf(out, "%s is %s", name, state ? state : "?");
    } else {
        const char *t = jstr(o, "t"), *note = jstr(o, "note");
        int id = (int)jnum(o, "id", 0);
        if (!t) t = "";
        *kind = EV_GRID;
        if (!strcmp(t, "join"))
            sb_addf(out, "%s joined (%s)", name, note ? note : "?");
        else if (!strcmp(t, "leave"))
            sb_addf(out, "%s left", name);
        else if (!strcmp(t, "claim"))
            sb_addf(out, "%s took task #%d", name, id);
        else if (!strcmp(t, "done") || !strcmp(t, "fail"))
            sb_addf(out, "%s %s task #%d%s%s", name, !strcmp(t, "done") ? "finished" : "failed", id,
                    note ? ": " : "", note ? note : "");
        else if (!strcmp(t, "edit")) {
            *kind = EV_EDIT;
            sb_addf(out, "%s edited %s +%.0f -%.0f", name, note ? note : "?", jnum(o, "added", 0),
                    jnum(o, "removed", 0));
        } else
            sb_addf(out, "%s %s", name, t);
    }
    return o;
}

int cmd_tail(nq_ctx *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!daemon_alive(&c->p)) die("no grid is running for %s", c->p.dir);
    int sub = bus_sub_connect(c->p.ev_url);
    if (sub < 0) die("cannot subscribe: %s", bus_error());
    bus_sub_add(sub, "msg ");
    bus_sub_add(sub, "st/");
    bus_sub_add(sub, "ev ");
    bus_set_rcvtimeo(sub, 2000);
#ifndef _WIN32
    setvbuf(stdout, NULL, _IOLBF, 0);
#endif

    for (;;) {
        char *frame = bus_recv(sub, 0, NULL);
        if (!frame) {
            if (!daemon_alive(&c->p)) {
                puts("grid stopped");
                break;
            }
            continue;
        }
        sb_t line = {0};
        int kind;
        cJSON *o = cli_fmt_event(frame, &line, &kind);
        if (o) {
            print_time((uint64_t)jnum(o, "ts", (double)now_ms()));
            puts(line.p ? line.p : "");
            /* The MSVC CRT has no line buffering (_IOLBF is full buffering). */
            fflush(stdout);
        }
        sb_free(&line);
        cJSON_Delete(o);
        free(frame);
    }
    bus_close(sub);
    return 0;
}

int cmd_work(nq_ctx *c, int argc, char **argv)
{
    cJSON *q = cli_req("work");
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], "--all")) cJSON_AddBoolToObject(q, "all", 1);
    cJSON *r = cli_call(c, q);
    const cJSON *tasks = cJSON_GetObjectItemCaseSensitive(r, "tasks"), *t;
    if (cJSON_GetArraySize(tasks) == 0) puts("no open tasks");
    cJSON_ArrayForEach(t, tasks)
    {
        const char *ws = jstr(t, "wstate"), *who = jstr(t, "claimant"), *from = jstr(t, "from");
        const char *body = jstr(t, "body");
        size_t first = body ? strcspn(body, "\n") : 0;
        printf("#%-4.0f %-8s %-14s from %s: %.*s%s\n", jnum(t, "id", 0), ws ? ws : "?",
               who ? who : "-", from ? from : "?", (int)(first > 100 ? 100 : first), body ? body : "",
               body && (first > 100 || body[first]) ? " ..." : "");
    }
    cJSON_Delete(r);
    return 0;
}

int cmd_kill(nq_ctx *c, int argc, char **argv)
{
    if (argc < 1) die("usage: nuq kill NAME...");
    for (int i = 0; i < argc; i++) {
        cJSON *q = cli_req("kill");
        cJSON_AddStringToObject(q, "name", argv[i][0] == '@' ? argv[i] + 1 : argv[i]);
        cJSON_Delete(cli_call(c, q));
        printf("stopped %s\n", argv[i]);
    }
    return 0;
}

int cmd_attach(nq_ctx *c, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!tmux_has_session(c->p.session)) die("no grid is running for %s", c->p.dir);
    tmux_grid_keys(c->p.session);
    char target[128];
    snprintf(target, sizeof target, "=%s", c->p.session);
    const char *sock = getenv("NUQ_TMUX_SOCKET");
    const char *verb = getenv("TMUX") ? "switch-client" : "attach-session";
#ifdef _WIN32
    /* psmux mirrors the last attached session's options into the global
     * ones, so the grid's key table only holds if it is set right before
     * the attach; tmux_takeover does that (and restores the globals). */
    if (!getenv("TMUX")) return tmux_takeover(c->p.session, NULL);
    /* No exec on Windows: run tmux on this console and pass its status on. */
    const char *argv_sock[] = {"tmux", "-L", sock, verb, "-t", target, NULL};
    const char *argv_plain[] = {"tmux", verb, "-t", target, NULL};
    int rc = nq_spawn_wait(sock && *sock ? argv_sock : argv_plain);
    if (rc < 0) die("cannot run tmux");
    return rc;
#else
    if (sock && *sock)
        execlp("tmux", "tmux", "-L", sock, verb, "-t", target, (char *)NULL);
    else
        execlp("tmux", "tmux", verb, "-t", target, (char *)NULL);
    die("cannot run tmux");
#endif
}
