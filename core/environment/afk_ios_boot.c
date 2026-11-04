#define _POSIX_C_SOURCE 200809L
#include "afk_ios_boot.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sqlite3.h>

int afk_ios_boot_root(struct afk_catalog *catalog, const char *id,
                      char out[AFK_ENV_PATH_MAX], char *err, size_t errlen) {
    struct afk_env_record env;
    char base[AFK_ENV_PATH_MAX], path[AFK_ENV_PATH_MAX];
    struct stat st;
    if (!out) goto invalid;
    out[0] = '\0';
    if (!catalog || !afk_env_validate_uuid(id)) goto invalid;
    if (afk_catalog_get(catalog, id, &env) != AFK_ENV_OK ||
        strcmp(env.architecture, "aarch64") || strcmp(env.default_user, "root") ||
        strcmp(env.default_session, "terminal") ||
        afk_catalog_resolve_root(catalog, &env, base, sizeof(base)) != AFK_ENV_OK)
        goto invalid;
    char canonical[AFK_ENV_RELPATH_LEN + 1];
    snprintf(canonical, sizeof(canonical), "environments/%s", env.id);
    if (strcmp(env.relative_path, canonical)) goto invalid;
    /* Reject symlink components under the trusted app-private store. */
    strcpy(path, base);
    char *slash = strrchr(path, '/');
    if (!slash) goto invalid;
    *slash = '\0';
    if (lstat(path, &st) || !S_ISDIR(st.st_mode)) goto invalid;
    if (lstat(base, &st) || !S_ISDIR(st.st_mode)) goto invalid;
    int n = snprintf(path, sizeof(path), "%s/rootfs", base);
    if (n < 0 || n >= (int)sizeof(path) || lstat(path, &st) || !S_ISDIR(st.st_mode)) goto invalid;
    char root[AFK_ENV_PATH_MAX];
    strcpy(root, path);
    n = snprintf(path, sizeof(path), "%s/data", root);
    if (n < 0 || n >= (int)sizeof(path) || lstat(path, &st) || !S_ISDIR(st.st_mode)) goto invalid;
    n = snprintf(path, sizeof(path), "%s/meta.db", root);
    if (n < 0 || n >= (int)sizeof(path) || lstat(path, &st) || !S_ISREG(st.st_mode)) goto invalid;
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int valid = 0;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK &&
        sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW)
        valid = sqlite3_column_int(stmt, 0) == 3;
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (valid) {
        valid = sqlite3_prepare_v2(db,
            "SELECT meta.db_inode, paths.path, paths.inode, stats.stat FROM meta, paths JOIN stats ON paths.inode=stats.inode LIMIT 0",
            -1, &stmt, NULL) == SQLITE_OK;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    if (!valid) goto invalid;
    strcpy(out, root);
    return AFK_ENV_OK;
invalid:
    if (err && errlen) snprintf(err, errlen, "Requires an existing aarch64 root/terminal environment with fakefs v3 meta.db and data; no files were migrated.");
    return AFK_ENV_ERR_INVALID;
}
