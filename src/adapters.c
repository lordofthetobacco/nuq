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
    /* Pi's extension delivers mail itself, idle or busy. The wake line is
     * only the fallback for a Pi that runs without it. */
    if (!strcmp(kind, "pi")) return MODE_WAKE;
    /* Codex has hooks for state and mid-turn mail but no way to wake an idle
     * session, so idle delivery is a paste of the messages themselves. */
    if (!strcmp(kind, "codex")) return MODE_PASTE;
    /* The same goes for Cursor's CLI and Devin. A paste also still arrives
     * if their hooks never run, which leaves the state unknown. */
    if (!strcmp(kind, "cursor") || !strcmp(kind, "devin")) return MODE_PASTE;
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

/* The queue commands an agent may run without being asked each time. */
static const char *const QUEUE_VERBS[] = {"send", "pub",  "sub",   "unsub", "push", "take", "done",
                                          "fail", "inbox", "show", "ls",    "work", NULL};

/* obj[key] as an object or an array, created (or put right) if need be. */
static cJSON *member(cJSON *obj, const char *key, int array)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (array ? cJSON_IsArray(it) : cJSON_IsObject(it)) return it;
    if (it) cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
    return array ? cJSON_AddArrayToObject(obj, key) : cJSON_AddObjectToObject(obj, key);
}

/* ---- Claude Code (and Devin, which reads the same hook format) ---------- */

static cJSON *claude_command(const char *self, const char *agent, const char *event)
{
    sb_t cmd = {0};
    int powershell = add_self(&cmd, self);
    sb_addf(&cmd, " hook %s %s", agent, event);
    cJSON *h = cJSON_CreateObject();
    cJSON_AddStringToObject(h, "type", "command");
    cJSON_AddStringToObject(h, "command", cmd.p);
    if (powershell && !strcmp(agent, "claude")) cJSON_AddStringToObject(h, "shell", "powershell");
    sb_free(&cmd);
    return h;
}

/* Adds a hook for `event` after any that are already in `hooks`.
 * with_waiter: the agent may be idle after this event, so also start the
 * background hook that wakes it when mail arrives (see hook.c). */
static void claude_hook(cJSON *hooks, const char *self, const char *agent, const char *event,
                        const char *matcher, int with_waiter)
{
    cJSON *group = cJSON_CreateObject();
    if (matcher) cJSON_AddStringToObject(group, "matcher", matcher);
    cJSON *list = cJSON_AddArrayToObject(group, "hooks");

    cJSON *h = claude_command(self, agent, event);
    cJSON_AddNumberToObject(h, "timeout", 10);
    cJSON_AddItemToArray(list, h);
    if (with_waiter) {
        cJSON *w = claude_command(self, agent, "Wait");
        cJSON_AddBoolToObject(w, "asyncRewake", 1);
        cJSON_AddItemToArray(list, w);
    }
    cJSON_AddItemToArray(member(hooks, event, 1), group);
}

