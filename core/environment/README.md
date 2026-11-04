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

`afk_env_store` does create and remove the directory tree
(`environments/<uuid>/rootfs`) and measure its usage, but it does not populate that
directory with a fakefs, create a user, or run a guest. Wiring the state machine in
`core/session` to the iSH runtime is a host responsibility that requires the Apple build
and a physical device. A passing host test here is not evidence that Linux boots.

Deletion is deliberately conservative: it walks with `openat(..., O_NOFOLLOW)` so a symlink
planted inside an environment cannot redirect the removal outside the tree, and it removes
metadata before the tree so a crash leaves an orphaned directory rather than a row pointing
at nothing.

## Command-line proof

`tools/afk-envctl` exposes exactly the calls the Apple UI will make, so the multi-environment
behaviour can be exercised on any machine:

```sh
cc -std=c11 -Wall -Wextra -Werror -I. core/environment/afk_env_catalog.c \
  core/environment/afk_env_store.c tools/afk-envctl.c -lsqlite3 -o /tmp/afk-envctl

/tmp/afk-envctl --root /tmp/afk create --name "Debian A" --distro debian --version 13
/tmp/afk-envctl --root /tmp/afk list
/tmp/afk-envctl --root /tmp/afk activate <id>
/tmp/afk-envctl --root /tmp/afk delete <id>
```

`tests/host/afk_envctl_test.sh` runs the acceptance shape from the plan end to end: two
environments created, files isolated between them, a restart re-reading the same catalog,
rename without moving data, the active environment refusing deletion, and independent
deletion of the others.

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

## G10 bounded host increment: offline store bundles

`tools/afk-store-backup.py` supports entire offline stores and individual-environment
bundles (not an iOS share-sheet archive). Whole-store commands:

```sh
python3 tools/afk-store-backup.py export /absolute/store /absolute/new-bundle --quiescent
python3 tools/afk-store-backup.py import /absolute/new-bundle /absolute/new-store --quiescent
```

Both destinations must be absent. Stop the runtime **and every file-provider/writer**
before either operation; `--quiescent` is an operator assertion, not a runtime lock.
Use a private source/destination parent with serialized access. These are trusted local
filesystem operations, not a hardened hostile-archive importer or concurrent-writer API.
Same-parent staging avoids publishing partial copies on ordinary failures; there is no
fsync/crash-durability guarantee or atomic no-replace guarantee against racing publishers.

The bundle preserves the catalog (including desktop/active metadata), complete environment
trees, host modes, hardlinks and symlink targets without following symlinks. SQLite's backup
API includes committed WAL content in standalone database snapshots; raw WAL/SHM sidecars
are not copied. SQLite reads can update ephemeral SHM/locking state on the source.
SHA-256 inventory checking detects accidental corruption, not malicious manifest edits.
Unsupported host special nodes and orphan environment directories are refused. Guest
ownership, permissions and device types remain in the unmodified fakefs stat blobs.
Host ownership/ACLs/resource forks and cross-platform extended metadata are not promised.

This deliberately requires `rootfs/meta.db` plus `rootfs/data/` (fakefs schema v3).
A plain distribution directory produced by today's generic installer is refused: rootfs-only
copying is not a fakefs backup. Upstream `tools/fakefs.c` stores guest symlinks as regular
host files with symlink type/permissions in SQLite; both representations must travel together.
Inode identities change on restore; upstream fakefs's db-inode rebuild path must still be
exercised by the real runtime. The host fixture uses the upstream schema/stat encoding and
checks exact paths/stat blobs, contents, modes, symlinks, hardlinks, committed WAL, corruption
refusal and nonreplacement. It does **not** mount/boot a restored fakefs.

For a single environment, export requires its canonical UUID and an absent bundle path;
import requires an **existing** valid offline store and an explicit unique new name:

```sh
python3 tools/afk-store-backup.py export-env /absolute/store /absolute/env-bundle --id UUID --quiescent
python3 tools/afk-store-backup.py import-env /absolute/env-bundle /absolute/store --name 'Debian clone' --quiescent
```

Import prints a newly generated UUID, retains distro/version/architecture, creation timestamp,
default user/session and desktop metadata, and never restores the source active selection.
The complete selected environment tree travels with its fakefs database/data and host modes.
Export refuses the active source even if stopped: deselect it first. Both operations still
require all runtimes and writers stopped; running state is not persisted in the catalog and
cannot be detected here. All environments in source/destination must pass store validation.

Merge stages only the incoming tree, inserts rows under `BEGIN IMMEDIATE`, publishes the tree,
then commits; ordinary exceptions roll back metadata and remove a published new tree. Existing
trees/rows are not replaced. A process/power failure between rename and commit can leave an
orphan new UUID directory (never an existing environment overwrite); remove that orphan only
after checking the catalog. This is not a crash-atomic transaction across SQLite/filesystem.
Private directories and serialized access remain mandatory; no hostile bundles or racing
writers are supported. Inventory hashes are corruption checks, not signatures. Catalog schema
v1 and fakefs schema v3 only; host ownership/ACLs are still not promised.

CI runs `tests/host/afk_store_backup_test.py` in the environment-store job, including merge into
two synthetic existing environments, new identity/name, duplicate/missing name, quiescence,
active-source and corrupt-bundle refusals, publication-failure rollback and unchanged old trees.
No Apple app/API integration, compression, IPA or physical-device proof is provided.
G10 remains device-unproven, including on the target iPhone 17 Pro Max / iOS 26.

## G10 bounded host increment: shared files

[`../shared/README.md`](../shared/README.md) documents the descriptor-relative
shared-file boundary for the fixed future guest name `/mnt/shared`. The host
explicitly supplies an approved directory fd per environment; this is separate
from fakefs/store backup and does not automatically expose any environment tree.
Only regular-file reads/writes and exclusive creation are provided. There is no
runtime mount, UI, or device claim; trusted-root/hardlink assumptions apply.
