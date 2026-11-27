# The Claude Code integration, without Claude: a fake full-screen program
# named "claude" sits in the pane, and the test plays the part of Claude's
# hook system by running `nuq hook claude <event>` with the JSON Claude
# would send. Codex and Pi follow at the end, the same way.
tests_dir=$(cd "$(dirname "$0")" && pwd)
. "$tests_dir/lib.sh"
# node is a native program on Windows and needs a C:/ path.
[ -z "$NQ_WIN" ] || tests_dir=$(cygpath -m "$tests_dir")

: "${FAKE_TUI:?}"
if ! command -v tmux >/dev/null 2>&1; then
    echo "tmux is not installed; skipping"
    exit 77
fi

mkdir -p "$T/bin"
cp "$FAKE_TUI" "$T/bin/claude$EXE"
if [ -n "$NQ_WIN" ]; then
    # MSYS splits PATH on ':' when it hands it to native programs, so the
    # entry must be in /c/x form for C:/x/bin to reach nuq.exe.
    PATH="$(cygpath -u "$T")/bin:$PATH"
else
    PATH="$T/bin:$PATH"
fi
export PATH

# hook EVENT [JSON]   what Claude would run inside the pane of claude-1
hook() {
    printf '%s' "${2:-{\}}" | as claude-1 "$NQ" hook claude "$1"
}

state_of() {
    "$NQ" ls | awk -v n="$1" '$1 == n { print $3 }'
}

step "launching claude passes per-run settings, not the user's"
out=$("$NQ" claude </dev/null) || fail "launch: $out"
has "$out" "claude-1 started in pane"
waitfor '"$NQ" ls | grep "^claude-1" | grep -q "%"' "claude-1 to register"
settings=$(find "$T/run" -name 'claude-claude-1.json')
[ -n "$settings" ] || fail "no generated settings file"
for ev in SessionStart UserPromptSubmit PostToolUse Stop Notification; do
    grep -q "\"$ev\"" "$settings" || fail "settings lack a $ev hook"
done
grep -q 'hook claude Stop' "$settings" || fail "Stop hook does not call nuq"
grep -q 'Bash(nuq send:\*)' "$settings" || fail "nuq send is not pre-approved"
proc_args | grep -q "^$T/bin/claude --settings $settings" || fail "claude was not started with --settings"
[ "$(state_of claude-1)" = starting ] || fail "expected state starting, got $(state_of claude-1)"

step "SessionStart explains the grid and marks the agent idle"
as dana "$NQ" inbox >/dev/null
out=$(hook SessionStart '{"source":"startup"}')
has "$out" '"hookEventName":"SessionStart"'
has "$out" "Your name in the grid is claude-1"
has "$out" "dana (person)"
has "$out" "nuq send @NAME"
[ "$(state_of claude-1)" = idle ] || fail "expected idle after SessionStart"

step "an idle agent is woken with a short line; the hook carries the message"
as dana "$NQ" send @claude-1 -- please run the linter >/dev/null
waitfor 'grep -q "^\[nuq #[0-9]*\] 1 new message for claude-1" "$FAKE_AGENT_LOG"' "the wake line"
hasnt "$(cat "$FAKE_AGENT_LOG")" "please run the linter" "what was typed into the pane"
out=$(hook UserPromptSubmit '{"prompt":"[nuq] wake"}')
has "$out" '"hookEventName":"UserPromptSubmit"'
has "$out" "please run the linter"
has "$out" 'from=\"dana\"'
[ "$(state_of claude-1)" = busy ] || fail "expected busy after UserPromptSubmit"

