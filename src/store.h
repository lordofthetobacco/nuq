#ifndef NQ_STORE_H
#define NQ_STORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "cJSON.h"

/* Broker state: mailboxes, messages, subscriptions and the work queue.
 *
 * Every change is a JSON record that is applied to memory and appended to
 * the journal, so replaying the journal rebuilds the same state. */

enum { ST_UNKNOWN, ST_STARTING, ST_IDLE, ST_BUSY, ST_BLOCKED };
enum { W_NONE, W_QUEUED, W_CLAIMED, W_DONE, W_FAILED };

/* How messages reach a mailbox's owner. */
enum {
    MODE_PULL,  /* owner asks: `nuq inbox` / `nuq listen` */
    MODE_WAKE,  /* agent hooks pull; an idle agent gets a short wake line */
    MODE_PASTE, /* full messages are pasted into the agent's pane */
};

typedef struct {
    uint64_t id, ts, reply_to;
    char *from, *kind, *to, *body;
    char **rcpt;
    size_t n_rcpt;
    int wstate;
    char *claimant, *note;
} nq_msg;

typedef struct {
    char *path;
    uint64_t added, removed;
} nq_file;

typedef struct {
    char *name, *kind, *pane;
    char *token; /* identifies one run of the agent; see st_register */
    int live, mode;
    int state;
    uint64_t state_ts, reg_ts;
    uint64_t *inbox;
    size_t n_inbox, cap_inbox;
    char **subs;
    size_t n_subs;

    /* Lines changed through the agent's edit tools, since the grid began. */
    uint64_t added, removed;
    nq_file *files;
    size_t n_files;
    char *last_tool;     /* broker-only: last tool the agent ran, and when */
    uint64_t last_ts;

    /* Broker-only, never journaled: pane label and in-flight wake. */
    int label_dirty;
    uint64_t waiter_until; /* a background hook is waiting to wake the agent */
    int inj_phase;
    uint64_t inj_start, inj_stable_since, inj_retry_at;
    size_t inj_count;
    char *inj_marker, *inj_pre, *inj_last;
} nq_agent;

typedef struct {
    nq_msg **msgs;
    size_t n_msgs, cap_msgs;
    nq_agent **agents;
    size_t n_agents, cap_agents;
    FILE *journal;
} nq_store;

void st_init(nq_store *s);
void st_free(nq_store *s);
/* Applies every record in the journal. Returns the record count, -1 on error. */
int st_replay(nq_store *s, const char *path);
int st_open_journal(nq_store *s, const char *path);

/* Live mailbox by name, or NULL. */
nq_agent *st_find(const nq_store *s, const char *name);
nq_msg *st_msg(const nq_store *s, uint64_t id);

/* name == NULL picks "<kind>-<n>". With update == 0 a live name is an error,
 * except that a "cli" mailbox may be re-opened by another "cli" caller.
 *
 * token (optional) is unique to one run of an agent. Names are reused, so
 * a request that carries the token of an earlier run can be told apart
 * from the agent that holds the name now. */
nq_agent *st_register(nq_store *s, const char *name, const char *kind, const char *pane,
                      const char *token, int mode, int update, char *err, size_t errlen);
/* Does this request's token (may be NULL) belong to the current run of a? */
int st_token_ok(const nq_agent *a, const char *token);
/* Drops the mailbox and puts any work it had claimed back in the queue. */
void st_unregister(nq_store *s, const char *name);
int st_sub(nq_store *s, const char *name, const char *topic, int on);

/* kind: "dm" (to = "a,b"), "all", "topic" (to = topic) or "work".
 * Returns the new message id, or 0 with err filled in. */
uint64_t st_post(nq_store *s, const char *kind, const char *from, const char *to, const char *body,
                 uint64_t reply_to, char *err, size_t errlen);

/* Records lines an agent changed in one file. */
void st_edit(nq_store *s, const char *name, const char *file, uint64_t added, uint64_t removed);

/* Marks the first n inbox entries as delivered. */
void st_take_inbox(nq_store *s, nq_agent *a, size_t n);

/* Oldest queued task becomes a's; optionally also lands in a's inbox. */
nq_msg *st_claim(nq_store *s, nq_agent *a, int to_inbox);
nq_msg *st_claimed_by(const nq_store *s, const char *name);
int st_finish(nq_store *s, const char *name, uint64_t id, int ok, const char *note, char *err,
              size_t errlen);
size_t st_count_work(const nq_store *s, int wstate);
/* The main agent: the live agent in a pane that registered first. It keeps
 * the left half of the grid. NULL if there is none. */
nq_agent *st_main_agent(const nq_store *s);
/* The agent that has been idle longest and can take a task, or NULL. */
nq_agent *st_idle_worker(const nq_store *s);

cJSON *st_msg_json(const nq_msg *m);
const char *st_state_name(int state);
int st_state_parse(const char *s);
const char *st_wstate_name(int wstate);
const char *st_mode_name(int mode);

#endif
