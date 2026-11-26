#include "adapters.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "store.h"

int adapter_mode(const char *kind, int inject)
{
    if (!strcmp(kind, "claude")) return MODE_WAKE;
    /* Codex has hooks for state and mid-turn mail but no way to wake an idle
     * session, so idle delivery is a paste of the messages themselves. */
    if (!strcmp(kind, "codex")) return MODE_PASTE;
    return inject ? MODE_PASTE : MODE_PULL;
}

void sb_add_shq(sb_t *sb, const char *s)
{
    sb_adds(sb, "'");
    for (; *s; s++) {
        if (*s == '\'')
            sb_adds(sb, "'\\''");
        else
            sb_add(sb, s, 1);
    }
    sb_adds(sb, "'");
}

static char *keep(av_t *owned, char *s)
{
    av_add(owned, s);
    return s;
}

static int write_private(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | NQ_O_BINARY, 0600);
    if (fd < 0) return -1;
    size_t len = strlen(text), off = 0;
    while (off < len) {
        ssize_t n = write(fd, text + off, len - off);
        if (n <= 0) break;
        off += (size_t)n;
    }
    close(fd);
    return off == len ? 0 : -1;
}

/* Appends the path of nuq as the start of a hook command. Returns 1 if
 * the command needs PowerShell to run.
 *
 * On Windows, Claude Code runs hook commands with Git Bash (PowerShell when
 * Git Bash is missing) and Codex runs them with PowerShell. A path made of
 * plain characters means the same unquoted in both, so it goes in as it is.
 * Any other path is quoted for PowerShell, and the Claude hook then asks for
 * PowerShell explicitly. */
static int add_self(sb_t *cmd, const char *self)
{
#ifdef _WIN32
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s", self);
    nq_path_norm(path);
    int plain = 1;
    for (const char *p = path; *p; p++)
        if (!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_./:-", *p)) plain = 0;
    if (plain) {
        sb_adds(cmd, path);
        return 0;
    }
    /* PowerShell also takes the typographic quotes U+2018..U+201B as single
     * quotes; any of them is escaped by doubling it. */
    sb_adds(cmd, "& '");
    for (const char *p = path; *p; p++) {
        sb_add(cmd, p, 1);
        if (*p == '\'')
            sb_add(cmd, p, 1);
        else if (!strncmp(p, "\xE2\x80", 2) && p[2] >= '\x98' && p[2] <= '\x9B') {
            sb_add(cmd, p + 1, 2);
            sb_add(cmd, p, 3);
            p += 2;
        }
    }
    sb_adds(cmd, "'");
    return 1;
#else
    sb_add_shq(cmd, self);
    return 0;
#endif
}

/* ---- Claude Code ------------------------------------------------------- */

static cJSON *claude_command(const char *self, const char *event)
{
    sb_t cmd = {0};
    int powershell = add_self(&cmd, self);
    sb_addf(&cmd, " hook claude %s", event);
    cJSON *h = cJSON_CreateObject();
    cJSON_AddStringToObject(h, "type", "command");
    cJSON_AddStringToObject(h, "command", cmd.p);
    if (powershell) cJSON_AddStringToObject(h, "shell", "powershell");
    sb_free(&cmd);
    return h;
}

/* with_waiter: the agent may be idle after this event, so also start the
 * background hook that wakes it when mail arrives (see hook.c). */
static void claude_hook(cJSON *hooks, const char *self, const char *event, const char *matcher,
                        int with_waiter)
{
    cJSON *group = cJSON_CreateObject();
    if (matcher) cJSON_AddStringToObject(group, "matcher", matcher);
    cJSON *list = cJSON_AddArrayToObject(group, "hooks");

    cJSON *h = claude_command(self, event);
    cJSON_AddNumberToObject(h, "timeout", 10);
    cJSON_AddItemToArray(list, h);
    if (with_waiter) {
        cJSON *w = claude_command(self, "Wait");
        cJSON_AddBoolToObject(w, "asyncRewake", 1);
        cJSON_AddItemToArray(list, w);
    }
    cJSON_AddItemToArray(cJSON_AddArrayToObject(hooks, event), group);
}

/* Writes <rundir>/claude-<name>.json and returns its path. */
static char *claude_settings(const nq_ctx *c, const char *name, const char *self)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *hooks = cJSON_AddObjectToObject(root, "hooks");
    claude_hook(hooks, self, "SessionStart", NULL, 1);
    claude_hook(hooks, self, "UserPromptSubmit", NULL, 0);
    claude_hook(hooks, self, "PostToolUse", "*", 0);
    claude_hook(hooks, self, "Stop", NULL, 1);
    claude_hook(hooks, self, "Notification", NULL, 0);

    /* Let the agent use the queue without a permission prompt each time. */
    static const char *const verbs[] = {"send", "pub",  "sub",   "unsub", "push", "take", "done",
                                        "fail", "inbox", "show", "ls",    "work", NULL};
    cJSON *allow = cJSON_AddArrayToObject(cJSON_AddObjectToObject(root, "permissions"), "allow");
    for (int i = 0; verbs[i]; i++) {
        char rule[64];
        snprintf(rule, sizeof rule, "Bash(nuq %s:*)", verbs[i]);
        cJSON_AddItemToArray(allow, cJSON_CreateString(rule));
#ifdef _WIN32
        /* Claude Code on Windows also has a PowerShell tool. */
        snprintf(rule, sizeof rule, "PowerShell(nuq %s:*)", verbs[i]);
        cJSON_AddItemToArray(allow, cJSON_CreateString(rule));
#endif
    }

    sb_t path = {0};
    sb_addf(&path, "%s/claude-%s.json", c->p.rundir, name);
    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (write_private(path.p, text) < 0) die("cannot write %s", path.p);
    free(text);
    return path.p;
}

