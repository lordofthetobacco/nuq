# Broker and messaging, without tmux or agents.
. "$(dirname "$0")/lib.sh"

step "no broker: commands say so and hooks stay silent"
out=$("$NQ" ls 2>&1) && fail "ls should fail without a broker"
has "$out" "no grid is running"
out=$(printf '{}' | as claude-1 "$NQ" hook claude Stop 2>&1) || fail "hook must exit 0 without a broker"
[ -z "$out" ] || fail "hook must print nothing without a broker, got: $out"

step "up is idempotent"
"$NQ" up >/dev/null || fail "up"
"$NQ" up >/dev/null || fail "second up"

step "direct messages"
has "$(as alice "$NQ" inbox)" "no new messages"
has "$(as bob "$NQ" send @alice -- hello there)" "sent #1 to alice"
out=$(as bob "$NQ" send @nobody hi 2>&1) && fail "send to unknown name should fail"
has "$out" "no agent named 'nobody'; known: alice, bob"
out=$(as alice "$NQ" inbox)
has "$out" '<nuq id="1" from="bob" kind="dm">'
has "$out" "hello there"
has "$(as alice "$NQ" inbox)" "no new messages" "second read"

step "text from stdin keeps quotes, dollars and newlines"
as carol "$NQ" inbox >/dev/null
as carol "$NQ" send @alice,@bob -r 1 <<'EOF' >/dev/null || fail "heredoc send"
line one
it's "quoted" and costs $5 `not run`
EOF
out=$(as bob "$NQ" inbox)
has "$out" 're="1"'
has "$out" "line one"
has "$out" "it's \"quoted\" and costs \$5 \`not run\`"

step "control characters are stripped"
printf 'safe\033[201~\033[31mtext\r\n' | as carol "$NQ" send @bob >/dev/null
out=$(as bob "$NQ" inbox)
has "$out" "safe[201~[31mtext"
case "$out" in *"$(printf '\033')"*) fail "an escape character survived" ;; esac

step "broadcast reaches everyone but the sender"
as alice "$NQ" inbox >/dev/null
as bob "$NQ" send @all standup >/dev/null
has "$(as alice "$NQ" inbox)" 'kind="all"'
has "$(as carol "$NQ" inbox)" "standup"
has "$(as bob "$NQ" inbox)" "no new messages"

step "topics reach subscribers only"
as alice "$NQ" sub review >/dev/null
as carol "$NQ" sub review >/dev/null
has "$(as bob "$NQ" pub review -- PR 12 is ready)" "(2 subscribers)"
has "$(as alice "$NQ" inbox)" 'topic="review"'
as carol "$NQ" unsub review >/dev/null
as carol "$NQ" inbox >/dev/null
has "$(as bob "$NQ" pub review again)" "(1 subscriber)"
has "$(as carol "$NQ" inbox)" "no new messages"
as alice "$NQ" inbox >/dev/null

step "work queue: each task goes to one taker, in order"
has "$(as bob "$NQ" push -- write the tests)" "queued task #"
as bob "$NQ" push update the docs >/dev/null
out=$(as alice "$NQ" take)
has "$out" "write the tests"
has "$out" "nuq done"
id1=$(printf '%s\n' "$out" | sed -n 's/^<nuq id="\([0-9]*\)".*/\1/p')
out=$(as alice "$NQ" take 2>&1) && fail "a second take while holding a task should fail"
has "$out" "you still hold task #$id1"
out=$(as carol "$NQ" take)
has "$out" "update the docs"
id2=$(printf '%s\n' "$out" | sed -n 's/^<nuq id="\([0-9]*\)".*/\1/p')
as carol "$NQ" take >/dev/null && fail "the queue should be empty"
out=$(as carol "$NQ" done "$id1" 2>&1) && fail "finishing someone else's task should fail"
has "$out" "claimed by alice"
as alice "$NQ" done "$id1" -m all green >/dev/null || fail "done"
has "$(as bob "$NQ" inbox)" "task #$id1 done: all green"
has "$("$NQ" work)" "#$id2"
hasnt "$("$NQ" work)" "write the tests"

step "listen wakes up when a message arrives"
(as alice "$NQ" listen -t 10 >"$T/listen.out" 2>&1 &)
sleep 0.5
as bob "$NQ" send @alice wake up >/dev/null
waitfor '[ -s "$T/listen.out" ]' "listen output"
has "$(cat "$T/listen.out")" "wake up"
as alice "$NQ" listen -t 1 >/dev/null && fail "listen with nothing to read should time out"