step "a busy agent gets mail at its next tool call, with nothing typed"
lines_before=$(grep -c . "$FAKE_AGENT_LOG")
as dana "$NQ" send @claude-1 -- also check the docs >/dev/null
sleep 1.5
[ "$(grep -c . "$FAKE_AGENT_LOG")" = "$lines_before" ] || fail "a busy agent must not be typed at"
out=$(hook PostToolUse '{"tool_name":"Bash"}')
has "$out" '"hookEventName":"PostToolUse"'
has "$out" "also check the docs"
[ -z "$(hook PostToolUse '{"tool_name":"Bash"}')" ] || fail "an empty inbox should print nothing"

step "tool calls inside a subagent do not consume grid mail"
as dana "$NQ" send @claude-1 -- for the main session >/dev/null
[ -z "$(hook PostToolUse '{"tool_name":"Read","agent_id":"sub-1"}')" ] || fail "subagent hook took mail"

step "mail waiting at the end of a turn keeps the agent going"
out=$(hook Stop '{"stop_hook_active":false}')
has "$out" '"decision":"block"'
has "$out" "for the main session"
[ "$(state_of claude-1)" = busy ] || fail "expected busy after a blocking Stop"

step "with nothing waiting, Stop lets it go idle"
[ -z "$(hook Stop '{}')" ] || fail "Stop with an empty inbox should print nothing"
[ "$(state_of claude-1)" = idle ] || fail "expected idle after Stop"

step "a background waiter wakes the idle agent without typing anything"
lines_before=$(grep -c . "$FAKE_AGENT_LOG")
(
    hook Wait >"$T/wait.out" 2>"$T/wait.err"
    echo "$?" >"$T/wait.rc"
) &
sleep 0.5
as dana "$NQ" send @claude-1 -- via the waiter >/dev/null
waitfor '[ -s "$T/wait.rc" ]' "the waiter to finish"
[ "$(cat "$T/wait.rc")" = 2 ] || fail "the waiter should exit 2 to wake the agent, got $(cat "$T/wait.rc")"
has "$(cat "$T/wait.err")" "via the waiter" "the waiter's stderr"
[ -s "$T/wait.out" ] && fail "the waiter must not write to stdout"
[ "$(state_of claude-1)" = busy ] || fail "expected busy after the waiter delivered"
sleep 1
[ "$(grep -c . "$FAKE_AGENT_LOG")" = "$lines_before" ] || fail "with a waiter present nothing should be typed"
hook Stop '{}' >/dev/null

step "the waiter steps aside quietly when a turn starts"
(
    hook Wait >/dev/null 2>"$T/wait2.err"
    echo "$?" >"$T/wait2.rc"
) &
sleep 0.5
hook UserPromptSubmit '{}' >/dev/null
waitfor '[ -s "$T/wait2.rc" ]' "the second waiter to finish"
[ "$(cat "$T/wait2.rc")" = 0 ] || fail "a waiter with no mail must exit 0, got $(cat "$T/wait2.rc")"
[ -s "$T/wait2.err" ] && fail "a waiter with no mail must stay silent"
hook Stop '{}' >/dev/null

step "edits reported by hooks are counted per agent and per file"
hook PostToolUse "{\"tool_name\":\"Edit\",\"tool_input\":{\"file_path\":\"$PROJ/src/x.c\",\"old_string\":\"a\\nb\",\"new_string\":\"a\\nB\\nc\"}}" >/dev/null
hook PostToolUse "{\"tool_name\":\"Write\",\"tool_input\":{\"file_path\":\"$PROJ/notes.md\",\"content\":\"1\\n2\\n\"}}" >/dev/null
out=$("$NQ" dash --once)
has "$out" "doing: Write" "the dashboard"
has "$out" "lines +4 -1 in 2 files" "the dashboard"
has "$out" "src/x.c  (claude-1)" "the dashboard"
has "$out" "notes.md  (claude-1)" "the dashboard"
hasnt "$out" "$PROJ/src" "the dashboard (paths should be relative)"
has "$out" "* claude-1 (claude)" "the dashboard (main agent mark)"
has "$out" "dana -> @claude-1" "the dashboard's message flow"

