# The tmux grid: launching, labels, delivery into panes, and what must
# never be typed. Uses its own tmux server, so the user's sessions are
# never touched.
. "$(dirname "$0")/lib.sh"

: "${FAKE_AGENT:?}" "${FAKE_TUI:?}"
if ! command -v tmux >/dev/null 2>&1; then
    echo "tmux is not installed; skipping"
    exit 77
fi

tm() {
    tmux -L "$NUQ_TMUX_SOCKET" "$@"
}

panes() {
    tm list-panes -a -F '#{pane_id}' 2>/dev/null | grep -c .
}

# Columns of `nuq ls`: NAME AGENT STATE TASK INBOX PANE TOPICS
pane_of() {
    "$NQ" ls | awk -v n="$1" '$1 == n { print $6 }'
}

inbox_of() {
    "$NQ" ls | awk -v n="$1" '$1 == n { print $5 }'
}

step "the first agent creates the grid"
out=$("$NQ" --inject -n one "$FAKE_AGENT" </dev/null) || fail "launching one: $out"
has "$out" "one started in pane"
[ "$(panes)" = 1 ] || fail "expected 1 pane, found $(panes)"

step "the second joins it as another pane"
"$NQ" --inject -n two "$FAKE_TUI" </dev/null >/dev/null || fail "launching two"
[ "$(panes)" = 2 ] || fail "expected 2 panes, found $(panes)"
[ "$(tm list-sessions | grep -c .)" = 1 ] || fail "expected a single tmux session"
waitfor '[ "$("$NQ" ls | grep -c "%")" = 2 ]' "both agents to register their panes"
waitfor 'tm list-panes -a -F "#{@nq_label}" | grep -q "^one \[fake_agent.sh\]"' "pane label"

step "the first agent keeps the left half, full height"
"$NQ" --inject -n three "$FAKE_AGENT" </dev/null >/dev/null || fail "launching three"
waitfor '[ "$(panes)" = 3 ]' "three panes"
# geometry NAME -> "left top width height window_width window_height"
geometry() {
    tm list-panes -a -F '#{@nq_label}|#{pane_left} #{pane_top} #{pane_width} #{pane_height} #{window_width} #{window_height}' |
        grep "^$1 " | cut -d'|' -f2
}
check_main() {
    set -- $(geometry "$1")
    [ "$1" = 0 ] || return 1
    [ "$4" -ge $(($6 - 1)) ] || return 1
    half=$(($5 / 2))
    [ "$3" -ge $((half - 1)) ] && [ "$3" -le $((half + 1)) ]
}
waitfor 'check_main one' "one to be the full-height left half (got: $(geometry one))"
for other in two three; do
    set -- $(geometry "$other")
    [ "$1" -gt 0 ] || fail "$other should be in the right column, geometry: $*"
done
step "the split survives a resize"
tm resize-window -t "$(tm list-sessions -F '#{session_name}' | head -1):" -x 150 -y 46
waitfor 'check_main one' "one to stay the left half after a resize (got: $(geometry one))"
"$NQ" kill three >/dev/null
waitfor '[ "$(panes)" = 2 ]' "three's pane to close"

step "the pane runs in the grid directory with the launcher's environment"
p1=$(pane_of one)
case "$p1" in %[0-9]*) ;; *) fail "could not find one's pane id, got '$p1'" ;; esac
waitfor 'tm capture-pane -p -J -t "$p1" | grep -q "fake agent one ready in $(pwd -P)"' "agent banner"

step "default arguments come from the config file, before the ones given"
mkdir -p "$XDG_CONFIG_HOME/nuq"
cat >"$XDG_CONFIG_HOME/nuq/config" <<'CONFIG'
# flags that would otherwise sit in a shell alias
fake_agent.sh.args = --from-config "two words" it\'s
other.args = --not-this
CONFIG
"$NQ" --inject -n cfg "$FAKE_AGENT" --given </dev/null >/dev/null || fail "launching cfg"
waitfor 'ps -ax -o args= | grep -q -- "fake_agent.sh --from-config two words it.s --given\$"' "the configured arguments"
"$NQ" kill cfg >/dev/null
waitfor '[ "$(panes)" = 2 ]' "cfg's pane to close"
rm "$XDG_CONFIG_HOME/nuq/config"

