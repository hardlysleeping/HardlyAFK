#include "afk_environment_api.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/environment/afk_env_store.h"

struct afk_api {
    struct afk_catalog *catalog;
    struct afk_host_backend backend;
    struct afk_session session;
};

static void api_error(char *err, size_t errlen, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static void api_error(char *err, size_t errlen, const char *format, ...) {
    if (!err || errlen == 0) return;
    va_list args;
    va_start(args, format);
    vsnprintf(err, errlen, format, args);
    va_end(args);
}

int afk_api_open(const char *root_dir, const struct afk_host_backend *backend,
                 struct afk_api **out, char *err, size_t errlen) {
    if (out) *out = NULL;
    if (!root_dir || !backend || !backend->start || !backend->stop || !backend->run_command ||
        !backend->name || !*backend->name) {
        api_error(err, errlen, "a root directory and a complete backend are required");
        return AFK_ENV_ERR_INVALID;
    }
    struct afk_api *api = calloc(1, sizeof *api);
    if (!api) {
        api_error(err, errlen, "out of memory");
        return AFK_ENV_ERR_STORAGE;
    }
    int rc = afk_catalog_open(root_dir, &api->catalog, err, errlen);
    if (rc != AFK_ENV_OK) {
        free(api);
        return rc;
    }
    api->backend = *backend;
    afk_session_init(&api->session);
    *out = api;
    return AFK_ENV_OK;
}

void afk_api_close(struct afk_api *api) {
    if (!api) return;
    afk_catalog_close(api->catalog);
    free(api);
}

const char *afk_api_backend_name(const struct afk_api *api) {
    return api && api->backend.name ? api->backend.name : "";
}

enum afk_session_state afk_api_state(const struct afk_api *api) {
    return api ? api->session.state : AFK_STOPPED;
}

int afk_api_active_environment(struct afk_api *api, char out[AFK_ENV_ID_LEN + 1]) {
    if (!api || !out) return AFK_ENV_ERR_INVALID;
    return afk_catalog_active(api->catalog, out);
}

int afk_api_list(struct afk_api *api, afk_env_visit_fn visit, void *context) {
    if (!api) return AFK_ENV_ERR_INVALID;
    return afk_catalog_list(api->catalog, visit, context);
}

int afk_api_create_environment(struct afk_api *api, const struct afk_env_spec *spec,
                               struct afk_env_record *out, char *err, size_t errlen) {
    if (!api || !spec) {
        api_error(err, errlen, "api and spec are required");
        return AFK_ENV_ERR_INVALID;
    }
    return afk_store_create(api->catalog, spec, out, err, errlen);
}

int afk_api_install_environment(struct afk_api *api, const struct afk_distro *distro,
                                const char *archive_path, const struct afk_unpacker *unpacker,
                                const struct afk_env_spec *spec,
                                struct afk_env_record *out, char *err, size_t errlen) {
    if (out) memset(out, 0, sizeof *out);
    if (!api || !distro || !archive_path || !*archive_path || !unpacker || !unpacker->unpack ||
        !spec) {
        api_error(err, errlen, "an api, distribution, archive, unpacker and spec are required");
        return AFK_ENV_ERR_INVALID;
    }
    /* Installing writes a whole rootfs; refuse while a guest is running rather
     * than competing with it for memory and storage on a phone. */
    if (api->session.state != AFK_STOPPED) {
        api_error(err, errlen, "stop the running environment before installing %s",
                  distro->display_name);
        return AFK_SESSION_BUSY;
    }
    if (strcmp(spec->architecture, distro->architecture) != 0) {
        api_error(err, errlen, "%s %s is built for %s, not %s", distro->display_name,
                  distro->version, distro->architecture, spec->architecture);
        return AFK_ENV_ERR_INVALID;
    }

    /* Nothing is created until the bytes are the bytes we pinned. */
    int rc = afk_distro_verify_archive(distro, archive_path, NULL, 0, err, errlen);
    if (rc != AFK_DISTRO_OK) return rc;

    struct afk_env_record record;
    rc = afk_store_create(api->catalog, spec, &record, err, errlen);
    if (rc != AFK_ENV_OK) return rc;

    char root[AFK_ENV_PATH_MAX];
    if (afk_catalog_resolve_root(api->catalog, &record, root, sizeof root) != AFK_ENV_OK) {
        afk_store_delete(api->catalog, record.id, NULL, 0);
        api_error(err, errlen, "environment path is not usable");
        return AFK_ENV_ERR_INVALID;
    }
    /* The distribution unpacks into rootfs/, next to the environment's own
     * metadata, so the guest never sees the catalog's files. */
    char rootfs[AFK_ENV_PATH_MAX];
    int written = snprintf(rootfs, sizeof rootfs, "%s/rootfs", root);
    if (written < 0 || (size_t) written >= sizeof rootfs) {
        afk_store_delete(api->catalog, record.id, NULL, 0);
        api_error(err, errlen, "environment path is too long");
        return AFK_ENV_ERR_INVALID;
    }

    char unpack_error[256] = {0};
    if (unpacker->unpack(unpacker->context, archive_path, rootfs, unpack_error,
                         sizeof unpack_error) != 0) {
        /* A half-unpacked system must not stay in the list as if it worked. */
        afk_store_delete(api->catalog, record.id, NULL, 0);
        api_error(err, errlen, "unpacking %s %s failed: %s", distro->display_name,
                  distro->version, unpack_error[0] ? unpack_error : "no detail from unpacker");
        return AFK_API_ERR_BACKEND;
    }

    if (out) *out = record;
    return AFK_ENV_OK;
}

int afk_api_delete_environment(struct afk_api *api, const char *id, char *err, size_t errlen) {
    if (!api || !afk_env_validate_uuid(id)) {
        api_error(err, errlen, "invalid environment id");
        return AFK_ENV_ERR_INVALID;
    }
    if (api->session.state != AFK_STOPPED && strcmp(api->session.environment_id, id) == 0) {
        api_error(err, errlen, "the running environment cannot be deleted");
        return AFK_ENV_ERR_ACTIVE;
    }
    return afk_store_delete(api->catalog, id, err, errlen);
}

/* Resolves the record and its on-disk root, or reports why it cannot. */
static int resolve(struct afk_api *api, const char *id, struct afk_env_record *env,
                   char *root, size_t rootlen, char *err, size_t errlen) {
    if (afk_catalog_get(api->catalog, id, env) != AFK_ENV_OK) {
        api_error(err, errlen, "environment not found");
        return AFK_ENV_ERR_NOT_FOUND;
    }
    if (afk_catalog_resolve_root(api->catalog, env, root, rootlen) != AFK_ENV_OK) {
        api_error(err, errlen, "environment path is not usable");
        return AFK_ENV_ERR_INVALID;
    }
    return AFK_ENV_OK;
}

static int backend_start(struct afk_api *api, struct afk_env_record *env, const char *root,
                         char *err, size_t errlen) {
    int rc = api->backend.start(api->backend.context, env, root, err, errlen);
    if (rc != AFK_ENV_OK && err && errlen && err[0] == '\0')
        api_error(err, errlen, "the runtime refused to start the environment");
    return rc;
}

int afk_api_start(struct afk_api *api, const char *id, char *err, size_t errlen) {
    if (!api || !afk_env_validate_uuid(id)) {
        api_error(err, errlen, "invalid environment id");
        return AFK_ENV_ERR_INVALID;
    }
    if (api->session.state != AFK_STOPPED) {
        api_error(err, errlen, "an environment is already %s",
                  afk_session_state_name(api->session.state));
        return AFK_SESSION_BUSY;
    }
    struct afk_env_record env;
    char root[AFK_ENV_PATH_MAX];
    int rc = resolve(api, id, &env, root, sizeof root, err, errlen);
    if (rc != AFK_ENV_OK) return rc;

    if (afk_session_prepare_start(&api->session, id) != AFK_SESSION_OK) {
        api_error(err, errlen, "cannot begin starting the environment");
        return AFK_SESSION_WRONG_STATE;
    }
    if (backend_start(api, &env, root, err, errlen) != AFK_ENV_OK) {
        afk_session_fail(&api->session);
        return AFK_API_ERR_BACKEND;
    }
    if (afk_session_mark_running(&api->session) != AFK_SESSION_OK) {
        char ignored[128] = {0};
        (void) api->backend.stop(api->backend.context, ignored, sizeof ignored);
        afk_session_fail(&api->session);
        api_error(err, errlen, "session state rejected a successful start");
        return AFK_SESSION_WRONG_STATE;
    }
    /* Only a running environment is recorded as active. */
    if (afk_catalog_set_active(api->catalog, id, err, errlen) != AFK_ENV_OK) {
        char ignored[128] = {0};
        (void) api->backend.stop(api->backend.context, ignored, sizeof ignored);
        afk_session_mark_stopped(&api->session);
        api_error(err, errlen, "cannot record the active environment");
        return AFK_ENV_ERR_STORAGE;
    }
    return AFK_ENV_OK;
}

static int flush_backend(struct afk_api *api, char *err, size_t errlen) {
    if (!api->backend.flush) return AFK_ENV_OK;
    int rc = api->backend.flush(api->backend.context, err, errlen);
    if (rc != AFK_ENV_OK) {
        if (err && errlen && err[0] == '\0')
            api_error(err, errlen, "the runtime could not flush the filesystem");
        return AFK_API_ERR_BACKEND;
    }
    return AFK_ENV_OK;
}

int afk_api_suspend(struct afk_api *api, char *err, size_t errlen) {
    if (!api) return AFK_ENV_ERR_INVALID;
    if (afk_session_prepare_suspend(&api->session) != AFK_SESSION_OK) {
        api_error(err, errlen, "only a running environment can be suspended (now %s)",
                  afk_session_state_name(api->session.state));
        return AFK_SESSION_WRONG_STATE;
    }
    /* The state machine stays in Suspending until the filesystem is flushed,
     * so a crash here is visible rather than silently losing metadata. */
    if (flush_backend(api, err, errlen) != AFK_ENV_OK) {
        afk_session_fail(&api->session);
        return AFK_API_ERR_BACKEND;
    }
    if (afk_session_mark_suspended(&api->session) != AFK_SESSION_OK) {
        api_error(err, errlen, "cannot record the suspended state");
        return AFK_SESSION_WRONG_STATE;
    }
    return AFK_ENV_OK;
}

int afk_api_resume(struct afk_api *api, char *err, size_t errlen) {
    if (!api) return AFK_ENV_ERR_INVALID;
    if (api->session.state != AFK_SUSPENDED) {
        api_error(err, errlen, "only a suspended environment can be resumed (now %s)",
                  afk_session_state_name(api->session.state));
        return AFK_SESSION_WRONG_STATE;
    }
    struct afk_env_record env;
    char root[AFK_ENV_PATH_MAX];
    int rc = resolve(api, api->session.environment_id, &env, root, sizeof root, err, errlen);
    if (rc != AFK_ENV_OK) {
        afk_session_fail(&api->session);
        return rc;
    }
    if (afk_session_prepare_resume(&api->session) != AFK_SESSION_OK) {
        api_error(err, errlen, "cannot resume the environment");
        return AFK_SESSION_WRONG_STATE;
    }
    if (backend_start(api, &env, root, err, errlen) != AFK_ENV_OK) {
        afk_session_fail(&api->session);
        return AFK_API_ERR_BACKEND;
    }
    if (afk_session_mark_running(&api->session) != AFK_SESSION_OK) {
        char ignored[128] = {0};
        (void) api->backend.stop(api->backend.context, ignored, sizeof ignored);
        afk_session_fail(&api->session);
        api_error(err, errlen, "session state rejected a successful resume");
        return AFK_SESSION_WRONG_STATE;
    }
    return AFK_ENV_OK;
}

int afk_api_stop(struct afk_api *api, char *err, size_t errlen) {
    if (!api) return AFK_ENV_ERR_INVALID;
    switch (api->session.state) {
    case AFK_STOPPED:
        api_error(err, errlen, "nothing is running");
        return AFK_SESSION_WRONG_STATE;
    case AFK_STARTING:
    case AFK_SUSPENDING:
        api_error(err, errlen, "the environment is %s; wait for the transition",
                  afk_session_state_name(api->session.state));
        return AFK_SESSION_WRONG_STATE;
    case AFK_RUNNING:
        if (afk_session_prepare_suspend(&api->session) != AFK_SESSION_OK) {
            api_error(err, errlen, "cannot begin stopping the environment");
            return AFK_SESSION_WRONG_STATE;
        }
        if (flush_backend(api, err, errlen) != AFK_ENV_OK) {
            afk_session_fail(&api->session);
            return AFK_API_ERR_BACKEND;
        }
        if (api->backend.stop(api->backend.context, err, errlen) != AFK_ENV_OK) {
            afk_session_fail(&api->session);
            if (err && errlen && err[0] == '\0')
                api_error(err, errlen, "the runtime refused to stop");
            return AFK_API_ERR_BACKEND;
        }
        break;
    case AFK_SUSPENDED:
    case AFK_FAILED:
        /* No guest is running; the backend is already quiescent. */
        break;
    }
    if (afk_session_mark_stopped(&api->session) != AFK_SESSION_OK) {
        api_error(err, errlen, "cannot record the stopped state");
        return AFK_SESSION_WRONG_STATE;
    }
    return AFK_ENV_OK;
}

int afk_api_run_command(struct afk_api *api, const char *id, const char *const *argv, int argc,
                        afk_output_sink sink, void *sink_context, char *err, size_t errlen) {
    if (!api || !afk_env_validate_uuid(id) || !argv || argc <= 0) {
        api_error(err, errlen, "a valid environment, argv and argc are required");
        return AFK_ENV_ERR_INVALID;
    }
    if (api->session.state != AFK_RUNNING) {
        api_error(err, errlen, "no environment is running (now %s)",
                  afk_session_state_name(api->session.state));
        return AFK_SESSION_WRONG_STATE;
    }
    if (strcmp(api->session.environment_id, id) != 0) {
        api_error(err, errlen, "that environment is not the running one");
        return AFK_ENV_ERR_INVALID;
    }
    int rc = api->backend.run_command(api->backend.context, argv, argc, sink, sink_context,
                                      err, errlen);
    if (rc != AFK_ENV_OK) {
        if (err && errlen && err[0] == '\0')
            api_error(err, errlen, "the runtime could not run the command");
        return AFK_API_ERR_BACKEND;
    }
    return AFK_ENV_OK;
}