step "the dashboard runs full-screen and quits on Ctrl-q"
if [ -n "$NQ_WIN" ]; then
    # A native pane has no env program; its console is UTF-8 without LANG.
    set -- "$NQ" dash
else
    set -- env LANG=en_US.UTF-8 "$NQ" dash
fi
tmux -L "$NUQ_TMUX_SOCKET" new-window -d -n dash -- "$@"
# psmux reads a bare target as a session name; ":dash" is the window.
dash=dash
[ -z "$NQ_WIN" ] || dash=:dash
waitfor 'tmux -L "$NUQ_TMUX_SOCKET" capture-pane -p -t "$dash" | grep -Fq "[Agents"' "the dashboard to draw"
screen=$(tmux -L "$NUQ_TMUX_SOCKET" capture-pane -p -t "$dash")
has "$screen" "claude-1" "the dashboard screen"
has "$screen" "Changed files" "the dashboard screen"
has "$screen" "src/x.c  [claude-1]" "the dashboard file attribution"
tmux -L "$NUQ_TMUX_SOCKET" send-keys -t "$dash" C-q
waitfor '! tmux -L "$NUQ_TMUX_SOCKET" list-windows -F "#{window_name}" | grep -qx dash' "the dashboard to quit"

if [ -n "$NQ_WIN" ]; then
    step "Windows paths from hooks are made relative whatever their spelling"
    # Claude Code on Windows reports C:\x\y paths (escaped in JSON), with
    # the drive letter in whatever case the directory was opened with.
    winproj=$(printf '%s' "$PROJ" | sed 's|^C:|c:|; s|/|\\\\|g')
    hook PostToolUse "{\"tool_name\":\"Write\",\"tool_input\":{\"file_path\":\"$winproj\\\\src\\\\y.c\",\"content\":\"1\\n\"}}" >/dev/null
    out=$("$NQ" dash --once)
    has "$out" "src/y.c  (claude-1)" "the dashboard"
    hasnt "$out" 'src\y.c' "the dashboard"
fi

step "a permission prompt blocks waking"
hook Notification '{"notification_type":"permission_prompt"}'
[ "$(state_of claude-1)" = blocked ] || fail "expected blocked"
lines_before=$(grep -c . "$FAKE_AGENT_LOG")
as dana "$NQ" send @claude-1 -- held back >/dev/null
sleep 1.5
[ "$(grep -c . "$FAKE_AGENT_LOG")" = "$lines_before" ] || fail "a blocked agent must not be typed at"
out=$(hook PostToolUse '{"tool_name":"Bash"}')
has "$out" "held back"
hook Stop '{}' >/dev/null

step "long messages are cut with a pointer to the full text"
awk 'BEGIN { for (i = 0; i < 900; i++) printf "0123456789"; print "" }' | as dana "$NQ" send @claude-1 >/dev/null
waitfor '[ "$(state_of claude-1)" = idle ]'
out=$(hook UserPromptSubmit '{}')
has "$out" "[truncated:"
has "$out" "nuq show"
[ "${#out}" -lt 9000 ] || fail "hook output is ${#out} characters; Claude caps context at 10000"
hook Stop '{}' >/dev/null

step "a queued task is handed to the idle agent"
waitfor '[ "$(state_of claude-1)" = idle ]'
as dana "$NQ" push -- tidy the changelog >/dev/null
waitfor '"$NQ" ls | awk "\$1 == \"claude-1\"" | grep -q "#"' "the task to be assigned"
out=$(hook UserPromptSubmit '{}')
has "$out" "tidy the changelog"
has "$out" "nuq done"
task=$("$NQ" work | sed -n 's/^#\([0-9]*\).*/\1/p')
as claude-1 "$NQ" done "$task" -m changelog tidied >/dev/null || fail "done"
has "$(as dana "$NQ" inbox)" "task #$task done: changelog tidied"

