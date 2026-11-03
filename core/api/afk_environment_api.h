#ifndef AFK_ENVIRONMENT_API_H
#define AFK_ENVIRONMENT_API_H

/*
 * The boundary the application talks to. Everything above this header knows
 * about environments, sessions and commands; nothing above it knows how the
 * guest is executed.
 *
 * The iSH-derived runtime sits behind afk_host_backend, so the Linux host, the
 * Apple app and a later Android port can each supply their own backend without
 * changing this contract or the UI.
 */

#include <stddef.h>

#include "core/environment/afk_env_catalog.h"
#include "core/session/afk_session_state.h"

#define AFK_API_ERR_BACKEND (-6)
#define AFK_API_ERR_NO_BACKEND (-7)

typedef void (*afk_output_sink)(void *context, const char *bytes, size_t length);

/*
 * Implemented by a platform. All calls are made on the caller's thread; the
 * backend must not retain guest state after stop() returns.
 */
struct afk_host_backend {
    const char *name;
    void *context;
    /* Boots the environment whose filesystem is at root_path. */
    int (*start)(void *context, const struct afk_env_record *env, const char *root_path,
                 char *err, size_t errlen);
    /* Terminates the running guest and releases its session. */
    int (*stop)(void *context, char *err, size_t errlen);
    /* Optional: flushes filesystem state before suspend, i.e. before iOS
     * backgrounds the app. NULL means there is nothing to flush. */
    int (*flush)(void *context, char *err, size_t errlen);
    int (*run_command)(void *context, const char *const *argv, int argc,
                       afk_output_sink sink, void *sink_context,
                       char *err, size_t errlen);
};

struct afk_api;

int afk_api_open(const char *root_dir, const struct afk_host_backend *backend,
                 struct afk_api **out, char *err, size_t errlen);
void afk_api_close(struct afk_api *api);

/* Metadata and storage only; the environment is left stopped. */
int afk_api_create_environment(struct afk_api *api, const struct afk_env_spec *spec,
                               struct afk_env_record *out, char *err, size_t errlen);
/* Refused while the environment is running or active. */
int afk_api_delete_environment(struct afk_api *api, const char *id, char *err, size_t errlen);

/* Only one environment runs at a time. Starting records it as active. */
int afk_api_start(struct afk_api *api, const char *id, char *err, size_t errlen);
/* Safe from running, suspended or failed states. */
int afk_api_stop(struct afk_api *api, char *err, size_t errlen);
/* For UIApplication entering the background: flush, then leave the guest
 * suspended. There is no promise of continued execution while backgrounded. */
int afk_api_suspend(struct afk_api *api, char *err, size_t errlen);
int afk_api_resume(struct afk_api *api, char *err, size_t errlen);

/* Requires a running guest and names the environment that is running. */
int afk_api_run_command(struct afk_api *api, const char *id, const char *const *argv, int argc,
                        afk_output_sink sink, void *sink_context, char *err, size_t errlen);

enum afk_session_state afk_api_state(const struct afk_api *api);
/* "" when nothing is active. */
int afk_api_active_environment(struct afk_api *api, char out[AFK_ENV_ID_LEN + 1]);
int afk_api_list(struct afk_api *api, afk_env_visit_fn visit, void *context);
/* The backend that was attached at open time; "" never occurs. */
const char *afk_api_backend_name(const struct afk_api *api);

#endif
