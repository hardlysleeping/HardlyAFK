#define _POSIX_C_SOURCE 200809L
#include "afk_managed_install.h"
#include "core/environment/afk_env_store.h"
#include "core/environment/afk_ios_boot.h"
#include <archive.h>
#include <archive_entry.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Match the runtime importer's normalized-path buffer, including leading '/'.
 * Reject rather than skip traversal, absolute paths and oversized names. */
static int safe_path(const char *path) {
    if (!path || !*path || *path == '/' || strlen(path) >= 4094) return 0;
    for (const char *p = path; *p;) {
        const char *end = strchr(p, '/');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n == 2 && p[0] == '.' && p[1] == '.') return 0;
        p += n;
        if (*p) p++;
    }
    return 1;
}

/* Full decoding before creating anything: a pin is necessary, but does not
 * replace structural validation. No host extraction occurs here. Limits bound
 * this deliberately small Alpine installer, not a generic backup importer. */
static int preflight(const char *path, char *err, size_t errlen) {
    struct archive *a = archive_read_new();
    if (!a) return AFK_ENV_ERR_STORAGE;
    archive_read_support_filter_gzip(a);
    archive_read_support_format_tar(a);
    int ok = archive_read_open_filename(a, path, 65536) == ARCHIVE_OK;
    struct archive_entry *entry;
    int rc = ARCHIVE_FATAL;
    unsigned count = 0;
    uint64_t bytes = 0;
    char buffer[65536];
    while (ok && (rc = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        const char *hardlink = archive_entry_hardlink(entry);
        int type = archive_entry_filetype(entry);
        if (++count > 100000 || !safe_path(archive_entry_pathname(entry)) ||
            (hardlink && !safe_path(hardlink)) ||
            (!hardlink && type != AE_IFREG && type != AE_IFDIR && type != AE_IFLNK)) {
            ok = 0;
            break;
        }
        /* Symlinks in the guest are ordinary data in fakefs, never host links. */
        la_ssize_t n;
        while ((n = archive_read_data(a, buffer, sizeof buffer)) > 0) {
            bytes += (uint64_t)n;
            if (bytes > 512ULL * 1024 * 1024) { ok = 0; break; }
        }
        if (n < 0) ok = 0;
    }
    ok = ok && rc == ARCHIVE_EOF && count > 0;
    if (!ok && err && errlen)
        snprintf(err, errlen, "Archive is corrupt, unsafe or exceeds the small-rootfs limits");
    archive_read_free(a);
    return ok ? AFK_ENV_OK : AFK_ENV_ERR_INVALID;
}

int afk_managed_install(struct afk_catalog *catalog, const struct afk_distro *distro,
                        const char *archive, const char *name, struct progress progress,
                        struct afk_env_record *out, char *err, size_t errlen) {
    if (out) memset(out, 0, sizeof *out);
    struct stat st;
    if (!catalog || !distro || !archive || !name || !*name ||
        strcmp(distro->architecture, "aarch64") || lstat(archive, &st) || !S_ISREG(st.st_mode)) {
        if (err && errlen) snprintf(err, errlen, "Requires an immutable regular ARM64 rootfs archive and a name");
        return AFK_ENV_ERR_INVALID;
    }
    int rc = afk_distro_verify_archive(distro, archive, NULL, 0, err, errlen);
    if (rc) return rc;
    rc = preflight(archive, err, errlen);
    if (rc) return rc;
    struct afk_env_spec spec = {
        .name = name, .distro_id = distro->id, .distro_version = distro->version,
        .architecture = "aarch64", .default_user = "root", .default_session = "terminal",
    };
    struct afk_env_record record;
    rc = afk_store_create(catalog, &spec, &record, err, errlen);
    if (rc) return rc;
    char base[AFK_ENV_PATH_MAX], root[AFK_ENV_PATH_MAX];
    rc = afk_catalog_resolve_root(catalog, &record, base, sizeof base);
    int n = rc ? -1 : snprintf(root, sizeof root, "%s/rootfs", base);
    if (n < 0 || n >= (int)sizeof root || rmdir(root)) {
        if (err && errlen) snprintf(err, errlen, "Cannot prepare empty fakefs destination");
        goto rollback;
    }
    struct fakefsify_error failure = {0};
    if (!fakefs_import(archive, root, &failure, progress)) {
        if (err && errlen) snprintf(err, errlen, "Rootfs conversion failed: %s", failure.message ?: "unknown error");
        free(failure.message);
        goto rollback;
    }
    rc = afk_ios_boot_root(catalog, record.id, root, err, errlen);
    if (rc) goto rollback;
    if (out) *out = record;
    return AFK_ENV_OK;
rollback: {
    char cleanup[256] = {0};
    int deleted = afk_store_delete(catalog, record.id, cleanup, sizeof cleanup);
    if (deleted && err && errlen)
        snprintf(err, errlen, "Installation failed; cleanup also failed for %s: %s", record.id, cleanup);
    return AFK_ENV_ERR_STORAGE;
}
}