step "a task queued while it works is picked up when the turn ends"
as dana "$NQ" push -- second task >/dev/null
out=$(hook Stop '{}')
has "$out" '"decision":"block"'
has "$out" "second task"

step "codex gets trusted hooks for this run and mail by paste"
cp "$FAKE_TUI" "$T/bin/codex$EXE"
: >"$FAKE_AGENT_LOG"
"$NQ" codex </dev/null >/dev/null || fail "launching codex"
waitfor '"$NQ" ls | grep "^codex-1" | grep -q "%"' "codex-1 to register"
waitfor 'tmux -L "$NUQ_TMUX_SOCKET" capture-pane -p -t "$("$NQ" ls | awk "\$1 == \"codex-1\" { print \$6 }")" | grep -q "fake tui ready"' "the fake codex to start"
args=$(proc_args | grep "^$T/bin/codex ")
has "$args" "features.hooks=true" "codex arguments"
has "$args" "hook codex Stop" "codex arguments"
has "$args" 'config.toml:stop:0:0" = { trusted_hash = "sha256:' "codex arguments"
has "$args" "sandbox_workspace_write.network_access=true" "codex arguments"
hasnt "$args" "bypass-hook-trust" "codex arguments"

step "if the agent dies holding a task, the task returns to the queue"
# Codex keeps the grid alive but cannot claim work before SessionStart.
"$NQ" kill claude-1 >/dev/null
has "$("$NQ" work)" "queued"

codex_hook() {
    printf '%s' "${2:-{\}}" | as codex-1 "$NQ" hook codex "$1"
}
has "$(codex_hook SessionStart '{"source":"startup"}')" "Your name in the grid is codex-1"

step "the task that went back to the queue is pasted to the idle codex"
waitfor 'grep -q "second task" "$FAKE_AGENT_LOG"' "the task to be pasted into codex"
has "$(cat "$FAKE_AGENT_LOG")" "nuq done"
has "$(cat "$FAKE_AGENT_LOG")" "to answer run: nuq send"
waitfor '[ "$(state_of codex-1)" = busy ]' "codex-1 to be marked busy after the paste"

step "while codex works, mail waits for a hook instead of being typed"
as dana "$NQ" send @codex-1 -- hello codex >/dev/null
sleep 1.5
hasnt "$(cat "$FAKE_AGENT_LOG")" "hello codex" "what was typed into codex"
out=$(codex_hook Stop '{}')
has "$out" '"decision":"block"'
has "$out" "hello codex"
[ -z "$(codex_hook Stop '{}')" ] || fail "Stop with an empty inbox should print nothing"
[ "$(state_of codex-1)" = idle ] || fail "expected codex-1 idle after Stop"

step "an idle codex gets the next message by paste"
as dana "$NQ" send @codex-1 -- pasted while idle >/dev/null
waitfor 'grep -q "pasted while idle" "$FAKE_AGENT_LOG"' "the paste into codex"

step "an approval prompt in codex holds mail back"
codex_hook PermissionRequest '{}'
[ "$(state_of codex-1)" = blocked ] || fail "expected codex-1 blocked"
as dana "$NQ" send @codex-1 -- not into the dialog >/dev/null
sleep 1.5
hasnt "$(cat "$FAKE_AGENT_LOG")" "not into the dialog" "what was typed into codex"
has "$(codex_hook PostToolUse '{}')" "not into the dialog"

step "pi is started with nuq's extension for this run only"
cp "$FAKE_TUI" "$T/bin/pi$EXE"
"$NQ" pi </dev/null >/dev/null || fail "launching pi"
waitfor '"$NQ" ls | grep "^pi-1" | grep -q "%"' "pi-1 to register"
ext=$(find "$T/run" -name 'pi-pi-1.ts')
[ -n "$ext" ] || fail "no generated extension file"
waitfor 'proc_args | grep -Fq -- "$T/bin/pi -e $ext"' "pi to start with -e"
head -1 "$ext" | grep -q '^const NUQ = ".*nuq.*";$' || fail "the extension does not know where nuq is"
grep -q '"hook", "pi", event' "$ext" || fail "the extension does not call nuq hook"

