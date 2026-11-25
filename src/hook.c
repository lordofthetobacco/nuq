#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "adapters.h"
#include "bus.h"
#include "cli.h"
#include "proto.h"
#include "util.h"

/* `nuq hook <agent> <event>` is what the agents' hook systems run.
 *
 * Rules: it must be quick, it must never fail the agent (exit 0 and print
 * nothing on error), and it must do nothing when the broker is gone. The
 * one deliberate non-zero exit is the background waiter's "wake up" (2). */

#define HOOK_TIMEOUT_MS 500
/* Agents cap injected context (Claude: 10k characters), so messages are
 * handed over in batches and long bodies are cut with a pointer to
 * `nuq show`. Whatever is left stays in the inbox for the next hook. */
#define HOOK_BATCH_BYTES 6000
#define HOOK_BODY_BYTES 5000

/* A "hook" request. $NUQ_TOKEN (set by the pane wrapper) ties it to this
 * run of the agent, so hooks left over from an earlier run with the same
 * name are refused by the broker. */
static cJSON *hook_req(const nq_ctx *c)
{
    cJSON *q = cli_req("hook");
    cJSON_AddStringToObject(q, "name", c->me);
    const char *token = getenv("NUQ_TOKEN");
    if (token && *token) cJSON_AddStringToObject(q, "token", token);
    return q;
}

/* One trip to the broker: optionally report a state, optionally collect
 * mail. Returns the formatted messages, or NULL if there are none. */
static char *exchange_with(const nq_ctx *c, const char *state, int drain, int claim, cJSON *extra)
{
    cJSON *q = hook_req(c);
    if (extra) {
        /* Move extra's fields into the request. */
        while (extra->child) {
            cJSON *item = cJSON_DetachItemViaPointer(extra, extra->child);
            cJSON_AddItemToObject(q, item->string, item);
        }
        cJSON_Delete(extra);
    }
    if (state) cJSON_AddStringToObject(q, "state", state);
    if (drain) {
        cJSON_AddBoolToObject(q, "drain", 1);
        cJSON_AddNumberToObject(q, "max", HOOK_BATCH_BYTES);
    }
    if (claim) cJSON_AddBoolToObject(q, "claim", 1);
    cJSON *r = cli_rpc(c, q, HOOK_TIMEOUT_MS);
    if (!r) return NULL;
    sb_t out = {0};
    const cJSON *msgs = cJSON_GetObjectItemCaseSensitive(r, "msgs");
    if (jbool(r, "ok") && cJSON_GetArraySize(msgs) > 0) {
        proto_fmt_msgs(&out, msgs, HOOK_BODY_BYTES);
        int left = (int)jnum(r, "left", 0);
        if (left > 0) sb_addf(&out, "\n\n(%d more waiting; they follow shortly)", left);
    }
    cJSON_Delete(r);
    return out.p;
}

static char *exchange(const nq_ctx *c, const char *state, int drain, int claim)
{
    return exchange_with(c, state, drain, claim, NULL);
}

/* The tool a PostToolUse event ran, and what it changed, for the stats. */
static cJSON *tool_activity(const cJSON *in)
{
    const char *tool = jstr(in, "tool_name");
    if (!tool) return NULL;
    cJSON *extra = cJSON_CreateObject();
    cJSON_AddStringToObject(extra, "tool", tool);
    cJSON *edits = cJSON_CreateArray();
    if (proto_edit_stats(tool, cJSON_GetObjectItemCaseSensitive(in, "tool_input"),
                         cJSON_GetObjectItemCaseSensitive(in, "tool_response"), edits) > 0)
        cJSON_AddItemToObject(extra, "edits", edits);
    else
        cJSON_Delete(edits);
    return extra;
}

static void print_json(cJSON *o)
{
    char *text = cJSON_PrintUnformatted(o);
    puts(text);
    free(text);
    cJSON_Delete(o);
}

/* {"hookSpecificOutput":{"hookEventName":...,"additionalContext":...}} is
 * understood by both Claude Code and Codex. */
static void emit_context(const char *event, const char *text)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *h = cJSON_AddObjectToObject(o, "hookSpecificOutput");
    cJSON_AddStringToObject(h, "hookEventName", event);
    cJSON_AddStringToObject(h, "additionalContext", text);
    print_json(o);
}

static void emit_bootstrap(const nq_ctx *c, const char *event)
{
    cJSON *ls = cli_rpc(c, cli_req("ls"), HOOK_TIMEOUT_MS);
    sb_t text = {0};
    adapter_bootstrap(&text, c->me, ls ? cJSON_GetObjectItemCaseSensitive(ls, "agents") : NULL);
    emit_context(event, text.p);
    sb_free(&text);
    cJSON_Delete(ls);
}

static void emit_mail(const char *event, char *mail)
{
    if (!mail) return;
    sb_t text = {0};
    sb_addf(&text, "New nuq messages:\n\n%s", mail);
    emit_context(event, text.p);
    sb_free(&text);
    free(mail);
}

