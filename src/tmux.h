#ifndef NQ_TMUX_H
#define NQ_TMUX_H

#include <stddef.h>

#include "util.h"

/* Every tmux call goes through here and is exec'd with an argv array: no
 * shell is involved, so nothing needs quoting. $NUQ_TMUX_SOCKET selects a
 * separate tmux server (`tmux -L <name>`); the tests use it for isolation. */

/* Runs `tmux <args...>`; the argument list ends with NULL. Returns the exit
 * status. `out` (optional) receives stdout and stderr. */
int tmux(sb_t *out, const char *in, ...) NQ_SENTINEL;
int tmux_argv(av_t *args, const char *in, sb_t *out);

/* Installed and at least 3.2 (needed for per-pane options)? */
int tmux_check(char *err, size_t errlen);
int tmux_has_session(const char *session);
/* Pane ids of the session, one per line. Returns -1 if the session is gone. */
int tmux_list_panes(const char *session, sb_t *out);

/* Visible text of a pane. */
int tmux_capture(const char *pane, sb_t *out);
/* Bracketed paste of `text` into the pane; no Enter. */
int tmux_paste(const char *pane, const char *text);
int tmux_enter(const char *pane);

int tmux_set_label(const char *pane, const char *label);
/* Arranges the session's panes. The window holding main_pane gets the
 * "main-vertical" layout: main_pane on the left, full height, half the
 * width, the other panes stacked on the right. Any other (overflow) window
 * is tiled. main_pane == NULL keeps the first window's first pane as main.
 * Returns -1 and changes nothing while a window is zoomed (an agent taken
 * over): a layout would undo the zoom, so the caller retries later. */
int tmux_layout(const char *session, const char *main_pane);
/* Most panes the main window takes before new agents go to another window:
 * the right column needs a few rows per agent to be usable. */
int tmux_main_window_full(const char *window);

/* Makes F12 and Ctrl-q detach a client from the grid session, so whoever
 * took over an agent gets back to the nuq UI (or their shell). Uses a key
 * table only this session selects; other sessions and the global key
 * bindings are untouched. Safe to call repeatedly. (psmux can only keep
 * that promise around an attach: see tmux_takeover in tmux.c.) */
void tmux_grid_keys(const char *session);

/* Is this process running in a pane of `session` (on the grid's server)? */
int tmux_inside_session(const char *session);

/* Shows the grid in this terminal until the user presses F12 or Ctrl-q.
 * With a pane, that pane is selected and zoomed to fill the window, and
 * unzoomed again afterwards. Returns tmux's exit status. */
int tmux_takeover(const char *session, const char *pane);

#endif
