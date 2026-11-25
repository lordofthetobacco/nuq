#ifndef NQ_INJECT_H
#define NQ_INJECT_H

#include <stdint.h>

#include "store.h"

/* Typing into an agent's pane, safely.
 *
 * Pressing Enter in a pane that shows a permission dialog would approve it.
 * So text is pasted first (bracketed paste, which dialogs ignore), and Enter
 * is sent only after the pasted text has been seen sitting on the screen,
 * unchanged, for a moment. If it never shows up, nothing is submitted. */

enum { INJ_NONE, INJ_PENDING, INJ_DONE, INJ_FAILED };

/* Pastes `text` into a's pane and starts watching for `marker`, a short
 * string unique to this paste that appears at the start of the text. */
void inject_start(nq_agent *a, const char *text, const char *marker, size_t count, uint64_t now);

/* Call every ~50 ms while a->inj_phase is set. */
int inject_tick(nq_agent *a, uint64_t now);

#endif