step "from a terminal, launching opens the nuq UI instead of tmux"
# A second tmux server plays the user's terminal.
term() {
    tmux -L "${NUQ_TMUX_SOCKET}o" "$@"
}
screen() {
    term capture-pane -p -t ui
}
clients() {
    tm list-clients 2>/dev/null | grep -c .
}
zoomed_flag() {
    tm display-message -p -t "$(pane_of ui)" '#{window_zoomed_flag}'
}
term new-session -d -s ui -x 120 -y 34 "env LANG=en_US.UTF-8 '$NQ' --inject -n ui '$FAKE_AGENT'; echo UI-EXITED; sleep 60"
waitfor 'screen | grep -q "Agent terminal"' "the UI to open"
[ "$(clients)" = 0 ] || fail "launching should not attach to tmux"

step "the roster, narrow layout, and help stay usable"
waitfor 'screen | grep -q "Agent terminal / one"' "the first agent's details"
screen | grep -q "fake_agent.sh" || fail "the tile should show the agent's kind"
term send-keys -t ui Down
waitfor 'screen | grep -q "Agent terminal / two"' "selection to update the details"
term resize-window -t ui -x 58 -y 22
waitfor 'screen | grep "Agent terminal" | grep -qv "Agents"' "agent terminal in the stacked layout"
term send-keys -t ui F1
waitfor 'screen | grep -q ":send N|name|all text"' "help commands"
term send-keys -t ui Escape
waitfor 'screen | grep -q "Agent terminal / two"' "selection to survive resize and help"
term resize-window -t ui -x 120 -y 34

step "the agent tile is display only: typing never reaches the agent"
waitfor 'screen | grep -q "fake tui ready"' "the selected agent's live output"
term send-keys -t ui -l -- 'xyzzy'
printf 'pasted line' | term load-buffer -
term paste-buffer -p -d -t ui
sleep 1
hasnt "$(screen)" "xyzzy" "the agent tile (typed text)"
hasnt "$(screen)" "pasted line" "the agent tile (pasted text)"
hasnt "$(cat "$FAKE_AGENT_LOG" 2>/dev/null)" "xyzzy" "the agent input"
[ "$(clients)" = 0 ] || fail "typing should keep the dashboard open"

step "the bottom panel switches to a shell in the project directory"
term send-keys -t ui Tab
waitfor 'screen | grep -q "i type"' "the terminal tab"
term send-keys -t ui i
waitfor 'screen | grep -q "Terminal (typing)"' "typing mode"
term send-keys -t ui -l -- 'echo shell-$((6*7))'
term send-keys -t ui Enter
waitfor 'screen | grep -q "shell-42"' "the shell to run the command"
term send-keys -t ui F12
waitfor 'screen | grep -q "i type"' "F12 to stop typing"
hasnt "$(cat "$FAKE_AGENT_LOG" 2>/dev/null)" "shell-" "the agent input"
term send-keys -t ui Tab
waitfor 'screen | grep -q "Tab terminal"' "the changed-files tab"

step ":takeover shows the agent full screen; F12 and Ctrl-q come back"
term send-keys -t ui ':takeover ui' Enter
waitfor '[ "$(clients)" = 1 ]' "the takeover to attach"
waitfor 'screen | grep -q "fake agent ui ready"' "the agent to fill the terminal"
[ "$(zoomed_flag)" = 1 ] || fail "the taken-over pane should be zoomed"
screen | grep -q "back to nuq" || fail "tmux should show how to get back"
term send-keys -t ui F12
waitfor '[ "$(clients)" = 0 ]' "F12 to detach"
waitfor 'screen | grep -q "back from ui"' "the UI to return"
[ "$(zoomed_flag)" = 0 ] || fail "the pane should be unzoomed afterwards"
term send-keys -t ui ':t ui' Enter
waitfor '[ "$(clients)" = 1 ]' "the second takeover"
term send-keys -t ui C-q
waitfor '[ "$(clients)" = 0 ]' "Ctrl-q to detach"
[ "$(tm show-options -v -g key-table 2>/dev/null)" != nuq ] || fail "the global key table must not change"

