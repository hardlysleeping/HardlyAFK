#include <assert.h>
#include <string.h>
#include "core/session/afk_session_state.h"

static const char *const first = "123e4567-e89b-42d3-a456-426614174000";
static const char *const second = "123e4567-e89b-42d3-a456-426614174001";

int main(void) {
    struct afk_session s;
    afk_session_init(&s);
    assert(s.state == AFK_STOPPED && s.environment_id[0] == '\0');
    assert(afk_session_prepare_start(&s, "../bad") == AFK_SESSION_INVALID);
    assert(afk_session_prepare_start(&s, first) == AFK_SESSION_OK);
    assert(s.state == AFK_STARTING && strcmp(s.environment_id, first) == 0);
    assert(afk_session_prepare_start(&s, second) == AFK_SESSION_BUSY);
    assert(afk_session_prepare_suspend(&s) == AFK_SESSION_WRONG_STATE);
    assert(afk_session_mark_running(&s) == AFK_SESSION_OK);
    assert(afk_session_prepare_suspend(&s) == AFK_SESSION_OK);
    assert(s.state == AFK_SUSPENDING);
    assert(afk_session_mark_suspended(&s) == AFK_SESSION_OK);
    assert(afk_session_prepare_resume(&s) == AFK_SESSION_OK);
    assert(s.state == AFK_STARTING && strcmp(s.environment_id, first) == 0);
    assert(afk_session_mark_running(&s) == AFK_SESSION_OK);
    assert(afk_session_mark_stopped(&s) == AFK_SESSION_OK);
    assert(s.state == AFK_STOPPED && s.environment_id[0] == '\0');
    assert(afk_session_prepare_start(&s, second) == AFK_SESSION_OK);
    assert(afk_session_fail(&s) == AFK_SESSION_OK);
    assert(s.state == AFK_FAILED && strcmp(s.environment_id, second) == 0);
    assert(afk_session_prepare_start(&s, first) == AFK_SESSION_BUSY);
    assert(afk_session_mark_stopped(&s) == AFK_SESSION_OK);
    assert(afk_session_prepare_start(&s, first) == AFK_SESSION_OK);
    assert(afk_session_fail(&s) == AFK_SESSION_OK);
    assert(strcmp(afk_session_state_name(AFK_FAILED), "Failed") == 0);
    assert(afk_session_mark_stopped(&s) == AFK_SESSION_OK);
    assert(afk_session_mark_stopped(&s) == AFK_SESSION_WRONG_STATE);
    return 0;
}