/* Writes <rundir>/claude-<name>.json and returns its path. */
static char *claude_settings(const nq_ctx *c, const char *name, const char *self)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *hooks = cJSON_AddObjectToObject(root, "hooks");
    claude_hook(hooks, self, "claude", "SessionStart", NULL, 1);
    claude_hook(hooks, self, "claude", "UserPromptSubmit", NULL, 0);
    claude_hook(hooks, self, "claude", "PostToolUse", "*", 0);
    claude_hook(hooks, self, "claude", "Stop", NULL, 1);
    claude_hook(hooks, self, "claude", "Notification", NULL, 0);

    /* Let the agent use the queue without a permission prompt each time. */
    cJSON *allow = cJSON_AddArrayToObject(cJSON_AddObjectToObject(root, "permissions"), "allow");
    for (int i = 0; QUEUE_VERBS[i]; i++) {
        char rule[64];
        snprintf(rule, sizeof rule, "Bash(nuq %s:*)", QUEUE_VERBS[i]);
        cJSON_AddItemToArray(allow, cJSON_CreateString(rule));
#ifdef _WIN32
        /* Claude Code on Windows also has a PowerShell tool. */
        snprintf(rule, sizeof rule, "PowerShell(nuq %s:*)", QUEUE_VERBS[i]);
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
    /* Run sandboxed without approval prompts: commands stay inside the
     * workspace-write sandbox and a blocked one fails back to the model
     * instead of stopping for a person. These are config overrides, not
     * -a/-s flags, so a user's own --sandbox or -a still wins. */
    av_add(av, "-c");
    av_add(av, "approval_policy=\"never\"");
    av_add(av, "-c");
    av_add(av, "sandbox_mode=\"workspace-write\"");
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

/* ---- Devin ------------------------------------------------------------- */

/* Blanks out // and block comments, which Devin allows in its config files. */
static void strip_json_comments(char *s)
{
    int in_string = 0;
    for (; *s; s++) {
        if (in_string) {
            if (*s == '\\' && s[1])
                s++;
            else if (*s == '"')
                in_string = 0;
        } else if (*s == '"') {
            in_string = 1;
        } else if (s[0] == '/' && s[1] == '/') {
            while (*s && *s != '\n') *s++ = ' ';
            if (!*s) break;
        } else if (s[0] == '/' && s[1] == '*') {
            char *end = strstr(s + 2, "*/");
            end = end ? end + 2 : s + strlen(s);
            while (s < end) *s++ = ' ';
            s--;
        }
    }
}

/* The user's own Devin settings, or an empty object. */
static cJSON *devin_user_config(void)
{
    sb_t path = {0};
#ifdef _WIN32
    const char *base = getenv("APPDATA");
    if (base && *base) sb_addf(&path, "%s/devin/config.json", base);
#else
    const char *base = getenv("XDG_CONFIG_HOME"), *home = nq_home();
    if (base && *base)
        sb_addf(&path, "%s/devin/config.json", base);
    else if (home && *home)
        sb_addf(&path, "%s/.config/devin/config.json", home);
#endif
    cJSON *root = NULL;
    int fd = path.p ? open(path.p, O_RDONLY | NQ_O_BINARY) : -1;
    if (fd >= 0) {
        char *text = read_fd_all(fd, 4 * 1024 * 1024, NULL);
        close(fd);
        if (text) strip_json_comments(text);
        root = text ? cJSON_Parse(text) : NULL;
        free(text);
        if (!cJSON_IsObject(root)) {
            nq_warn("%s is not readable as JSON; Devin starts without your settings from it\n", path.p);
            cJSON_Delete(root);
            root = NULL;
        }
    }
    sb_free(&path);
    return root ? root : cJSON_CreateObject();
}

/* Devin has no flag for hooks alone, only --config for the whole settings
 * file. So this writes <rundir>/devin-<name>.json, a copy of the user's
 * settings with nuq's hooks and permission rules added, and returns its
 * path. ~/.config/devin is not touched. */
static char *devin_config(const nq_ctx *c, const char *name, const char *self)
{
    static const char *const events[] = {"SessionStart", "UserPromptSubmit", "PostToolUse", "Stop",
                                         "PermissionRequest"};
    cJSON *root = devin_user_config();
    cJSON *hooks = member(root, "hooks", 0);
    for (size_t i = 0; i < sizeof events / sizeof events[0]; i++)
        claude_hook(hooks, self, "devin", events[i], NULL, 0);

    /* A rule is a command prefix, matched by whole words. */
    cJSON *allow = member(member(root, "permissions", 0), "allow", 1);
    for (int i = 0; QUEUE_VERBS[i]; i++) {
        char rule[64];
        snprintf(rule, sizeof rule, "Exec(nuq %s)", QUEUE_VERBS[i]);
        cJSON_AddItemToArray(allow, cJSON_CreateString(rule));
    }

    sb_t path = {0};
    sb_addf(&path, "%s/devin-%s.json", c->p.rundir, name);
    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (write_private(path.p, text) < 0) die("cannot write %s", path.p);
    free(text);
    return path.p;
}

/* ---- Cursor ------------------------------------------------------------ */

/* Cursor's CLI runs the hooks of a plugin, and --plugin-dir names a plugin
 * directory for one run. Writes that plugin to <rundir>/cursor-<name>/ and
 * returns the directory. ~/.cursor is not touched.
 *
 * Cursor has its own event names and replies (see cursor_event() in
 * hook.c). There is no event for a permission prompt. */
static char *cursor_plugin(const nq_ctx *c, const char *name, const char *self)
{
    static const char *const events[] = {"sessionStart", "beforeSubmitPrompt", "postToolUse",
                                         "afterFileEdit", "stop"};
    sb_t dir = {0}, sub = {0}, file = {0};
    sb_addf(&dir, "%s/cursor-%s", c->p.rundir, name);
    sb_addf(&sub, "%s/.cursor-plugin", dir.p);
    if (mkdir_p(sub.p, 0700) < 0) die("cannot create %s", sub.p);
    sb_addf(&file, "%s/plugin.json", sub.p);
    if (write_private(file.p, "{\n  \"name\": \"nuq\",\n  \"description\": \"Connects this session to "
                              "the nuq grid it was started in.\"\n}\n") < 0)
        die("cannot write %s", file.p);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON *hooks = cJSON_AddObjectToObject(root, "hooks");
    for (size_t i = 0; i < sizeof events / sizeof events[0]; i++) {
        sb_t cmd = {0};
        add_self(&cmd, self);
        sb_addf(&cmd, " hook cursor %s", events[i]);
        cJSON *h = cJSON_CreateObject();
        cJSON_AddStringToObject(h, "command", cmd.p);
        cJSON_AddNumberToObject(h, "timeout", 10);
        /* Cursor stops following up after five rounds by default; mail is
         * not a loop, so there is no limit. */
        if (!strcmp(events[i], "stop")) cJSON_AddNullToObject(h, "loop_limit");
        cJSON_AddItemToArray(cJSON_AddArrayToObject(hooks, events[i]), h);
        sb_free(&cmd);
    }
    sb_free(&sub);
    sb_free(&file);
    sb_addf(&sub, "%s/hooks", dir.p);
    if (mkdir_p(sub.p, 0700) < 0) die("cannot create %s", sub.p);
    sb_addf(&file, "%s/hooks.json", sub.p);
    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (write_private(file.p, text) < 0) die("cannot write %s", file.p);
    free(text);
    sb_free(&sub);
    sb_free(&file);
    return dir.p;
}

/* ---- Pi ---------------------------------------------------------------- */

/* src/pi_extension.ts, compiled in (see cmake/EmbedFile.cmake). */
extern const unsigned char nq_pi_extension[];

/* Writes <rundir>/pi-<name>.ts and returns its path: the extension behind a
 * line that tells it where nuq is. Pi loads it with -e for this run only;
 * ~/.pi is not touched. */
static char *pi_extension(const nq_ctx *c, const char *name, const char *self)
{
    char *quoted = json_quote(self);
    sb_t text = {0};
    sb_addf(&text, "const NUQ = %s;\n%s", quoted, (const char *)nq_pi_extension);
    free(quoted);

    sb_t path = {0};
    sb_addf(&path, "%s/pi-%s.ts", c->p.rundir, name);
    if (write_private(path.p, text.p) < 0) die("cannot write %s", path.p);
    sb_free(&text);
    return path.p;
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
    } else if (!strcmp(kind, "pi")) {
        av_add(av, "-e");
        av_add(av, keep(owned, pi_extension(c, name, self)));
    } else if (!strcmp(kind, "cursor")) {
        av_add(av, "--plugin-dir");
        av_add(av, keep(owned, cursor_plugin(c, name, self)));
    } else if (!strcmp(kind, "devin")) {
        av_add(av, "--config");
        av_add(av, keep(owned, devin_config(c, name, self)));
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