/* End of a turn: if mail (or a queued task) is waiting, keep the agent
 * going with it instead of letting it go idle. */
static void emit_stop(const nq_ctx *c)
{
    char *mail = exchange(c, "idle", 1, 1);
    if (!mail) return;
    sb_t text = {0};
    sb_addf(&text, "nuq messages arrived while you were working:\n\n%s", mail);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "decision", "block");
    cJSON_AddStringToObject(o, "reason", text.p);
    print_json(o);
    sb_free(&text);
    free(mail);
}

/* Runs as a background hook (Claude Code: "asyncRewake") for as long as the
 * agent sits idle. When mail arrives it prints it to stderr and exits 2,
 * which wakes the idle session with that text. Nothing is typed into the
 * pane. In every other case it exits 0 and wakes nobody:
 *   - a turn starts (that turn's own hooks deliver from then on),
 *   - the agent or the broker goes away. */
static int wait_for_mail(const nq_ctx *c)
{
    int sub = bus_sub_connect(c->p.ev_url);
    if (sub < 0) return 0;
    char prefix[64];
    snprintf(prefix, sizeof prefix, "to/%s ", c->me);
    bus_sub_add(sub, prefix);
    snprintf(prefix, sizeof prefix, "st/%s ", c->me);
    bus_sub_add(sub, prefix);

    uint64_t start = now_ms();
    int seen_idle = 0, rc = 0;
    for (;;) {
        cJSON *q = hook_req(c);
        cJSON_AddBoolToObject(q, "waiter", 1);
        cJSON *r = cli_rpc(c, q, 2000);
        if (!r || !jbool(r, "ok")) {
            cJSON_Delete(r);
            break;
        }
        const char *state = jstr(r, "state");
        int idle = state && !strcmp(state, "idle");
        int waiting = (int)jnum(r, "left", 0);
        cJSON_Delete(r);

        if (!idle) {
            /* This hook starts alongside the one that reports "idle", so
             * give that a moment before concluding a turn is running. */
            if (seen_idle || now_ms() - start > 5000) break;
            sleep_ms(200);
            continue;
        }
        seen_idle = 1;
        if (waiting > 0) {
            char *mail = exchange(c, "busy", 1, 0);
            if (mail) {
                fprintf(stderr, "nuq messages arrived:\n\n%s\n", mail);
                free(mail);
                rc = 2;
                break;
            }
        }
        /* Events are hints; check in again every few seconds regardless.
         * That also keeps the broker from pasting a wake line itself. */
        bus_set_rcvtimeo(sub, 5000);
        free(bus_recv(sub, 0, NULL));
    }
    bus_close(sub);
    return rc;
}

/* Claude Code and Codex use the same event names and the same JSON in both
 * directions for the events handled here. */
static int hook_event(const nq_ctx *c, const char *event, const cJSON *in)
{
    /* Events raised inside a subagent belong to that subagent's context;
     * grid mail is for the main session. */
    int subagent = jstr(in, "agent_id") != NULL;

    if (!strcmp(event, "SessionStart")) {
        /* After a compaction the agent is still mid-turn. */
        const char *source = jstr(in, "source");
        free(exchange(c, source && !strcmp(source, "compact") ? NULL : "idle", 0, 0));
        emit_bootstrap(c, event);
    } else if (!strcmp(event, "UserPromptSubmit")) {
        emit_mail(event, exchange(c, "busy", 1, 0));
    } else if (!strcmp(event, "PostToolUse")) {
        /* A subagent's edits count, but its context is not where mail goes. */
        if (subagent) {
            cJSON *extra = tool_activity(in);
            if (extra) free(exchange_with(c, NULL, 0, 0, extra));
            return 0;
        }
        emit_mail(event, exchange_with(c, "busy", 1, 0, tool_activity(in)));
    } else if (!strcmp(event, "Stop")) {
        emit_stop(c);
    } else if (!strcmp(event, "Notification")) {
        const char *type = jstr(in, "notification_type");
        if (type && !strcmp(type, "permission_prompt")) free(exchange(c, "blocked", 0, 0));
    } else if (!strcmp(event, "PermissionRequest")) {
        free(exchange(c, "blocked", 0, 0));
    } else if (!strcmp(event, "Wait")) {
        return wait_for_mail(c);
    }
    return 0;
}

int cmd_hook(nq_ctx *c, int argc, char **argv)
{
    /* Outside a nuq pane these hooks have nothing to do. */
    if (argc < 2 || !getenv("NUQ_AGENT")) return 0;

    cJSON *in = NULL;
    if (!isatty(STDIN_FILENO)) {
        char *text = read_fd_all(STDIN_FILENO, 4 * 1024 * 1024, NULL);
        if (text) in = cJSON_Parse(text);
        free(text);
    }
    int rc = 0;
    if (!strcmp(argv[0], "claude") || !strcmp(argv[0], "codex")) rc = hook_event(c, argv[1], in);
    cJSON_Delete(in);
    fflush(stdout);
    return rc;
}
