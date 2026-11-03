#define _GNU_SOURCE
#include "afk_env_store.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void store_error(char *err, size_t errlen, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static void store_error(char *err, size_t errlen, const char *format, ...) {
    if (!err || errlen == 0) return;
    va_list args;
    va_start(args, format);
    vsnprintf(err, errlen, format, args);
    va_end(args);
}

static int ensure_dir(const char *path, char *err, size_t errlen) {
    if (mkdir(path, 0700) == 0) return 0;
    if (errno == EEXIST) {
        struct stat st;
        if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
        store_error(err, errlen, "%s exists and is not a directory", path);
        return -1;
    }
    store_error(err, errlen, "cannot create %s: %s", path, strerror(errno));
    return -1;
}

/* Splits an absolute path into parent and final component. */
static int split_path(const char *path, char *parent, size_t parentlen,
                      char *leaf, size_t leaflen) {
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) return -1;
    size_t plen = (size_t) (slash - path);
    if (plen >= parentlen) return -1;
    memcpy(parent, path, plen);
    parent[plen] = '\0';
    if (strlen(slash + 1) >= leaflen) return -1;
    strcpy(leaf, slash + 1);
    return 0;
}

/* Resolves the environment directory plus an optional suffix. */
static int env_path(struct afk_catalog *catalog, const struct afk_env_record *env,
                    const char *suffix, char *out, size_t outlen,
                    char *err, size_t errlen) {
    char root[AFK_ENV_PATH_MAX];
    if (afk_catalog_resolve_root(catalog, env, root, sizeof root) != AFK_ENV_OK) {
        store_error(err, errlen, "refusing unsafe environment path");
        return -1;
    }
    int written = snprintf(out, outlen, "%s%s", root, suffix ? suffix : "");
    if (written < 0 || (size_t) written >= outlen) {
        store_error(err, errlen, "environment path is too long");
        return -1;
    }
    return 0;
}

/* Recursively removes one entry, never following a symlink out of the tree. */
static int remove_entry_at(int parent_fd, const char *name, char *err, size_t errlen) {
    if (unlinkat(parent_fd, name, 0) == 0) return 0;
    if (errno != EISDIR && errno != EPERM) {
        store_error(err, errlen, "cannot remove %s: %s", name, strerror(errno));
        return -1;
    }
    int fd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ELOOP) { /* symlink to a directory: unlink it, do not walk it */
            if (unlinkat(parent_fd, name, 0) == 0) return 0;
        }
        store_error(err, errlen, "cannot open %s: %s", name, strerror(errno));
        return -1;
    }
    DIR *dir = fdopendir(fd);
    if (!dir) {
        close(fd);
        store_error(err, errlen, "cannot read %s: %s", name, strerror(errno));
        return -1;
    }
    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (remove_entry_at(dirfd(dir), entry->d_name, err, errlen) != 0) {
            result = -1;
            break;
        }
    }
    closedir(dir); /* closes fd */
    if (result != 0) return -1;
    if (unlinkat(parent_fd, name, AT_REMOVEDIR) != 0) {
        store_error(err, errlen, "cannot remove directory %s: %s", name, strerror(errno));
        return -1;
    }
    return 0;
}

static int measure_path(const char *path, struct afk_env_usage *out) {
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        if (fd < 0) return -1;
        DIR *dir = fdopendir(fd);
        if (!dir) {
            close(fd);
            return -1;
        }
        out->directories++;
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            char child[AFK_ENV_PATH_MAX];
            int written = snprintf(child, sizeof child, "%s/%s", path, entry->d_name);
            if (written < 0 || (size_t) written >= sizeof child) continue;
            if (measure_path(child, out) != 0) {
                /* A vanished file during measurement is not a fatal error. */
                if (errno != ENOENT) { closedir(dir); return -1; }
            }
        }
        closedir(dir);
        return 0;
    }
    out->bytes += (uint64_t) (st.st_size > 0 ? st.st_size : 0);
    out->files++;
    return 0;
}

