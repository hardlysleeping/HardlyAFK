#include "afk_distro_catalog.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "afk_sha256.h"

/*
 * Pins are the full lower-case SHA-256 of the archive named by url.
 *
 * Alpine is pinned and verified against the file actually downloaded from
 * dl-cdn.alpinelinux.org. Debian has no canonical aarch64 minirootfs tarball
 * with a stable published digest, so its entry stays pin-required until a
 * specific artifact is chosen and recorded here; the installer must refuse to
 * download it until then rather than installing something unverified.
 */
static const struct afk_distro kDistros[] = {
    {
        .id = "alpine",
        .display_name = "Alpine",
        .version = "3.24",
        .architecture = "aarch64",
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
        .url = NULL,
        .sha256 = NULL,
        .rootfs_hint = "Debian 13 (trixie) aarch64 rootfs, glibc, apt",
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
