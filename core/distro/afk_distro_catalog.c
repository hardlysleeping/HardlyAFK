#include "afk_distro_catalog.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "afk_sha256.h"

/*
 * Pins are the SHA-256 of the exact bytes the installer will unpack.
 *
 * Alpine ships a minirootfs tarball, so url + sha256 are enough.
 *
 * Debian publishes no canonical aarch64 minirootfs tarball: the cdimage .tar.xz
 * files wrap a disk image (verified: the archive contains a single member,
 * disk.raw), and debootstrap output is not bit-reproducible. Debian therefore
 * comes from the official debuerreotype-built OCI image, where the arm64 layer
 * of debian:trixie-slim *is* the root filesystem and its digest is immutable at
 * registry-1.docker.io even though the tag keeps moving.
 *
 * Verified at pin time:
 *   debian trixie-slim arm64 layer  sha256 2b70e040...
 *     30213496 bytes, 3266 entries, usr/bin/apt, usr/bin/dpkg, usr/bin/bash,
 *     usr/sbin/useradd, etc/os-release, usr/bin/dash e_machine 183 (AArch64).
 *   alpine 3.24 minirootfs         sha256 9bf70a7f...
 *     4028030 bytes, digest also matches dl-cdn and coreutils in CI.
 */
static const struct afk_distro kDistros[] = {
    {
        .id = "alpine",
        .display_name = "Alpine",
        .version = "3.24",
        .architecture = "aarch64",
        .source = AFK_SOURCE_DIRECT_URL,
        .url = "https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/aarch64/"
               "alpine-minirootfs-3.24.2-aarch64.tar.gz",
        .sha256 = "9bf70a7f18ea44094cbb5f70c58f9af129c8214745743db0e68e5502cc2ce773",
        .rootfs_hint = "Alpine minirootfs, musl, BusyBox",
        .status = AFK_DISTRO_SUPPORTED,
    },
    {
        .id = "debian",
        .display_name = "Debian",
        .version = "13",
        .architecture = "aarch64",
        .source = AFK_SOURCE_OCI_BLOB,
        .oci_repository = "library/debian",
        .oci_reference = "trixie-slim",
        .oci_manifest_digest = "sha256:1470991b497d26788dc4d6f438bf5690c6ba0d499235d237e55f4b43caea7a75",
        .sha256 = "2b70e040f7d132c8d366ee0223462552686510fb372ea5a83822973c30f19e36",
        .rootfs_hint = "Debian 13 (trixie) slim rootfs, glibc, apt, dpkg",
        .status = AFK_DISTRO_SUPPORTED,
    },
    {
        .id = "arch",
        .display_name = "Arch Linux",
        .version = "latest",
        .architecture = "aarch64",
        .source = AFK_SOURCE_DIRECT_URL,
        .url = NULL,
        .sha256 = NULL,
        .rootfs_hint = "Arch Linux ARM aarch64 rootfs, pacman",
        .status = AFK_DISTRO_PIN_REQUIRED,
    },
};

const struct afk_distro *afk_distro_all(size_t *count) {
    if (count) *count = sizeof kDistros / sizeof kDistros[0];
    return kDistros;
}

const struct afk_distro *afk_distro_find(const char *id, const char *version) {
    if (!id || !*id) return NULL;
    size_t count = 0;
    const struct afk_distro *all = afk_distro_all(&count);
    for (size_t i = 0; i < count; i++) {
        if (strcmp(all[i].id, id) != 0) continue;
        if (version && *version && strcmp(all[i].version, version) != 0) continue;
        return &all[i];
    }
    return NULL;
}

const char *afk_distro_status_name(enum afk_distro_status status) {
    switch (status) {
    case AFK_DISTRO_SUPPORTED: return "supported";
    case AFK_DISTRO_PIN_REQUIRED: return "pin-required";
    case AFK_DISTRO_EXPERIMENTAL: return "experimental";
    default: return "unknown";
    }
}

/* Case-insensitive compare of two 64-character hex digests. */
static int digests_equal(const char *a, const char *b) {
    if (strlen(a) != 64 || strlen(b) != 64) return 0;
    for (int i = 0; i < 64; i++)
        if (tolower((unsigned char) a[i]) != tolower((unsigned char) b[i])) return 0;
    return 1;
}

int afk_distro_verify_archive(const struct afk_distro *distro, const char *archive_path,
                              char *actual_hex, size_t actual_hex_len,
                              char *err, size_t errlen) {
    if (!distro || !archive_path) {
        if (err && errlen) snprintf(err, errlen, "a distribution and an archive are required");
        return AFK_DISTRO_ERR_NOT_FOUND;
    }
    if (!distro->sha256 || strlen(distro->sha256) != 64) {
        if (err && errlen)
            snprintf(err, errlen,
                     "%s %s has no pinned checksum; refusing to install an unverified archive",
                     distro->display_name, distro->version);
        return AFK_DISTRO_ERR_UNPINNED;
    }

    char computed[65];
    char io_error[256] = {0};
    if (afk_sha256_file_hex(archive_path, computed, io_error, sizeof io_error) != 0) {
        if (err && errlen) snprintf(err, errlen, "%s", io_error);
        return AFK_DISTRO_ERR_IO;
    }
    if (actual_hex && actual_hex_len > 64) {
        memcpy(actual_hex, computed, 65);
    }
    if (!digests_equal(computed, distro->sha256)) {
        if (err && errlen)
            snprintf(err, errlen,
                     "checksum mismatch for %s %s: expected %.64s, got %.64s",
                     distro->display_name, distro->version, distro->sha256, computed);
        return AFK_DISTRO_ERR_DIGEST_MISMATCH;
    }
    return AFK_DISTRO_OK;
}