int afk_store_materialise(struct afk_catalog *catalog, const struct afk_env_record *env,
                          char *err, size_t errlen) {
    if (!catalog || !env) return AFK_ENV_ERR_INVALID;
    char root[AFK_ENV_PATH_MAX];
    char rootfs[AFK_ENV_PATH_MAX];
    if (env_path(catalog, env, "", root, sizeof root, err, errlen) != 0) return AFK_ENV_ERR_INVALID;
    if (env_path(catalog, env, "/rootfs", rootfs, sizeof rootfs, err, errlen) != 0) return AFK_ENV_ERR_INVALID;

    char parent[AFK_ENV_PATH_MAX];
    char leaf[256];
    if (split_path(root, parent, sizeof parent, leaf, sizeof leaf) != 0) {
        store_error(err, errlen, "malformed environment path");
        return AFK_ENV_ERR_INVALID;
    }
    if (ensure_dir(parent, err, errlen) != 0) return AFK_ENV_ERR_STORAGE;
    if (ensure_dir(root, err, errlen) != 0) return AFK_ENV_ERR_STORAGE;
    if (ensure_dir(rootfs, err, errlen) != 0) return AFK_ENV_ERR_STORAGE;
    return AFK_ENV_OK;
}

int afk_store_create(struct afk_catalog *catalog, const struct afk_env_spec *spec,
                     struct afk_env_record *out, char *err, size_t errlen) {
    struct afk_env_record record;
    int rc = afk_catalog_create(catalog, spec, &record, err, errlen);
    if (rc != AFK_ENV_OK) return rc;
    if (afk_store_materialise(catalog, &record, err, errlen) != AFK_ENV_OK) {
        char ignored[256] = {0};
        /* A row must never outlive a failed directory creation. */
        afk_catalog_delete(catalog, record.id, ignored, sizeof ignored);
        return AFK_ENV_ERR_STORAGE;
    }
    if (out) *out = record;
    return AFK_ENV_OK;
}

int afk_store_delete(struct afk_catalog *catalog, const char *id, char *err, size_t errlen) {
    if (!catalog || !afk_env_validate_uuid(id)) {
        store_error(err, errlen, "invalid environment id");
        return AFK_ENV_ERR_INVALID;
    }
    struct afk_env_record env;
    if (afk_catalog_get(catalog, id, &env) != AFK_ENV_OK) {
        store_error(err, errlen, "environment not found");
        return AFK_ENV_ERR_NOT_FOUND;
    }
    char active[AFK_ENV_ID_LEN + 1];
    if (afk_catalog_active(catalog, active) == AFK_ENV_OK && strcmp(active, id) == 0) {
        store_error(err, errlen, "the active environment cannot be deleted");
        return AFK_ENV_ERR_ACTIVE;
    }
    char root[AFK_ENV_PATH_MAX];
    if (env_path(catalog, &env, "", root, sizeof root, err, errlen) != 0) return AFK_ENV_ERR_INVALID;

    /* Row first: a leftover tree is recoverable, a row pointing at nothing is not. */
    int rc = afk_catalog_delete(catalog, id, err, errlen);
    if (rc != AFK_ENV_OK) return rc;

    char parent[AFK_ENV_PATH_MAX];
    char leaf[256];
    if (split_path(root, parent, sizeof parent, leaf, sizeof leaf) != 0) {
        store_error(err, errlen, "malformed environment path");
        return AFK_ENV_ERR_INVALID;
    }
    int parent_fd = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (parent_fd < 0) {
        if (errno == ENOENT) return AFK_ENV_OK; /* tree already absent */
        store_error(err, errlen, "cannot open %s: %s", parent, strerror(errno));
        return AFK_ENV_ERR_STORAGE;
    }
    int removed = remove_entry_at(parent_fd, leaf, err, errlen);
    close(parent_fd);
    return removed == 0 ? AFK_ENV_OK : AFK_ENV_ERR_STORAGE;
}

int afk_store_usage(struct afk_catalog *catalog, const char *id, struct afk_env_usage *out,
                    char *err, size_t errlen) {
    if (!catalog || !out || !afk_env_validate_uuid(id)) {
        store_error(err, errlen, "invalid arguments");
        return AFK_ENV_ERR_INVALID;
    }
    struct afk_env_record env;
    if (afk_catalog_get(catalog, id, &env) != AFK_ENV_OK) {
        store_error(err, errlen, "environment not found");
        return AFK_ENV_ERR_NOT_FOUND;
    }
    char root[AFK_ENV_PATH_MAX];
    if (env_path(catalog, &env, "", root, sizeof root, err, errlen) != 0) return AFK_ENV_ERR_INVALID;
    memset(out, 0, sizeof *out);
    if (measure_path(root, out) != 0) {
        store_error(err, errlen, "cannot measure %s: %s", root, strerror(errno));
        return AFK_ENV_ERR_STORAGE;
    }
    return AFK_ENV_OK;
}