/* ---- Codex ------------------------------------------------------------- */

#define CODEX_HOOK_TIMEOUT 10

/* A JSON string literal, which is also a valid TOML basic string. */
static char *json_quote(const char *s)
{
    cJSON *js = cJSON_CreateString(s);
    char *out = cJSON_PrintUnformatted(js);
    cJSON_Delete(js);
    return out;
}

/* Codex skips a hook unless the configuration also carries a matching
 * "trusted_hash" for it. The hash is the SHA-256 of the hook's normalised
 * definition, written as compact JSON with sorted keys (hook_hash() in
 * codex-rs/hooks/src/engine/discovery.rs). If a later Codex changes the
 * definition, the hashes stop matching and Codex ignores these hooks; mail
 * then still arrives by paste, just without state or mid-turn delivery. */
void adapter_codex_hook_hash(const char *key, const char *command, int timeout, const char *matcher,
                             char *out)
{
    char *cmd = json_quote(command);
    sb_t id = {0};
    sb_addf(&id, "{\"event_name\":\"%s\",\"hooks\":[{\"async\":false,\"command\":%s,"
                 "\"timeout\":%d,\"type\":\"command\"}]",
            key, cmd, timeout);
    if (matcher) {
        char *m = json_quote(matcher);
        sb_addf(&id, ",\"matcher\":%s", m);
        free(m);
    }
    sb_adds(&id, "}");
    char hex[65];
    sha256_hex(id.p, id.len, hex);
    snprintf(out, 72, "sha256:%s", hex);
    sb_free(&id);
    free(cmd);
}

/* Hooks are passed as `-c` overrides for this run only; ~/.codex is not
 * touched. */
