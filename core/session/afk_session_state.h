#ifndef HARDLYAFK_SESSION_STATE_H
#define HARDLYAFK_SESSION_STATE_H

/* Host-owned lifecycle contract. This module does not start or stop guest code. */

enum afk_session_state {
    AFK_STOPPED,
    AFK_STARTING,
    AFK_RUNNING,
    AFK_SUSPENDING,
    AFK_SUSPENDED,
    AFK_FAILED
};

enum afk_session_result {
    AFK_SESSION_OK = 0,
    AFK_SESSION_INVALID = -1,
    AFK_SESSION_BUSY = -2,
    AFK_SESSION_WRONG_STATE = -3
};

struct afk_session {
    enum afk_session_state state;
    char environment_id[37]; /* UUID string; empty when stopped. */
};

void afk_session_init(struct afk_session *session);
/* A caller starts the guest only AFTER prepare_start succeeds, then calls mark_running or fail. */
int afk_session_prepare_start(struct afk_session *session, const char *environment_id);
int afk_session_mark_running(struct afk_session *session);
int afk_session_prepare_suspend(struct afk_session *session);
int afk_session_mark_suspended(struct afk_session *session);
int afk_session_prepare_resume(struct afk_session *session);
/* Call mark_stopped only after the guest has exited and filesystem has been flushed. */
int afk_session_mark_stopped(struct afk_session *session);
int afk_session_fail(struct afk_session *session);
const char *afk_session_state_name(enum afk_session_state state);

#endif
