# Environment API

`afk_environment_api.h` is the only surface the application above it needs. The UI, the
environment list, the session lifecycle and command execution all go through it; none of them
touch fakefs, the emulator or the kernel.

The runtime is supplied as a backend:

```c
struct afk_host_backend {
    const char *name;
    void *context;
    int (*start)(ctx, const struct afk_env_record *env, const char *root_path, err, errlen);
    int (*stop)(ctx, err, errlen);
    int (*flush)(ctx, err, errlen);            /* optional */
    int (*run_command)(ctx, argv, argc, sink, sink_context, err, errlen);
};
```

The Apple build supplies an iSH-backed implementation, the Linux host a native one, and a later
Android port its own. The contract, the state machine and the catalog stay identical, which is
what makes the cross-platform split real instead of aspirational.

## Guarantees this facade enforces

- **One running environment.** `afk_api_start` refuses while anything is running; the
  environment is only recorded as active after the backend accepted the boot.
- **Boot must be real.** A backend failure produces `Failed`, never a running session, and the
  error from the backend is propagated to the caller.
- **Suspend flushes first.** `afk_api_suspend` moves to `Suspending`, calls `flush`, and only
  then records `Suspended`. A refused flush leaves the session `Failed` so the UI can offer
  recovery instead of pretending the data is safe.
- **Commands need a live guest.** `afk_api_run_command` requires `Running` *and* the requested
  environment to be the running one; a different environment is rejected rather than silently
  executed in the wrong filesystem.
- **Deletion is guarded.** The running environment cannot be deleted, and the catalog's active
  marker keeps protecting it after the session stops.
- **No background promise.** The API models iOS lifecycle honestly: you may suspend and later
  resume, but nothing here claims Linux keeps executing while the app is backgrounded.

## What it does not do

It does not boot Linux on this host: the mock backend in the test suite records calls and
returns. Wiring `start` to the real runtime, `run_command` to the guest PTY, and the display
session to the viewer remain device-gated work.

## Test

```sh
cc -std=c11 -Wall -Wextra -Werror -I. core/session/afk_session_state.c \
  core/environment/afk_env_catalog.c core/environment/afk_env_store.c \
  core/api/afk_environment_api.c tests/host/afk_environment_api_test.c \
  -lsqlite3 -o /tmp/afk-api-test
/tmp/afk-api-test
```

The regression drives the whole lifecycle through a recording mock backend and asserts call
order (`flush;stop;`), not just final state: busy starts, wrong-environment commands, refused
deletes, backend start/flush/stop failures, resume of the correct environment, and the active
marker outliving the session.