step ":q leaves the UI and the agents keep running"
# The UI drops keys typed while it is coming back, so wait for it first.
sleep 1
waitfor 'screen | grep -q "Agent terminal"' "the UI to be back"
term send-keys -t ui ':q' Enter
waitfor 'screen | grep -q UI-EXITED' "the UI to exit"
"$NQ" ls | grep -q "^ui " || fail "ui should still be running"
term kill-server
rm -f "${TMUX_TMPDIR:-/tmp}/tmux-$(id -u)/${NUQ_TMUX_SOCKET}o"
"$NQ" kill ui >/dev/null
waitfor '[ "$(panes)" = 2 ]' "ui's pane to close"

step "a name can be used once"
out=$("$NQ" --inject -n one "$FAKE_AGENT" </dev/null 2>&1) && fail "a duplicate name should be refused"
has "$out" "already in use"
[ "$(panes)" = 2 ] || fail "a refused launch must not leave a pane behind"

step "an unknown command is reported, not launched"
out=$("$NQ" no-such-agent-xyz </dev/null 2>&1) && fail "launching a missing command should fail"
has "$out" "not found on your PATH"

step "a message is pasted into a line-mode agent and submitted"
as boss "$NQ" send @one -- hello one >/dev/null
waitfor 'grep -q "hello one" "$FAKE_AGENT_LOG"' "delivery to one"
has "$(cat "$FAKE_AGENT_LOG")" '<nuq id="1" from="boss" kind="dm">'

step "a multi-line message reaches a full-screen agent as one paste"
as boss "$NQ" send @two <<'EOF' >/dev/null
first line
second line
EOF
waitfor 'grep -q "second line" "$FAKE_AGENT_LOG"' "delivery to two"
has "$(cat "$FAKE_AGENT_LOG")" '<nuq id="2" from="boss" kind="dm">'
waitfor '[ "$(inbox_of two)" = 0 ]' "two's inbox to empty"
# One paste, one Enter: the whole message is a single submission.
[ "$(grep -c '^<nuq id="2"' "$FAKE_AGENT_LOG")" = 1 ] || fail "the message was submitted more than once"

step "nothing is ever submitted into a permission dialog"
"$NQ" --inject -n risky "$FAKE_TUI" --dialog </dev/null >/dev/null || fail "launching risky"
waitfor '"$NQ" ls | grep "^risky" | grep -q "%"' "risky to register"
as boss "$NQ" send @risky -- please continue >/dev/null
waitfor 'broker_log | grep -q "could not deliver to risky"' "the broker to give up"
if grep -q APPROVED "$FAKE_AGENT_LOG"; then
    fail "Enter was pressed on a dialog"
fi
[ "$(inbox_of risky)" = 1 ] || fail "the message should still be waiting for risky"

step "an agent that exits leaves the grid"
tm send-keys -t "$p1" "/exit" Enter
waitfor '! "$NQ" ls | grep -q "^one "' "one to deregister"
waitfor '[ "$(panes)" = 2 ]' "one's pane to close"
waitfor 'check_main two' "two to take over the left half when one leaves (got: $(geometry two))"

step "kill stops one agent"
"$NQ" kill two >/dev/null || fail "kill"
waitfor '[ "$(panes)" = 1 ]' "two's pane to close"
hasnt "$("$NQ" ls)" "two "

step "nuke ends the grid"
"$NQ" nuke >/dev/null || fail "nuke"
tm has-session 2>/dev/null && fail "the tmux session should be gone"
out=$("$NQ" ls 2>&1) && fail "the broker should be gone"

step "the grid can be started again and remembers people, not panes"
# The same name straight after `nuke`: the old pane's wrapper may still be
# saying goodbye, and that must not unregister the new agent.
"$NQ" --inject -n risky "$FAKE_AGENT" </dev/null >/dev/null || fail "relaunch"
waitfor '"$NQ" ls | grep "^risky" | grep -q "%"' "the new risky to register"
sleep 2
p3=$(pane_of risky)
case "$p3" in %[0-9]*) ;; *) fail "the new risky lost its registration, pane is '$p3'" ;; esac
[ "$(inbox_of risky)" = 0 ] || fail "a new agent must not inherit the old one's mail"
out=$("$NQ" ls)
has "$out" "boss"
hasnt "$out" "two "

step "when the last pane closes, the broker stops by itself"
tm send-keys -t "$p3" "/exit" Enter
waitfor '! "$NQ" ls' "the broker to stop"

echo ok
