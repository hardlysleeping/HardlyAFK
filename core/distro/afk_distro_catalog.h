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

enum afk_distro_source {
    /* A plain file over HTTPS. */
    AFK_SOURCE_DIRECT_URL = 0,
    /*
     * An OCI image layer. The download needs a registry token, so the app
     * fetches it (URLSession / the platform HTTP stack) and hands the file to
     * the core, which only ever sees bytes and a digest.
     */
    AFK_SOURCE_OCI_BLOB = 1
};

struct afk_distro {
    const char *id;
    const char *display_name;
    const char *version;
    const char *architecture;
    enum afk_distro_source source;
    const char *url;               /* AFK_SOURCE_DIRECT_URL only */
    const char *oci_repository;    /* e.g. "library/debian" */
    const char *oci_reference;     /* tag used to find the blob, not trusted */
    const char *oci_manifest_digest; /* manifest the layer digest was read from */
    const char *sha256;            /* pin for the downloaded bytes, or NULL */
    const char *rootfs_hint;       /* what the archive is expected to be */
    enum afk_distro_status status;
};

/*
 * Each subsystem owns a band of result codes so a caller can switch on one
 * value: environment -1..-5, API -6..-9, distribution -20..-23.
 */
enum afk_distro_result {
    AFK_DISTRO_OK = 0,
    AFK_DISTRO_ERR_NOT_FOUND = -20,
    AFK_DISTRO_ERR_UNPINNED = -21,
    AFK_DISTRO_ERR_DIGEST_MISMATCH = -22,
    AFK_DISTRO_ERR_IO = -23
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
