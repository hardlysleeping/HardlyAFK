# Environment catalog

`afk_env_catalog.h` stores the metadata that makes several Linux environments independent
and recoverable. One SQLite catalog lives in the app container (`<catalog>/catalog.db`);
each environment owns a directory beneath `environments/<uuid>/` where its fakefs lives.

SQLite is used deliberately: the app, the file provider and fakefs already depend on it, and
metadata updated while a guest is running needs transactional writes rather than a
hand-rolled JSON rewrite per change.

## What this layer guarantees

- **Stable identity.** IDs are UUIDv4 strings generated from `/dev/urandom`; names are display
  labels and can change without moving a single file.
- **Relocatable storage.** Only the relative path (`environments/<uuid>`) is persisted. iOS
  rewrites the data container path on reinstall, so storing absolute paths would silently
  orphan every environment.
- **Validated names.** Empty, over-long, control-character, separator-bearing (`/`, `\`),
  `:`-bearing (resource forks), leading/trailing-space, `.` and `..` names are rejected; names
  are unique case-insensitively.
- **One active environment.** The active selection is recorded separately from the rows, so
  switching environments never rewrites metadata, and the active environment cannot be deleted
  while it is mounted.
- **Per-environment desktop installs.** `env_desktops` is keyed on the environment ID and
  cascades on delete, so a broken KDE install is removable without touching another system.

## What this layer does NOT do

It does not create, mount or run a guest. Materialising the fakefs at
`afk_catalog_resolve_root()`, creating the user, and wiring the state machine in
`core/session` to the iSH runtime are host responsibilities that require the Apple build and
a physical device. A passing host test here is not evidence that Linux boots.

## Build and test

Requires SQLite development headers (`libsqlite3-dev` on Debian/Ubuntu, `sqlite` on Arch):

```sh
cc -std=c11 -Wall -Wextra -Werror -I. core/environment/afk_env_catalog.c \
  tests/host/afk_env_catalog_test.c -lsqlite3 -o /tmp/env-catalog-test
/tmp/env-catalog-test
```

The regression covers creation order, duplicate and invalid names, UUID uniqueness,
case-insensitive rename collisions, active-environment deletion protection, desktop
add/remove/dedupe, reopening a catalog, cascade cleanup, and rejection of traversal and
absolute relative paths. CI runs the same suite under ASan/UBSan.
