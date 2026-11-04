# G10 shared-folder host boundary (host only)

`afk_shared_file` is a small POSIX C boundary for a future guest adapter whose
fixed guest mount name is `/mnt/shared` (`AFK_SHARED_GUEST_MOUNT`). It does not
mount anything, translate guest absolute paths, integrate a file provider, or
provide UI. The host explicitly opens an approved directory and supplies its fd;
there is no default host directory, catalog lookup, or guest-to-host string
concatenation. Choose a separate approved directory per environment and retain
that association in the host. Passing the same root deliberately shares data.
The borrowed root fd remains caller-owned; each returned file fd must be closed.

## Contract

Paths are nonempty relative POSIX paths. Absolute paths, empty components,
trailing slashes, `.` and `..` components are rejected before touching files.
Each intermediate component is opened with `openat`, `O_DIRECTORY` and
`O_NOFOLLOW`; the final component also uses `O_NOFOLLOW`. All symlinks are refused,
including links pointing inside the approved directory. Creation is exclusively
`O_CREAT|O_EXCL`, mode 0600 subject to umask; existing entries are never replaced.
Parents must already exist. Read, write and read/write opens require an existing
regular file. Write starts at offset zero and never truncates. No append,
truncate, directory creation, delete, rename, arbitrary flags or permission mode
is exposed. Operation selectors are not bit flags. Results use a dedicated band:
0 success, -50 invalid argument/path/operation, -51 filesystem/allocation failure,
-52 nonregular final entry. Symlink refusal can return -51 or -52 depending on
where it occurs; errno is not an API contract. On failure the output fd is -1.

Opening existing special nodes is prechecked with `fstatat(AT_SYMLINK_NOFOLLOW)`;
`fstat` revalidates the opened fd. `O_NONBLOCK` prevents FIFO replacement hangs
and is cleared before returning a regular fd. No destructive open flags are
used before validation. Post-create failures can leave the new empty file: this
is not a transactional write API, and writes have normal POSIX partial-write and
crash-durability semantics. The caller owns read/write error handling and fsync.

## Trust boundary and limits

The host must approve and open the root securely (including its ancestors),
keep descriptors valid for the duration of calls, and prevent untrusted host
actors from moving opened directories outside the approved tree or installing
special devices/mounts during operations. Descriptor walks resist symlink
replacement but do not enforce ancestry after a concurrent directory rename.
This is not a sandbox against a hostile host filesystem or a privileged guest.
Only expose this bounded file API to the guest, never the host root descriptor.

**Hardlinks are not distinguished from regular files.** An approved tree must
not contain hardlinks to sensitive files outside it; host policy must control
hardlink creation and tree population. Mount points are not excluded either.
O_NOFOLLOW does not solve either problem. Root approval and per-environment
association are explicit host obligations, not claims of automatic isolation.

Requires POSIX.1-2008 `openat`, `fstatat`, `O_NOFOLLOW`, `O_DIRECTORY`, `O_CLOEXEC`
and `AT_SYMLINK_NOFOLLOW` (Linux and modern Darwin). No Linux-only `openat2`,
`/proc` or path-resolution fallback is used; missing primitives must fail to
compile, never silently weaken the boundary. Linux GCC host tests are evidence
only for this layer. Darwin compilation, iOS sandbox/security-scoped access,
guest adapter/mount and physical iPhone 17 Pro Max / iOS 26 remain unverified.

## Reproduce

Use a private scratch TMPDIR (tests require it and remove synthetic fixtures):

```sh
cc -std=c11 -Wall -Wextra -Werror -pedantic -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer -I. \
  core/shared/afk_shared_file.c tests/host/afk_shared_file_test.c \
  -o "$TMPDIR/afk-shared-test"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$TMPDIR/afk-shared-test"
```

CI's dedicated `shared-file-boundary` job exercises real files, separate approved
environment roots, traversal and intermediate/final symlinks, special-node refusal,
invalid operations, descriptor ownership and exclusive-create non-clobbering.
