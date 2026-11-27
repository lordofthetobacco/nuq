#ifndef NQ_ADAPTERS_H
#define NQ_ADAPTERS_H

#include "cJSON.h"
#include "cli.h"
#include "util.h"

/* What nuq knows about each kind of agent: how its messages are
 * delivered and which extra arguments wire its hooks (for Pi, an extension)
 * to `nuq hook`.
 * Anything not listed is "generic": started as given, no hooks. */

/* MODE_* for a kind. `inject` is the --inject flag (generic agents only). */
int adapter_mode(const char *kind, int inject);

/* Appends the command line that starts the agent to av. Hook configuration
 * is passed per run (files in the grid's runtime dir, or flags), so the
 * user's own agent configuration is never edited. Strings created here are
 * also appended to `owned` for the caller to free. */
void adapter_command(const nq_ctx *c, const char *kind, const char *name, const char *self,
                     const char *agent_path, int argc, char **argv, av_t *av, av_t *owned);

/* The text that tells an agent where it is and how to use the queue.
 * `agents` is the "agents" array of an `ls` reply (may be NULL). */
void adapter_bootstrap(sb_t *out, const char *me, const cJSON *agents);

/* The trust hash Codex expects for a command hook, as "sha256:<hex>".
 * key is the event in snake case ("post_tool_use"); matcher may be NULL.
 * out must hold 72 bytes. Exposed for the tests. */
void adapter_codex_hook_hash(const char *key, const char *command, int timeout, const char *matcher,
                             char *out);

/* Appends s single-quoted for /bin/sh. */
void sb_add_shq(sb_t *sb, const char *s);

#endif
