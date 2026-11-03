# Distribution catalog

`afk_distro_catalog.h` lists the root filesystems the installer may create, each with the
SHA-256 of the exact bytes that will be unpacked. The core never downloads: the app fetches, the
core verifies, and only a verified archive reaches the unpacker.

| id | version | arch | source | status |
|---|---|---|---|---|
| alpine | 3.24 | aarch64 | direct HTTPS tarball | supported, pinned |
| debian | 13 | aarch64 | OCI layer, `library/debian:trixie-slim` | supported, pinned |
| arch | latest | aarch64 | — | pin-required |

## Why Debian is an OCI layer

Debian publishes no canonical aarch64 minirootfs tarball. The `cdimage` `.tar.xz` files that
look like one actually contain a single member, `disk.raw` — checked, not assumed — and
debootstrap output is not bit-reproducible. The official debuerreotype-built OCI image is the
only Debian-published aarch64 artifact that *is* a root filesystem tarball: the arm64 layer of
`debian:trixie-slim` is the filesystem itself, and its digest stays valid at
`registry-1.docker.io` even after the tag moves.

Pinned after inspecting the real layer: 30,213,496 bytes, 3,266 entries, `usr/bin/apt`,
`usr/bin/dpkg`, `usr/bin/bash`, `usr/sbin/useradd`, `etc/os-release`, and `usr/bin/dash` with
ELF `e_machine` 183 (AArch64).

Because the blob URL needs a registry token, `oci_repository`, `oci_reference` and
`oci_manifest_digest` are carried in the catalog and fetching happens in the app's HTTP stack.
The core contract is unchanged: bytes plus a digest.

`arch` stays `pin-required` with no URL, so the UI can list it honestly while
`afk_distro_verify_archive()` refuses to touch it:

```
Arch Linux latest has no pinned checksum; refusing to install an unverified archive
```

## Why SHA-256 lives in the core

`afk_sha256` is a portable FIPS 180-4 implementation used only for integrity. Depending on
CommonCrypto on Apple and OpenSSL on Linux would put a platform difference in the one place that
must behave identically on every host, and would make the verification path untestable from a
plain `cc` invocation. ~150 lines validated against published vectors, both real archives, and
coreutils' own `sha256sum` in CI is the smaller risk.

It guards against a corrupted or substituted download, not an attacker who can edit the pins in
the binary — that is what code signing and review are for.

## Result bands

`AFK_DISTRO_ERR_*` occupies -20..-23 so it cannot be confused with the environment codes
(-1..-5) or the API codes (-6..-9) when a caller switches on one result.

## Test

```sh
cc -std=c11 -Wall -Wextra -Werror -I. core/distro/afk_sha256.c \
  core/distro/afk_distro_catalog.c tests/host/afk_distro_test.c -o /tmp/afk-distro-test
/tmp/afk-distro-test alpine-minirootfs-3.24.2-aarch64.tar.gz debian-layer.tar.gz
```

Asserts five published SHA-256 vectors (including the million-'a' message streamed in 1000-byte
chunks), strict lookups, refusal of the unpinned `arch` entry before any file is opened, both
real archives matching their pins, and a one-bit-flipped copy of each rejected with both digests
reported.

CI checks both downloads with coreutils `sha256sum` before running our verifier, so a pin that
rots upstream fails loudly instead of two copies of our own code agreeing with each other. The
Debian pin is deliberately brittle there: if the tag moves, CI fails and a human re-pins.
