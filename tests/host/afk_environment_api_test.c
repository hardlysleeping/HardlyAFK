/* Host regression for the runtime-facing API. Build with -lsqlite3. */
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "core/api/afk_environment_api.h"

struct mock {
    char log[2048];
    char output[256];
    int fail_start;
    int fail_stop;
    int fail_flush;
    int starts;
    int stops;
    int flushes;
    int commands;
};

static void note(struct mock *m, const char *format, ...) {
    va_list args;
    va_start(args, format);
    size_t used = strlen(m->log);
    vsnprintf(m->log + used, sizeof m->log - used, format, args);
    va_end(args);
    if (used < sizeof m->log - 1) strcat(m->log, ";");
}

static int mock_start(void *ctx, const struct afk_env_record *env, const char *root_path,
                      char *err, size_t errlen) {
    struct mock *m = ctx;
    struct stat st;
    assert(stat(root_path, &st) == 0 && S_ISDIR(st.st_mode)); /* the facade passes a real root */
    m->starts++;
    note(m, "start:%s", env->name);
    if (m->fail_start) {
        snprintf(err, errlen, "mock start refused");
        return AFK_ENV_ERR_STORAGE;
    }
    return AFK_ENV_OK;
}

static int mock_stop(void *ctx, char *err, size_t errlen) {
    struct mock *m = ctx;
    m->stops++;
    note(m, "stop");
    if (m->fail_stop) {
        snprintf(err, errlen, "mock stop refused");
        return AFK_ENV_ERR_STORAGE;
    }
    return AFK_ENV_OK;
}

static int mock_flush(void *ctx, char *err, size_t errlen) {
    struct mock *m = ctx;
    m->flushes++;
    note(m, "flush");
    if (m->fail_flush) {
        snprintf(err, errlen, "mock flush refused");
        return AFK_ENV_ERR_STORAGE;
    }
    return AFK_ENV_OK;
}

static void mock_sink(void *ctx, const char *bytes, size_t length) {
    struct mock *m = ctx;
    size_t used = strlen(m->output);
    if (used + length >= sizeof m->output) length = sizeof m->output - used - 1;
    memcpy(m->output + used, bytes, length);
    m->output[used + length] = '\0';
}

static int mock_run(void *ctx, const char *const *argv, int argc, afk_output_sink sink,
                    void *sink_ctx, char *err, size_t errlen) {
    (void) err;
    (void) errlen;
    struct mock *m = ctx;
    m->commands++;
    note(m, "run:%s(argc=%d)", argv[0], argc);
    if (sink) sink(sink_ctx, "hello from the mock guest", 25);
    return AFK_ENV_OK;
}

static const struct afk_host_backend backend = {
    .name = "mock",
    .start = mock_start,
    .stop = mock_stop,
    .flush = mock_flush,
    .run_command = mock_run,
};

static void temp_dir(char *out, size_t len) {
    const char *base = getenv("TMPDIR");
    snprintf(out, len, "%s/afk-api-XXXXXX", base ? base : "/tmp");
    assert(mkdtemp(out) != NULL);
}

static const struct afk_env_spec debian = {
    .name = "Debian Work", .distro_id = "debian", .distro_version = "13",
    .architecture = "aarch64", .default_user = "hardly",
    .default_session = AFK_ENV_SESSION_TERMINAL, .created_at = 1700000000,
};
static const struct afk_env_spec alpine = {
    .name = "Alpine Test", .distro_id = "alpine", .distro_version = "3.24",
    .architecture = "aarch64", .default_user = "root",
    .default_session = AFK_ENV_SESSION_DESKTOP, .created_at = 1700000100,
};