step "tail shows traffic"
("$NQ" tail >"$T/tail.out" 2>&1 &)
sleep 0.5
as bob "$NQ" send @alice for the log >/dev/null
waitfor 'grep -q "bob -> @alice: for the log" "$T/tail.out"' "tail line"

step "no message is lost when the sender exits at once"
as alice "$NQ" inbox >/dev/null
i=0
while [ "$i" -lt 40 ]; do
    i=$((i + 1))
    as bob "$NQ" send @alice "burst $i" >/dev/null || fail "burst send $i"
done
n=$(as alice "$NQ" inbox | grep -c '^burst ')
[ "$n" = 40 ] || fail "expected 40 burst messages, got $n"

step "state survives a broker restart"
as bob "$NQ" send @alice keep me >/dev/null
"$NQ" down >/dev/null
out=$("$NQ" ls 2>&1) && fail "broker should be down"
"$NQ" up >/dev/null || fail "restart"
has "$(as alice "$NQ" inbox)" "keep me"
has "$("$NQ" work)" "#$id2"
has "$("$NQ" ls)" "review"
as carol "$NQ" fail "$id2" -m no time >/dev/null || fail "fail after restart"
has "$("$NQ" show "$id2")" "status: failed by carol: no time"

step "a second broker for the same directory refuses to start"
out=$("$NQ" daemon 2>&1)
has "$out" "already running"

step "skills install copies the skill to every agent that is set up"
mkdir -p "$T/home/.claude" "$T/altcodex"
sk() {
    env -u CLAUDE_CONFIG_DIR -u CODEX_HOME HOME="$T/home" "$NQ" skills "$@"
}
has "$(sk status)" "Claude Code"
has "$(sk status)" "not installed"
has "$(sk status)" "Codex        not set up"
out=$(sk install) || fail "skills install failed: $out"
has "$out" "$T/home/.claude/skills/nuq/SKILL.md  installed"
hasnt "$out" "Codex"
[ -e "$T/home/.codex" ] && fail "an agent that is not set up must not get a directory"
sk show >"$T/skill.expected"
cmp -s "$T/skill.expected" "$T/home/.claude/skills/nuq/SKILL.md" || fail "the installed skill differs from the built-in one"
grep -q '^name: nuq$' "$T/home/.claude/skills/nuq/SKILL.md" || fail "the skill has no name line"
has "$(sk install)" "up to date"
has "$(sk status)" "installed, current"

step "a changed copy is replaced and kept as a backup"
echo "my own notes" >>"$T/home/.claude/skills/nuq/SKILL.md"
has "$(sk status)" "differs"
has "$(sk install)" "updated (previous copy kept as SKILL.md.bak)"
cmp -s "$T/skill.expected" "$T/home/.claude/skills/nuq/SKILL.md" || fail "the skill was not restored"
grep -q "my own notes" "$T/home/.claude/skills/nuq/SKILL.md.bak" || fail "the changed copy was not kept"

step "agent config directories moved by environment variables are honoured"
out=$(env -u CLAUDE_CONFIG_DIR CODEX_HOME="$T/altcodex" HOME="$T/home" "$NQ" skills install)
has "$out" "$T/altcodex/skills/nuq/SKILL.md  installed"
has "$out" "up to date"

step "skills install --dir and the no-agent case"
has "$(sk install --dir "$T/elsewhere/skills")" "$T/elsewhere/skills/nuq/SKILL.md  installed"
mkdir -p "$T/emptyhome"
out=$(env -u CLAUDE_CONFIG_DIR -u CODEX_HOME HOME="$T/emptyhome" "$NQ" skills install 2>&1) && fail "install with no agent set up should fail"
has "$out" "no agent configuration found"

step "doctor and completion"
out=$("$NQ" doctor) || fail "doctor reported a failure: $out"
has "$out" "broker for this dir"
has "$out" "running, pid"
has "$("$NQ" completion bash)" "complete -F _nuq nuq"
has "$("$NQ" completion zsh)" "#compdef nuq"
if command -v bash >/dev/null 2>&1; then
    "$NQ" completion bash | bash -n || fail "the bash completion script has a syntax error"
fi
if command -v zsh >/dev/null 2>&1; then
    "$NQ" completion zsh | zsh -n || fail "the zsh completion script has a syntax error"
fi

echo ok