pi_hook() {
    printf '%s' "${2:-{\}}" | as pi-1 "$NQ" hook pi "$1"
}

step "pi's hooks introduce the grid and count its edits"
has "$(pi_hook SessionStart)" "Your name in the grid is pi-1"
[ "$(state_of pi-1)" = idle ] || fail "expected pi-1 idle after SessionStart"
pi_hook PostToolUse '{"tool_name":"edit","tool_input":{"path":"src/p.c","edits":[{"oldText":"a","newText":"b\nc"}]}}' >/dev/null
out=$("$NQ" dash --once)
has "$out" "doing: edit" "the dashboard"
has "$out" "src/p.c  (pi-1)" "the dashboard"

if command -v node >/dev/null 2>&1; then
    # The extension itself, with node playing Pi (tests/pi_host.mjs).
    cp "$ext" "$T/pi_ext.mjs"
    pi_host() {
        NQ="$NQ" as pi-1 node "$tests_dir/pi_host.mjs" "$T/pi_ext.mjs" "$@"
    }

    step "the extension wakes an idle pi itself; nothing is typed into its pane"
    : >"$FAKE_AGENT_LOG"
    out=$(pi_host session_start 'send:wake up pi' wait_message before_agent_start)
    has "$out" 'sendMessage triggerTurn=true deliverAs=steer "nuq messages arrived:' "the extension"
    has "$out" "wake up pi" "the extension"
    has "$out" 'intro="You are running inside nuq' "the extension (system prompt section)"
    has "$out" "message=null" "the extension"
    hasnt "$(cat "$FAKE_AGENT_LOG")" "pi-1" "what was typed into pi"
    [ "$(state_of pi-1)" = busy ] || fail "expected pi-1 busy during a run"

    step "mail waiting at a prompt joins the run; during the run it is steered in"
    as dana "$NQ" send @pi-1 -- before the prompt >/dev/null
    out=$(pi_host before_agent_start 'send:mid-turn mail' \
        'tool_result:{"toolName":"write","input":{"path":"pi.txt","content":"1\n2\n"},"isError":false}' \
        'send:lost edit' \
        'tool_result:{"toolName":"edit","input":{"path":"no.txt","edits":[{"oldText":"a","newText":"b"}]},"isError":true}')
    has "$out" 'message="New nuq messages:' "the extension"
    has "$out" "before the prompt" "the extension"
    has "$out" 'sendMessage triggerTurn=true deliverAs=steer "New nuq messages:' "the extension"
    has "$out" "mid-turn mail" "the extension"
    has "$out" "lost edit" "the extension"
    out=$("$NQ" dash --once)
    has "$out" "pi.txt  (pi-1)" "the dashboard"
    hasnt "$out" "no.txt" "the dashboard (a failed edit changed nothing)"

    step "mail at the end of a run starts another; otherwise pi goes idle"
    out=$(pi_host before_agent_start 'send:end of run mail' agent_settled)
    has "$out" 'sendMessage triggerTurn=true deliverAs=steer "nuq messages arrived while you were working:' "the extension"
    has "$out" "end of run mail" "the extension"
    [ "$(state_of pi-1)" = busy ] || fail "expected pi-1 busy with the mail it was handed"
    out=$(pi_host before_agent_start agent_settled)
    hasnt "$out" "sendMessage" "the extension"
    [ "$(state_of pi-1)" = idle ] || fail "expected pi-1 idle after a run with no mail"

    step "an extension dialog marks pi blocked until it is answered"
    pi_host before_agent_start ui_prompt_start >/dev/null
    [ "$(state_of pi-1)" = blocked ] || fail "expected pi-1 blocked"
    pi_host before_agent_start ui_prompt_start ui_prompt_end >/dev/null
    [ "$(state_of pi-1)" = busy ] || fail "expected pi-1 busy after the dialog"