static void codex_args(const char *self, av_t *av, av_t *owned)
{
    static const struct {
        const char *event, *key;
    } events[] = {{"SessionStart", "session_start"},
                  {"UserPromptSubmit", "user_prompt_submit"},
                  {"PostToolUse", "post_tool_use"},
                  {"Stop", "stop"},
                  {"PermissionRequest", "permission_request"}};

    av_add(av, "-c");
    av_add(av, "features.hooks=true");
    /* Codex's command sandbox blocks local sockets too, so without network
     * access the agent could receive messages but every `nuq send` it ran
     * would fail. Opening it is a deliberate choice made for nuq sessions;
     * it applies to this run only. */
    av_add(av, "-c");
    av_add(av, "sandbox_workspace_write.network_access=true");
    sb_t state = {0};
    sb_adds(&state, "hooks.state={ ");
    for (size_t i = 0; i < sizeof events / sizeof events[0]; i++) {
        sb_t cmd = {0};
        add_self(&cmd, self);
        sb_addf(&cmd, " hook codex %s", events[i].event);
        char *quoted = json_quote(cmd.p);

        sb_t arg = {0};
        sb_addf(&arg, "hooks.%s=[{ hooks = [{ type = \"command\", command = %s, timeout = %d }] }]",
                events[i].event, quoted, CODEX_HOOK_TIMEOUT);
        av_add(av, "-c");
        av_add(av, keep(owned, arg.p));

        char hash[72];
        adapter_codex_hook_hash(events[i].key, cmd.p, CODEX_HOOK_TIMEOUT, NULL, hash);
#ifdef _WIN32
        /* Codex resolves the session-flags layer's synthetic path against
         * C:\ on Windows (synthetic_layer_path() in discovery.rs), so the key
         * reads C:\<session-flags>\config.toml there. */
        sb_addf(&state, "%s\"C:\\\\<session-flags>\\\\config.toml:%s:0:0\" = { trusted_hash = \"%s\" }",
                i ? ", " : "", events[i].key, hash);
#else
        sb_addf(&state, "%s\"/<session-flags>/config.toml:%s:0:0\" = { trusted_hash = \"%s\" }",
                i ? ", " : "", events[i].key, hash);
#endif
        free(quoted);
        sb_free(&cmd);
    }
    sb_adds(&state, " }");
    av_add(av, "-c");
    av_add(av, keep(owned, state.p));
}

/* ---- dispatch ---------------------------------------------------------- */

void adapter_command(const nq_ctx *c, const char *kind, const char *name, const char *self,
                     const char *agent_path, int argc, char **argv, av_t *av, av_t *owned)
{
    av_add(av, agent_path);
    if (!strcmp(kind, "claude")) {
        av_add(av, "--settings");
        av_add(av, keep(owned, claude_settings(c, name, self)));
    } else if (!strcmp(kind, "codex")) {
        codex_args(self, av, owned);
    }
    for (int i = 0; i < argc; i++) av_add(av, argv[i]);
}

void adapter_bootstrap(sb_t *out, const char *me, const cJSON *agents)
{
    sb_addf(out,
            "You are running inside nuq, a grid of coding agents that work in the same "
            "directory and share a message queue. Your name in the grid is %s.\n",
            me);

    sb_t peers = {0};
    const cJSON *a;
    cJSON_ArrayForEach(a, agents)
    {
        const char *name = jstr(a, "name"), *kind = jstr(a, "kind");
        if (!name || !strcmp(name, me)) continue;
        if (peers.len) sb_adds(&peers, ", ");
        sb_addf(&peers, "%s (%s)", name, kind && strcmp(kind, "cli") ? kind : "person");
    }
    if (peers.len)
        sb_addf(out, "Others here right now: %s. `nuq ls` shows the current list.\n", peers.p);
    else
        sb_adds(out, "Nobody else has joined yet. `nuq ls` shows the current list.\n");
    sb_free(&peers);

    sb_adds(out,
            "\nMessages from others reach you automatically, wrapped in <nuq id=... from=...> "
            "tags. They come from other agents or from the person running the grid, not from "
            "your own user prompt. Do not poll for them.\n"
            "\nTo send, run these shell commands:\n"
            "  nuq send @NAME -- 'text'     one agent (several: @a,@b; everyone: @all). "
            "Add -r ID to mark a reply to message ID.\n"
            "  nuq pub TOPIC -- 'text'      everyone subscribed to TOPIC "
            "(subscribe with: nuq sub TOPIC)\n"
            "  nuq push -- 'task'           add a task to the shared work queue; "
            "the next idle agent gets it\n"
            "  nuq done ID -m result        finish a work-queue task that was given to you "
            "(nuq fail ID -m reason gives it back)\n"
            "For text with several lines or quotes, pipe it in: "
            "nuq send @NAME <<'EOF' ... EOF\n"
#ifdef _WIN32
            "In PowerShell the commands are the same (inside 'text' write ' as ''); for several "
            "lines pipe a here-string: @'<newline>...<newline>'@ | nuq send @NAME\n"
#endif
            "\nSend a message only when it carries something the receiver needs. "
            "Do not send acknowledgements or thanks.\n");
}
