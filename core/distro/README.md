# Distribution catalog

`afk_distro_catalog.h` lists the root filesystems the installer is allowed to fetch, and each
one carries the SHA-256 it must match. Nothing is downloaded by the core: the app fetches the
URL, then hands the file here for verification before it is unpacked.

| id | version | arch | status |
|---|---|---|---|
| alpine | 3.24 | aarch64 | supported, pinned |
| debian | 13 | aarch64 | pin-required |

`pin-required` is a deliberate state, not a placeholder to be filled with a guess. Debian
publishes no canonical aarch64 minirootfs tarball with a stable digest, so the entry exists so
the UI can list it honestly while `afk_distro_verify_archive()` refuses to touch it:

```
alpine 3.24 has no pinned checksum; refusing to install an unverified archive
```

When a specific Debian artifact is chosen, its URL and full digest go into
`core/distro/afk_distro_catalog.c` and its status becomes `supported`. A distribution never
becomes installable by leaving the field empty.

## Why SHA-256 lives in the core

`afk_sha256` is a portable FIPS 180-4 implementation used only for integrity. Depending on
CommonCrypto on Apple and OpenSSL on Linux would put a platform difference in the one place
that must behave identically on every host, and would make the verification path untestable
from a plain `cc` invocation. Shipping ~150 lines validated against published vectors, the real
4 MB Alpine archive, and coreutils' own `sha256sum` in CI is the smaller risk.

It is an integrity check against a corrupted or substituted download, not a defence against an
attacker who can edit the pins in the binary — that is what code signing and review are for.

## Test

```sh
cc -std=c11 -Wall -Wextra -Werror -I. core/distro/afk_sha256.c \
  core/distro/afk_distro_catalog.c tests/host/afk_distro_test.c -o /tmp/afk-distro-test
/tmp/afk-distro-test /path/to/alpine-minirootfs-3.24.2-aarch64.tar.gz
```

The test asserts five published SHA-256 vectors (including the million-'a' message streamed in
1000-byte chunks), that an unpinned distribution is refused before any file is opened, that the
real archive matches its pin, that a copy with one flipped bit is rejected with both digests
reported, and that a missing file surfaces as an IO error rather than a mismatch.

CI additionally checks the downloaded archive with `sha256sum` *before* running our verifier, so
a pin that silently rots upstream fails loudly instead of being "verified" by agreement between
two copies of our own code.