fi

step "cursor's CLI gets its hooks as a plugin for this run only"
# Cursor's CLI is installed as "agent"; in the grid it is called cursor.
cp "$FAKE_TUI" "$T/bin/agent$EXE"
: >"$FAKE_AGENT_LOG"
"$NQ" agent </dev/null >/dev/null || fail "launching cursor's agent"
waitfor '"$NQ" ls | grep "^cursor-1" | grep -q "%"' "cursor-1 to register"
plugin=$(find "$T/run" -type d -name 'cursor-cursor-1')
[ -n "$plugin" ] || fail "no generated plugin directory"
waitfor 'proc_args | grep -Fq -- "$T/bin/agent --plugin-dir $plugin"' "cursor to start with --plugin-dir"
grep -q '"name": "nuq"' "$plugin/.cursor-plugin/plugin.json" || fail "the plugin has no manifest"
for ev in sessionStart beforeSubmitPrompt postToolUse afterFileEdit stop; do
    grep -q "hook cursor $ev" "$plugin/hooks/hooks.json" || fail "the plugin lacks a $ev hook"
done
grep -q 'loop_limit.*null' "$plugin/hooks/hooks.json" || fail "follow-ups should not be limited"

cursor_hook() {
    printf '%s' "${2:-{\}}" | as cursor-1 "$NQ" hook cursor "$1"
}

step "cursor's hooks answer in cursor's own format"
out=$(cursor_hook sessionStart '{"session_id":"s1"}')
has "$out" '"additional_context":"You are running inside nuq'
has "$out" "Your name in the grid is cursor-1"
[ "$(state_of cursor-1)" = idle ] || fail "expected cursor-1 idle after sessionStart"

step "an idle cursor gets mail by paste"
as dana "$NQ" send @cursor-1 -- pasted to cursor >/dev/null
waitfor 'grep -q "pasted to cursor" "$FAKE_AGENT_LOG"' "the paste into cursor"
waitfor '[ "$(state_of cursor-1)" = busy ]' "cursor-1 to be marked busy after the paste"

step "while cursor works, mail comes with a tool result or as a follow-up"
[ -z "$(cursor_hook beforeSubmitPrompt '{"prompt":"go"}')" ] || fail "beforeSubmitPrompt should print nothing"
as dana "$NQ" send @cursor-1 -- with the tool result >/dev/null
sleep 1.5
hasnt "$(cat "$FAKE_AGENT_LOG")" "with the tool result" "what was typed into cursor"
out=$(cursor_hook postToolUse '{"tool_name":"Shell","tool_input":{"command":"ls"}}')
has "$out" '"additional_context":"New nuq messages:'
has "$out" "with the tool result"
cursor_hook afterFileEdit "{\"file_path\":\"$PROJ/src/c.c\",\"edits\":[{\"old_string\":\"a\",\"new_string\":\"b\\nc\"}]}" >/dev/null
out=$("$NQ" dash --once)
has "$out" "doing: Shell" "the dashboard"
has "$out" "src/c.c  (cursor-1)" "the dashboard"
as dana "$NQ" send @cursor-1 -- at the end of the turn >/dev/null
out=$(cursor_hook stop '{"status":"completed","loop_count":0}')
has "$out" '"followup_message":"nuq messages arrived while you were working:'
has "$out" "at the end of the turn"
[ "$(state_of cursor-1)" = busy ] || fail "expected cursor-1 busy with the follow-up"
[ -z "$(cursor_hook stop '{"status":"completed","loop_count":1}')" ] || fail "stop with an empty inbox should print nothing"
[ "$(state_of cursor-1)" = idle ] || fail "expected cursor-1 idle after stop"

