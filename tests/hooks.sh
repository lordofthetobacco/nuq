# The Claude Code integration, without Claude: a fake full-screen program
# named "claude" sits in the pane, and the test plays the part of Claude's
# hook system by running `nuq hook claude <event>` with the JSON Claude
# would send.
. "$(dirname "$0")/lib.sh"

: "${FAKE_TUI:?}"
if ! command -v tmux >/dev/null 2>&1; then
    echo "tmux is not installed; skipping"
    exit 77
fi

mkdir -p "$T/bin"
cp "$FAKE_TUI" "$T/bin/claude"
PATH="$T/bin:$PATH"
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
ps -ax -o args= | grep -q "^$T/bin/claude --settings $settings" || fail "claude was not started with --settings"
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

step "the dashboard runs full-screen and quits on q"
tmux -L "$NUQ_TMUX_SOCKET" new-window -d -n dash -- env LANG=en_US.UTF-8 "$NQ" dash
waitfor 'tmux -L "$NUQ_TMUX_SOCKET" capture-pane -p -t dash | grep -q "Message flow"' "the dashboard to draw"
screen=$(tmux -L "$NUQ_TMUX_SOCKET" capture-pane -p -t dash)
has "$screen" "claude-1" "the dashboard screen"
has "$screen" "Changed files" "the dashboard screen"
has "$screen" "src/x.c  [claude-1]" "the dashboard file attribution"
tmux -L "$NUQ_TMUX_SOCKET" send-keys -t dash q
waitfor '! tmux -L "$NUQ_TMUX_SOCKET" list-windows -F "#{window_name}" | grep -qx dash' "the dashboard to quit"

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

step "if the agent dies holding a task, the task returns to the queue"
"$NQ" kill claude-1 >/dev/null
has "$("$NQ" work)" "queued"

step "codex gets trusted hooks for this run and mail by paste"
cp "$FAKE_TUI" "$T/bin/codex"
: >"$FAKE_AGENT_LOG"
"$NQ" codex </dev/null >/dev/null || fail "launching codex"
waitfor '"$NQ" ls | grep "^codex-1" | grep -q "%"' "codex-1 to register"
waitfor 'tmux -L "$NUQ_TMUX_SOCKET" capture-pane -p -t "$("$NQ" ls | awk "\$1 == \"codex-1\" { print \$6 }")" | grep -q "fake tui ready"' "the fake codex to start"
args=$(ps -ax -o args= | grep "^$T/bin/codex ")
has "$args" "features.hooks=true" "codex arguments"
has "$args" "hook codex Stop" "codex arguments"
has "$args" 'config.toml:stop:0:0" = { trusted_hash = "sha256:' "codex arguments"
has "$args" "sandbox_workspace_write.network_access=true" "codex arguments"
hasnt "$args" "bypass-hook-trust" "codex arguments"
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

echo ok
