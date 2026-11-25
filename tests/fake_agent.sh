#!/bin/sh
# Stands in for a coding agent in the tests. It shows a prompt and appends
# every line typed (or pasted) into its pane to $FAKE_AGENT_LOG.
#
#   FAKE_AGENT_LOG     file that receives the lines (required)
#   FAKE_AGENT_DIALOG  if set, behave like an agent showing a permission
#                      dialog: the terminal does not echo, so pasted text
#                      never appears on screen and must not be submitted.
: "${FAKE_AGENT_LOG:?FAKE_AGENT_LOG must be set}"

if [ -n "${FAKE_AGENT_DIALOG:-}" ]; then
    stty -echo
    printf 'Allow this command? [y/N] '
else
    printf 'fake agent %s ready in %s\n> ' "${NUQ_AGENT:-?}" "$PWD"
fi

while IFS= read -r line; do
    printf '%s\n' "$line" >>"$FAKE_AGENT_LOG"
    [ "$line" = "/exit" ] && exit 0
    printf '> '
done