step "an interrupted cursor turn takes no follow-up; the mail is pasted instead"
cursor_hook beforeSubmitPrompt >/dev/null
as dana "$NQ" send @cursor-1 -- after the interrupt >/dev/null
[ -z "$(cursor_hook stop '{"status":"aborted","loop_count":0}')" ] || fail "an aborted stop should print nothing"
waitfor 'grep -q "after the interrupt" "$FAKE_AGENT_LOG"' "the paste after an interrupt"

step "devin gets a copy of its settings with nuq's hooks, for this run only"
cp "$FAKE_TUI" "$T/bin/devin$EXE"
devin_dir="$XDG_CONFIG_HOME/devin"
if [ -n "$NQ_WIN" ]; then
    export APPDATA="$T/appdata"
    devin_dir="$APPDATA/devin"
fi
mkdir -p "$devin_dir"
cat >"$devin_dir/config.json" <<'CONFIG'
{
  // the user's own settings, with the comments Devin allows
  "agent": { "model": "their-model" }, /* kept */
  "permissions": { "allow": ["Exec(git)"] },
  "hooks": { "Stop": [{ "hooks": [{ "type": "command", "command": "echo // theirs" }] }] }
}
CONFIG
"$NQ" devin </dev/null >/dev/null || fail "launching devin"
waitfor '"$NQ" ls | grep "^devin-1" | grep -q "%"' "devin-1 to register"
cfg=$(find "$T/run" -name 'devin-devin-1.json')
[ -n "$cfg" ] || fail "no generated config file"
waitfor 'proc_args | grep -Fq -- "$T/bin/devin --config $cfg"' "devin to start with --config"
for ev in SessionStart UserPromptSubmit PostToolUse Stop PermissionRequest; do
    grep -q "hook devin $ev" "$cfg" || fail "the config lacks a $ev hook"
done
grep -q 'Exec(nuq send)' "$cfg" || fail "nuq send is not pre-approved"
grep -q 'their-model' "$cfg" || fail "the user's settings were not carried over"
grep -q 'Exec(git)' "$cfg" || fail "the user's permission rules were not carried over"
grep -q 'echo // theirs' "$cfg" || fail "the user's own hooks were not carried over"
if grep -q nuq "$devin_dir/config.json"; then
    fail "the user's settings file was edited"
fi

devin_hook() {
    printf '%s' "${2:-{\}}" | as devin-1 "$NQ" hook devin "$1"
}

step "devin's hooks speak claude's format"
has "$(devin_hook SessionStart '{"source":"startup"}')" "Your name in the grid is devin-1"
[ "$(state_of devin-1)" = idle ] || fail "expected devin-1 idle after SessionStart"
devin_hook UserPromptSubmit '{"prompt":"go"}' >/dev/null
devin_hook PermissionRequest '{"tool_name":"exec","tool_input":{"command":"make"}}'
[ "$(state_of devin-1)" = blocked ] || fail "expected devin-1 blocked"
as dana "$NQ" send @devin-1 -- hello devin >/dev/null
out=$(devin_hook PostToolUse '{"tool_name":"edit","tool_input":{"file_path":"src/d.c","old_string":"a","new_string":"b"},"tool_response":{"success":true,"output":"","error":null}}')
has "$out" '"additionalContext":"New nuq messages:'
has "$out" "hello devin"
devin_hook PostToolUse '{"tool_name":"edit","tool_input":{"file_path":"src/failed.c","old_string":"a","new_string":"b"},"tool_response":{"success":false,"output":"","error":"no match"}}' >/dev/null
out=$("$NQ" dash --once)
has "$out" "src/d.c  (devin-1)" "the dashboard"
hasnt "$out" "src/failed.c" "the dashboard (a failed edit changed nothing)"
[ -z "$(devin_hook Stop '{"stop_hook_active":false}')" ] || fail "Stop with an empty inbox should print nothing"
[ "$(state_of devin-1)" = idle ] || fail "expected devin-1 idle after Stop"

echo ok
