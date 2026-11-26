#ifndef NQ_CLI_H
#define NQ_CLI_H

#include "cJSON.h"
#include "paths.h"
#include "util.h"

typedef struct {
    nq_paths p;
    const char *me; /* $NUQ_AGENT inside a pane, otherwise $USER */
} nq_ctx;

/* Options that come before the agent name: nuq [-n NAME] [--sub a,b] ... */
typedef struct {
    const char *name;
    const char *subs;
    int inject;
    int detach;
} nq_launch_opts;

/* Sends one request to the grid's broker. Takes ownership of req.
 * cli_rpc returns NULL if no broker answers; cli_call exits with a message
 * on any failure, including an {"ok":false} reply. */
cJSON *cli_rpc(const nq_ctx *c, cJSON *req, int timeout_ms);
cJSON *cli_call(const nq_ctx *c, cJSON *req);
cJSON *cli_req(const char *op);

/* Each takes the arguments after the command word. */
int cmd_up(nq_ctx *c, int argc, char **argv);
int cmd_nuke(nq_ctx *c, int argc, char **argv);
int cmd_send(nq_ctx *c, int argc, char **argv);
int cmd_pub(nq_ctx *c, int argc, char **argv);
int cmd_sub(nq_ctx *c, int argc, char **argv);
int cmd_unsub(nq_ctx *c, int argc, char **argv);
int cmd_push(nq_ctx *c, int argc, char **argv);
int cmd_take(nq_ctx *c, int argc, char **argv);
int cmd_done(nq_ctx *c, int argc, char **argv);
int cmd_fail(nq_ctx *c, int argc, char **argv);
int cmd_inbox(nq_ctx *c, int argc, char **argv);
int cmd_show(nq_ctx *c, int argc, char **argv);
int cmd_listen(nq_ctx *c, int argc, char **argv);
int cmd_ls(nq_ctx *c, int argc, char **argv);
int cmd_tail(nq_ctx *c, int argc, char **argv);
int cmd_work(nq_ctx *c, int argc, char **argv);
int cmd_kill(nq_ctx *c, int argc, char **argv);
int cmd_attach(nq_ctx *c, int argc, char **argv);

/* Event kinds, for colouring. */
enum { EV_DM, EV_ALL, EV_TOPIC, EV_WORK, EV_STATE, EV_GRID, EV_EDIT };
/* Formats one event frame from the broker's PUB socket as a line, without
 * a timestamp. Returns the parsed event (caller frees) or NULL. */
cJSON *cli_fmt_event(const char *frame, sb_t *out, int *kind);

/* dash.c */
int cmd_dash(nq_ctx *c, int argc, char **argv);

/* extras.c */
int cmd_doctor(nq_ctx *c, int argc, char **argv);
int cmd_completion(nq_ctx *c, int argc, char **argv);
int cmd_skills(nq_ctx *c, int argc, char **argv);

/* launch.c, pane.c, hook.c */
int cmd_launch(nq_ctx *c, const nq_launch_opts *o, int argc, char **argv);
int cmd_layout(nq_ctx *c, int argc, char **argv);
int cmd_pane(nq_ctx *c, int argc, char **argv);
int cmd_hook(nq_ctx *c, int argc, char **argv);

#endif
