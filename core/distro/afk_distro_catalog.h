#ifndef AFK_DISTRO_CATALOG_H
#define AFK_DISTRO_CATALOG_H

/*
 * Supported root filesystems, with the pin each one must be installed against.
 *
 * A distribution is only installable when its URL and SHA-256 are both known:
 * an entry marked pin-required exists so the UI can list it honestly without
 * ever downloading something unverifiable.
 */

#include <stddef.h>

enum afk_distro_status {
    AFK_DISTRO_SUPPORTED = 0,
    AFK_DISTRO_PIN_REQUIRED = 1,
    AFK_DISTRO_EXPERIMENTAL = 2
};

struct afk_distro {
    const char *id;
    const char *display_name;
    const char *version;
    const char *architecture;
    const char *url;
    const char *sha256; /* NULL while pin-required */
    const char *rootfs_hint; /* what the archive is expected to be */
    enum afk_distro_status status;
};

enum afk_distro_result {
    AFK_DISTRO_OK = 0,
    AFK_DISTRO_ERR_NOT_FOUND = -1,
    AFK_DISTRO_ERR_UNPINNED = -2,
    AFK_DISTRO_ERR_DIGEST_MISMATCH = -3,
    AFK_DISTRO_ERR_IO = -4
};

const struct afk_distro *afk_distro_all(size_t *count);
/* Matches id case-sensitively and version exactly; NULL version means "any". */
const struct afk_distro *afk_distro_find(const char *id, const char *version);
const char *afk_distro_status_name(enum afk_distro_status status);

/*
 * Streams the archive and compares its digest to the pin. actual_hex may be
 * NULL; when supplied it receives the digest that was computed, so the UI can
 * show what was rejected instead of only that it was rejected.
 */
int afk_distro_verify_archive(const struct afk_distro *distro, const char *archive_path,
                              char *actual_hex, size_t actual_hex_len,
                              char *err, size_t errlen);

#endif
