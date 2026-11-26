# Shared by the integration tests. Sourced, not run.
#
# The tests are executed by bash and by zsh (and bus.sh by sh as well), so
# everything here sticks to syntax that means the same in all three: every
# expansion is quoted and nothing relies on word splitting.
#
# Expects in the environment: NQ (path to the nuq binary).

set -u

: "${NQ:?NQ must point at the nuq binary}"

# Native Windows: the scripts run under Git Bash and drive nuq.exe and
# psmux's tmux.exe, which are not MSYS programs.
case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*) NQ_WIN=1 ;;
*) NQ_WIN= ;;
esac

# A short path: unix socket addresses are limited to about 104 bytes.
T=$(mktemp -d /tmp/nqt.XXXXXX)
if [ -n "$NQ_WIN" ]; then
    # nuq prints and compares C:/x/y paths, so the tests use that form too
    # (Git Bash accepts it). MSYS must not rewrite arguments that look like
    # POSIX paths, such as /exit, on their way to native programs.
    T=$(cygpath -ml "$T")
    export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
    # psmux otherwise keeps a spare server with shells started in the
    # current directory, which would keep $T from being removed.
    export PSMUX_NO_WARM=1
    # There is no ps; the fake agents record their own command lines.
    export FAKE_ARGS_LOG="$T/args.log"
    export FAKE_START_LOG="$T/start.log"
    EXE=.exe
else
    EXE=
fi
PROJ="$T/proj"
mkdir -p "$PROJ" "$T/run"
chmod 700 "$T/run"

export XDG_RUNTIME_DIR="$T/run"
export XDG_STATE_HOME="$T/state"
export XDG_CONFIG_HOME="$T/config"
# Should a real agent ever be started by mistake, it must not touch the
# user's own configuration.
export CLAUDE_CONFIG_DIR="$T/claude-config" CODEX_HOME="$T/codex-home"
export NUQ_TMUX_SOCKET="nqt$$"
export FAKE_AGENT_LOG="$T/agent.log"
unset NUQ_DIR NUQ_AGENT NUQ_TOKEN TMUX TMUX_PANE

cd "$PROJ" || exit 1

cleanup() {
    "$NQ" nuke >/dev/null 2>&1
    tmux -L "$NUQ_TMUX_SOCKET" kill-server >/dev/null 2>&1
    # grid.sh plays the user's terminal with a second server.
    tmux -L "${NUQ_TMUX_SOCKET}o" kill-server >/dev/null 2>&1
    # tmux can leave the socket file of a killed server behind.
    rm -f "${TMUX_TMPDIR:-/tmp}/tmux-$(id -u)/$NUQ_TMUX_SOCKET"
    cd /
    rm -rf "$T"
}
trap cleanup EXIT

broker_log() {
    find "$T/state" -name daemon.log -exec cat {} + 2>/dev/null
}

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    printf '%s\n' '--- broker log:' >&2
    broker_log >&2
    exit 1
}

# as NAME command...   run a command as that grid member
as() {
    _who=$1
    shift
    env NUQ_AGENT="$_who" "$@"
}

# has TEXT NEEDLE [what]
has() {
    case "$1" in
    *"$2"*) ;;
    *) fail "${3:-output} should contain '$2' but was: $1" ;;
    esac
}

hasnt() {
    case "$1" in
    *"$2"*) fail "${3:-output} should not contain '$2' but was: $1" ;;
    *) ;;
    esac
}

# waitfor 'shell condition' [what]   polls for up to 15 seconds
waitfor() {
    _i=0
    until eval "$1" >/dev/null 2>&1; do
        _i=$((_i + 1))
        [ "$_i" -gt 150 ] && fail "timed out waiting for ${2:-$1}"
        sleep 0.1
    done
}

# The command lines of running processes (on Windows: of every fake agent
# started so far), one per line.
proc_args() {
    if [ -n "$NQ_WIN" ]; then
        cat "$FAKE_ARGS_LOG" 2>/dev/null
    else
        ps -ax -o args=
    fi
}

# The current directory as nuq spells it.
here() {
    if [ -n "$NQ_WIN" ]; then
        pwd -W
    else
        pwd -P
    fi
}

step() {
    printf '  %s\n' "$*"
}
