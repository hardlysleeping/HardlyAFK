#include "afk_session_state.h"
#include <ctype.h>
#include <string.h>

static int valid_uuid(const char *id) {
    if (!id || strlen(id) != 36)
        return 0;
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (id[i] != '-') return 0;
        } else if (!isxdigit((unsigned char) id[i])) {
            return 0;
        }
    }
    return 1;
}

void afk_session_init(struct afk_session *session) {
    if (!session) return;
    session->state = AFK_STOPPED;
    session->environment_id[0] = '\0';
}

int afk_session_prepare_start(struct afk_session *session, const char *environment_id) {
    if (!session || !valid_uuid(environment_id)) return AFK_SESSION_INVALID;
    if (session->state != AFK_STOPPED) return AFK_SESSION_BUSY;
    memcpy(session->environment_id, environment_id, 37);
    session->state = AFK_STARTING;
    return AFK_SESSION_OK;
}

int afk_session_mark_running(struct afk_session *session) {
    if (!session) return AFK_SESSION_INVALID;
    if (session->state != AFK_STARTING) return AFK_SESSION_WRONG_STATE;
    session->state = AFK_RUNNING;
    return AFK_SESSION_OK;
}

int afk_session_prepare_suspend(struct afk_session *session) {
    if (!session) return AFK_SESSION_INVALID;
    if (session->state != AFK_RUNNING) return AFK_SESSION_WRONG_STATE;
    session->state = AFK_SUSPENDING;
    return AFK_SESSION_OK;
}

int afk_session_mark_suspended(struct afk_session *session) {
    if (!session) return AFK_SESSION_INVALID;
    if (session->state != AFK_SUSPENDING) return AFK_SESSION_WRONG_STATE;
    session->state = AFK_SUSPENDED;
    return AFK_SESSION_OK;
}

int afk_session_prepare_resume(struct afk_session *session) {
    if (!session) return AFK_SESSION_INVALID;
    if (session->state != AFK_SUSPENDED) return AFK_SESSION_WRONG_STATE;
    session->state = AFK_STARTING;
    return AFK_SESSION_OK;
}

int afk_session_mark_stopped(struct afk_session *session) {
    if (!session) return AFK_SESSION_INVALID;
    if (session->state == AFK_STOPPED) return AFK_SESSION_WRONG_STATE;
    session->state = AFK_STOPPED;
    session->environment_id[0] = '\0';
    return AFK_SESSION_OK;
}

int afk_session_fail(struct afk_session *session) {
    if (!session) return AFK_SESSION_INVALID;
    if (session->state == AFK_STOPPED || session->state == AFK_FAILED)
        return AFK_SESSION_WRONG_STATE;
    session->state = AFK_FAILED;
    return AFK_SESSION_OK;
}

const char *afk_session_state_name(enum afk_session_state state) {
    switch (state) {
    case AFK_STOPPED: return "Stopped";
    case AFK_STARTING: return "Starting";
    case AFK_RUNNING: return "Running";
    case AFK_SUSPENDING: return "Suspending";
    case AFK_SUSPENDED: return "Suspended";
    case AFK_FAILED: return "Failed";
    default: return "Unknown";
    }
}
