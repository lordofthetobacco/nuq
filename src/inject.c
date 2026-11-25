#include "inject.h"

#include <stdlib.h>
#include <string.h>

#include "tmux.h"
#include "util.h"

#define STABLE_MS 150
#define TIMEOUT_MS 4000

static void clear(nq_agent *a)
{
    a->inj_phase = 0;
    free(a->inj_marker);
    free(a->inj_pre);
    free(a->inj_last);
    a->inj_marker = a->inj_pre = a->inj_last = NULL;
}

void inject_start(nq_agent *a, const char *text, const char *marker, size_t count, uint64_t now)
{
    clear(a);
    sb_t pre = {0};
    tmux_capture(a->pane, &pre);
    if (tmux_paste(a->pane, text) < 0) {
        sb_free(&pre);
        a->inj_retry_at = now + 5000;
        return;
    }
    a->inj_phase = 1;
    a->inj_start = now;
    a->inj_stable_since = now;
    a->inj_count = count;
    a->inj_marker = xstrdup(marker);
    a->inj_pre = pre.p;
}

int inject_tick(nq_agent *a, uint64_t now)
{
    if (!a->inj_phase) return INJ_NONE;

    sb_t cur = {0};
    if (tmux_capture(a->pane, &cur) == 0 && cur.p) {
        /* Agents collapse long pastes into a "[Pasted text ...]" placeholder,
         * so the marker itself may never be displayed. */
        int placeholder = strstr(cur.p, "[Pasted ") && (!a->inj_pre || strcmp(cur.p, a->inj_pre));
        if (strstr(cur.p, a->inj_marker) || placeholder) {
            if (a->inj_last && !strcmp(a->inj_last, cur.p)) {
                if (now - a->inj_stable_since >= STABLE_MS) {
                    sb_free(&cur);
                    int rc = tmux_enter(a->pane);
                    clear(a);
                    return rc == 0 ? INJ_DONE : INJ_FAILED;
                }
            } else {
                free(a->inj_last);
                a->inj_last = cur.p;
                a->inj_stable_since = now;
                cur.p = NULL;
            }
        }
    }
    sb_free(&cur);

    if (now - a->inj_start > TIMEOUT_MS) {
        clear(a);
        return INJ_FAILED;
    }
    return INJ_PENDING;
}