int main(void) {
    char dir[512];
    char err[512] = {0};
    temp_dir(dir, sizeof dir);

    struct mock mock;
    memset(&mock, 0, sizeof mock);

    /* An incomplete backend is rejected before anything is created. */
    struct afk_host_backend broken = backend;
    broken.run_command = NULL;
    struct afk_api *rejected = NULL;
    assert(afk_api_open(dir, &broken, &rejected, err, sizeof err) == AFK_ENV_ERR_INVALID);
    assert(rejected == NULL);

    struct afk_host_backend bound = backend;
    bound.context = &mock;
    struct afk_api *api = NULL;
    assert(afk_api_open(dir, &bound, &api, err, sizeof err) == AFK_ENV_OK);
    assert(strcmp(afk_api_backend_name(api), "mock") == 0);
    assert(afk_api_state(api) == AFK_STOPPED);

    struct afk_env_record a, b;
    assert(afk_api_create_environment(api, &debian, &a, err, sizeof err) == AFK_ENV_OK);
    assert(afk_api_create_environment(api, &alpine, &b, err, sizeof err) == AFK_ENV_OK);
    assert(afk_api_state(api) == AFK_STOPPED);
    assert(mock.starts == 0);

    /* Commands require a running guest that is the requested environment. */
    const char *argv[] = {"/bin/sh", "-c", "echo hi"};
    assert(afk_api_run_command(api, a.id, argv, 3, mock_sink, &mock, err, sizeof err) ==
           AFK_SESSION_WRONG_STATE);

    char unknown[AFK_ENV_ID_LEN + 1];
    snprintf(unknown, sizeof unknown, "00000000-0000-4000-8000-000000000000");
    assert(afk_api_start(api, unknown, err, sizeof err) == AFK_ENV_ERR_NOT_FOUND);
    assert(afk_api_state(api) == AFK_STOPPED);

    /* Start, run, and refuse a second environment. */
    assert(afk_api_start(api, a.id, err, sizeof err) == AFK_ENV_OK);
    assert(afk_api_state(api) == AFK_RUNNING);
    assert(mock.starts == 1);
    assert(strstr(mock.log, "start:Debian Work") != NULL);
    char active[AFK_ENV_ID_LEN + 1];
    assert(afk_api_active_environment(api, active) == AFK_ENV_OK);
    assert(strcmp(active, a.id) == 0);
    assert(afk_api_start(api, b.id, err, sizeof err) == AFK_SESSION_BUSY);
    assert(mock.starts == 1);
    assert(afk_api_run_command(api, b.id, argv, 3, mock_sink, &mock, err, sizeof err) ==
           AFK_ENV_ERR_INVALID);
    assert(mock.commands == 0);
    assert(afk_api_run_command(api, a.id, argv, 3, mock_sink, &mock, err, sizeof err) == AFK_ENV_OK);
    assert(mock.commands == 1);
    assert(strcmp(mock.output, "hello from the mock guest") == 0);
    assert(strstr(mock.log, "run:/bin/sh(argc=3)") != NULL);

    /* Delete is refused while it is the running environment. */
    assert(afk_api_delete_environment(api, a.id, err, sizeof err) == AFK_ENV_ERR_ACTIVE);

    /* Background: flush happens before the suspended state is recorded. */
    memset(mock.log, 0, sizeof mock.log);
    assert(afk_api_suspend(api, err, sizeof err) == AFK_ENV_OK);
    assert(afk_api_state(api) == AFK_SUSPENDED);
    assert(mock.flushes == 1);
    assert(strcmp(mock.log, "flush;") == 0);
    assert(afk_api_suspend(api, err, sizeof err) == AFK_SESSION_WRONG_STATE);
    assert(afk_api_run_command(api, a.id, argv, 3, mock_sink, &mock, err, sizeof err) ==
           AFK_SESSION_WRONG_STATE);

    /* Resume restarts the same environment, not the first in the list. */
    assert(afk_api_start(api, b.id, err, sizeof err) == AFK_SESSION_BUSY);
    assert(afk_api_resume(api, err, sizeof err) == AFK_ENV_OK);
    assert(afk_api_state(api) == AFK_RUNNING);
    assert(mock.starts == 2);

    /* Stop flushes, stops the backend, then clears the session. */
    memset(mock.log, 0, sizeof mock.log);
    assert(afk_api_stop(api, err, sizeof err) == AFK_ENV_OK);
    assert(afk_api_state(api) == AFK_STOPPED);
    assert(strcmp(mock.log, "flush;stop;") == 0);
    assert(afk_api_stop(api, err, sizeof err) == AFK_SESSION_WRONG_STATE);

    /* The active marker outlives the session, so the environment stays protected. */
    assert(afk_api_active_environment(api, active) == AFK_ENV_OK);
    assert(strcmp(active, a.id) == 0);
    assert(afk_api_delete_environment(api, a.id, err, sizeof err) == AFK_ENV_ERR_ACTIVE);
    assert(afk_api_delete_environment(api, b.id, err, sizeof err) == AFK_ENV_OK);

    /* A backend failure is surfaced as FAILED, never as a running session. */
    mock.fail_start = 1;
    assert(afk_api_start(api, a.id, err, sizeof err) == AFK_API_ERR_BACKEND);
    assert(afk_api_state(api) == AFK_FAILED);
    assert(strstr(err, "mock start refused") != NULL);
    assert(afk_api_run_command(api, a.id, argv, 3, mock_sink, &mock, err, sizeof err) ==
           AFK_SESSION_WRONG_STATE);
    assert(afk_api_stop(api, err, sizeof err) == AFK_ENV_OK);
    assert(afk_api_state(api) == AFK_STOPPED);

    /* A refused flush leaves the session FAILED rather than pretending to suspend. */
    mock.fail_start = 0;
    mock.fail_flush = 1;
    assert(afk_api_start(api, a.id, err, sizeof err) == AFK_ENV_OK);
    assert(afk_api_suspend(api, err, sizeof err) == AFK_API_ERR_BACKEND);
    assert(afk_api_state(api) == AFK_FAILED);
    mock.fail_flush = 0;
    assert(afk_api_stop(api, err, sizeof err) == AFK_ENV_OK);

    /* A refused stop also ends in FAILED so the UI can offer recovery. */
    mock.fail_stop = 1;
    assert(afk_api_start(api, a.id, err, sizeof err) == AFK_ENV_OK);
    assert(afk_api_stop(api, err, sizeof err) == AFK_API_ERR_BACKEND);
    assert(afk_api_state(api) == AFK_FAILED);
    mock.fail_stop = 0;
    assert(afk_api_stop(api, err, sizeof err) == AFK_ENV_OK);

    afk_api_close(api);
    printf("env-api-test: PASS (starts=%d stops=%d flushes=%d commands=%d)\n",
           mock.starts, mock.stops, mock.flushes, mock.commands);
    return 0;
}
