# Shared by the integration tests. Sourced, not run.
#
# The tests are executed by bash and by zsh (and bus.sh by sh as well), so
# everything here sticks to syntax that means the same in all three: every
# expansion is quoted and nothing relies on word splitting.
#
# Expects in the environment: NQ (path to the nuq binary).

set -u

: "${NQ:?NQ must point at the nuq binary}"

# A short path: unix socket addresses are limited to about 104 bytes.
T=$(mktemp -d /tmp/nqt.XXXXXX)
PROJ="$T/proj"
mkdir -p "$PROJ" "$T/run"
chmod 700 "$T/run"

export XDG_RUNTIME_DIR="$T/run"
export XDG_STATE_HOME="$T/state"
export XDG_CONFIG_HOME="$T/config"
export NUQ_TMUX_SOCKET="nqt$$"
export FAKE_AGENT_LOG="$T/agent.log"
unset NUQ_DIR NUQ_AGENT TMUX TMUX_PANE

cd "$PROJ" || exit 1

cleanup() {
    "$NQ" down >/dev/null 2>&1
    tmux -L "$NUQ_TMUX_SOCKET" kill-server >/dev/null 2>&1
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

step() {
    printf '  %s\n' "$*"
}
