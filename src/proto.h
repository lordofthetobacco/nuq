#ifndef NQ_PROTO_H
#define NQ_PROTO_H

#include <stddef.h>

#include "cJSON.h"
#include "util.h"

/* How messages look to the agents and people who read them. Input is the
 * JSON form produced by st_msg_json(). */

/* The block an agent sees:
 *   <nuq id="42" from="bob" kind="dm">
 *   body
 *   </nuq>
 * Bodies longer than max_body (0 = no limit) are cut, with a pointer to
 * `nuq show <id>`. Work-queue tasks get a line on how to finish them. */
void proto_fmt_msg(sb_t *out, const cJSON *msg, size_t max_body);

/* Formats every message in the array, separated by blank lines. */
void proto_fmt_msgs(sb_t *out, const cJSON *msgs, size_t max_body);

/* One line for `nuq tail` and `nuq work`: "#42 bob -> alice: first line". */
void proto_fmt_line(sb_t *out, const cJSON *msg, size_t max_body);

/* Lines an agent's tool call added and removed, per file, worked out from
 * the PostToolUse hook input (tool name, tool_input, tool_response).
 * Understands Claude Code's Write/Edit/MultiEdit (using its structuredPatch
 * when present) and Codex's apply_patch. Appends {"file","added","removed"}
 * objects to `out` and returns how many; 0 for tools that edit nothing. */
int proto_edit_stats(const char *tool, const cJSON *input, const cJSON *response, cJSON *out);

#endif
