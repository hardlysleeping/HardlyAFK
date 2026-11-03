#include "afk_env_catalog.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

#define AFK_CATALOG_SCHEMA_VERSION 1

struct afk_catalog {
    sqlite3 *db;
    char directory[AFK_ENV_PATH_MAX];
};

static void set_error(char *err, size_t errlen, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static void set_error(char *err, size_t errlen, const char *format, ...) {
    if (!err || errlen == 0) return;
    va_list args;
    va_start(args, format);
    vsnprintf(err, errlen, format, args);
    va_end(args);
}

static void copy_str(char *dst, size_t dstlen, const char *src) {
    if (dstlen == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= dstlen) n = dstlen - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int afk_env_validate_uuid(const char *id) {
    if (!id || strlen(id) != AFK_ENV_ID_LEN) return 0;
    for (int i = 0; i < AFK_ENV_ID_LEN; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (id[i] != '-') return 0;
        } else if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

int afk_env_validate_name(const char *name) {
    if (!name) return 0;
    size_t n = strlen(name);
    if (n == 0 || n > AFK_ENV_NAME_LEN) return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    if (name[0] == ' ' || name[n - 1] == ' ') return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char) name[i];
        if (c < 0x20 || c == 0x7f) return 0;      /* control characters */
        if (c == '/' || c == '\\') return 0;      /* path separators */
        if (c == ':') return 0;                   /* resource-fork suffixes */
    }
    return 1;
}

static int fill_random(void *buffer, size_t length) {
    size_t done = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    while (done < length) {
        ssize_t r = read(fd, (char *) buffer + done, length - done);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            close(fd);
            return -1;
        }
        done += (size_t) r;
    }
    close(fd);
    return 0;
}

static int generate_uuid(char out[AFK_ENV_ID_LEN + 1]) {
    unsigned char b[16];
    if (fill_random(b, sizeof b) != 0) return -1;
    b[6] = (unsigned char) ((b[6] & 0x0f) | 0x40); /* version 4 */
    b[8] = (unsigned char) ((b[8] & 0x3f) | 0x80); /* RFC 4122 variant */
    snprintf(out, AFK_ENV_ID_LEN + 1,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return 0;
}

static const char *const kSchema =
    "PRAGMA foreign_keys = ON;"
    "CREATE TABLE IF NOT EXISTS environments ("
    "  id TEXT PRIMARY KEY NOT NULL,"
    "  name TEXT NOT NULL COLLATE NOCASE UNIQUE,"
    "  distro_id TEXT NOT NULL,"
    "  distro_version TEXT NOT NULL,"
    "  architecture TEXT NOT NULL,"
    "  relative_path TEXT NOT NULL UNIQUE,"
    "  created_at INTEGER NOT NULL,"
    "  default_user TEXT NOT NULL,"
    "  default_session TEXT NOT NULL,"
    "  CHECK (default_session IN ('terminal','desktop'))"
    ");"
    "CREATE TABLE IF NOT EXISTS env_desktops ("
    "  env_id TEXT NOT NULL REFERENCES environments(id) ON DELETE CASCADE,"
    "  desktop_id TEXT NOT NULL,"
    "  PRIMARY KEY (env_id, desktop_id)"
    ");"
    "CREATE TABLE IF NOT EXISTS catalog_meta ("
    "  key TEXT PRIMARY KEY NOT NULL,"
    "  value TEXT NOT NULL"
    ");";

static int exec_sql(sqlite3 *db, const char *sql) {
    char *message = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &message);
    if (rc != SQLITE_OK) sqlite3_free(message);
    return rc;
}

int afk_catalog_open(const char *directory, struct afk_catalog **out,
                     char *err, size_t errlen) {
    if (out) *out = NULL;
    if (!directory || !out) {
        set_error(err, errlen, "catalog directory and output are required");
        return AFK_ENV_ERR_INVALID;
    }
    if (strlen(directory) >= AFK_ENV_PATH_MAX - 16) {
        set_error(err, errlen, "catalog directory path is too long");
        return AFK_ENV_ERR_INVALID;
    }
    struct stat st;
    if (stat(directory, &st) != 0 || !S_ISDIR(st.st_mode)) {
        set_error(err, errlen, "catalog directory does not exist: %s", directory);
        return AFK_ENV_ERR_INVALID;
    }

    struct afk_catalog *catalog = calloc(1, sizeof *catalog);
    if (!catalog) {
        set_error(err, errlen, "out of memory");
        return AFK_ENV_ERR_STORAGE;
    }
    copy_str(catalog->directory, sizeof catalog->directory, directory);

    char path[AFK_ENV_PATH_MAX];
    if (snprintf(path, sizeof path, "%s/catalog.db", directory) >= (int) sizeof path) {
        free(catalog);
        set_error(err, errlen, "catalog path is too long");
        return AFK_ENV_ERR_INVALID;
    }

    if (sqlite3_open_v2(path, &catalog->db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        NULL) != SQLITE_OK) {
        set_error(err, errlen, "cannot open catalog: %s",
                  catalog->db ? sqlite3_errmsg(catalog->db) : "unknown error");
        sqlite3_close(catalog->db);
        free(catalog);
        return AFK_ENV_ERR_STORAGE;
    }
    sqlite3_busy_timeout(catalog->db, 5000);

    int version = 0;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(catalog->db, "PRAGMA user_version;", -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) {
        version = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    if (version > AFK_CATALOG_SCHEMA_VERSION) {
        set_error(err, errlen, "catalog schema %d is newer than supported version %d",
                  version, AFK_CATALOG_SCHEMA_VERSION);
        sqlite3_close(catalog->db);
        free(catalog);
        return AFK_ENV_ERR_STORAGE;
    }
    if (exec_sql(catalog->db, "PRAGMA journal_mode = WAL;") != SQLITE_OK ||
        exec_sql(catalog->db, kSchema) != SQLITE_OK ||
        exec_sql(catalog->db, "PRAGMA user_version = 1;") != SQLITE_OK) {
        set_error(err, errlen, "cannot initialise catalog schema: %s", sqlite3_errmsg(catalog->db));
        sqlite3_close(catalog->db);
        free(catalog);
        return AFK_ENV_ERR_STORAGE;
    }

    *out = catalog;
    return AFK_ENV_OK;
}

void afk_catalog_close(struct afk_catalog *catalog) {
    if (!catalog) return;
    sqlite3_close(catalog->db);
    free(catalog);
}

static void read_record(sqlite3_stmt *stmt, struct afk_env_record *out) {
    copy_str(out->id, sizeof out->id, (const char *) sqlite3_column_text(stmt, 0));
    copy_str(out->name, sizeof out->name, (const char *) sqlite3_column_text(stmt, 1));
    copy_str(out->distro_id, sizeof out->distro_id, (const char *) sqlite3_column_text(stmt, 2));
    copy_str(out->distro_version, sizeof out->distro_version, (const char *) sqlite3_column_text(stmt, 3));
    copy_str(out->architecture, sizeof out->architecture, (const char *) sqlite3_column_text(stmt, 4));
    copy_str(out->relative_path, sizeof out->relative_path, (const char *) sqlite3_column_text(stmt, 5));
    out->created_at = sqlite3_column_int64(stmt, 6);
    copy_str(out->default_user, sizeof out->default_user, (const char *) sqlite3_column_text(stmt, 7));
    copy_str(out->default_session, sizeof out->default_session, (const char *) sqlite3_column_text(stmt, 8));
}

#define kSelectColumns \
    "SELECT id, name, distro_id, distro_version, architecture, relative_path," \
    " created_at, default_user, default_session FROM environments"

int afk_catalog_create(struct afk_catalog *catalog, const struct afk_env_spec *spec,
                       struct afk_env_record *out, char *err, size_t errlen) {
    if (!catalog || !spec || !out) {
        set_error(err, errlen, "catalog, spec and output are required");
        return AFK_ENV_ERR_INVALID;
    }
    if (!afk_env_validate_name(spec->name)) {
        set_error(err, errlen, "invalid environment name");
        return AFK_ENV_ERR_INVALID;
    }
    const char *session = spec->default_session ? spec->default_session : AFK_ENV_SESSION_TERMINAL;
    if (strcmp(session, AFK_ENV_SESSION_TERMINAL) != 0 && strcmp(session, AFK_ENV_SESSION_DESKTOP) != 0) {
        set_error(err, errlen, "unknown default session: %s", session);
        return AFK_ENV_ERR_INVALID;
    }
    if (!spec->distro_id || !*spec->distro_id || strlen(spec->distro_id) > AFK_ENV_DISTRO_LEN) {
        set_error(err, errlen, "invalid distro id");
        return AFK_ENV_ERR_INVALID;
    }

    struct afk_env_record record;
    memset(&record, 0, sizeof record);
    if (generate_uuid(record.id) != 0) {
        set_error(err, errlen, "cannot read random bytes for environment id");
        return AFK_ENV_ERR_STORAGE;
    }
    copy_str(record.name, sizeof record.name, spec->name);
    copy_str(record.distro_id, sizeof record.distro_id, spec->distro_id);
    copy_str(record.distro_version, sizeof record.distro_version,
             spec->distro_version ? spec->distro_version : "");
    copy_str(record.architecture, sizeof record.architecture,
             spec->architecture ? spec->architecture : "aarch64");
    copy_str(record.default_user, sizeof record.default_user,
             spec->default_user ? spec->default_user : "root");
    copy_str(record.default_session, sizeof record.default_session, session);
    record.created_at = spec->created_at > 0 ? spec->created_at : (int64_t) time(NULL);
    snprintf(record.relative_path, sizeof record.relative_path, "environments/%s", record.id);

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(catalog->db,
                           "INSERT INTO environments"
                           " (id, name, distro_id, distro_version, architecture, relative_path,"
                           "  created_at, default_user, default_session)"
                           " VALUES (?,?,?,?,?,?,?,?,?);",
                           -1, &stmt, NULL) != SQLITE_OK) {
        set_error(err, errlen, "cannot prepare insert: %s", sqlite3_errmsg(catalog->db));
        return AFK_ENV_ERR_STORAGE;
    }
    sqlite3_bind_text(stmt, 1, record.id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, record.name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, record.distro_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, record.distro_version, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, record.architecture, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, record.relative_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 7, record.created_at);
    sqlite3_bind_text(stmt, 8, record.default_user, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, record.default_session, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    int extended = sqlite3_extended_errcode(catalog->db);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        if (extended == SQLITE_CONSTRAINT_UNIQUE || extended == SQLITE_CONSTRAINT_PRIMARYKEY) {
            set_error(err, errlen, "an environment named \"%s\" already exists", record.name);
            return AFK_ENV_ERR_DUPLICATE_NAME;
        }
        set_error(err, errlen, "cannot store environment: %s", sqlite3_errmsg(catalog->db));
        return AFK_ENV_ERR_STORAGE;
    }
    *out = record;
    return AFK_ENV_OK;
}

int afk_catalog_get(struct afk_catalog *catalog, const char *id, struct afk_env_record *out) {
    if (!catalog || !out || !afk_env_validate_uuid(id)) return AFK_ENV_ERR_INVALID;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(catalog->db, kSelectColumns " WHERE id = ?;", -1, &stmt, NULL) != SQLITE_OK)
        return AFK_ENV_ERR_STORAGE;
    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
    int result = AFK_ENV_ERR_NOT_FOUND;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        read_record(stmt, out);
        result = AFK_ENV_OK;
    }
    sqlite3_finalize(stmt);
    return result;
}

int afk_catalog_list(struct afk_catalog *catalog, afk_env_visit_fn visit, void *ctx) {
    if (!catalog || !visit) return AFK_ENV_ERR_INVALID;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(catalog->db, kSelectColumns " ORDER BY created_at, rowid;", -1, &stmt, NULL) != SQLITE_OK)
        return AFK_ENV_ERR_STORAGE;
    int count = 0;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        struct afk_env_record record;
        memset(&record, 0, sizeof record);
        read_record(stmt, &record);
        visit(&record, ctx);
        count++;
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? count : AFK_ENV_ERR_STORAGE;
}

int afk_catalog_rename(struct afk_catalog *catalog, const char *id, const char *new_name,
                       char *err, size_t errlen) {
    if (!catalog || !afk_env_validate_uuid(id)) {
        set_error(err, errlen, "invalid environment id");
        return AFK_ENV_ERR_INVALID;
    }
    if (!afk_env_validate_name(new_name)) {
        set_error(err, errlen, "invalid environment name");
        return AFK_ENV_ERR_INVALID;
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(catalog->db, "UPDATE environments SET name = ? WHERE id = ?;",
                           -1, &stmt, NULL) != SQLITE_OK) {
        set_error(err, errlen, "cannot prepare rename: %s", sqlite3_errmsg(catalog->db));
        return AFK_ENV_ERR_STORAGE;
    }
    sqlite3_bind_text(stmt, 1, new_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, id, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    int extended = sqlite3_extended_errcode(catalog->db);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        if (extended == SQLITE_CONSTRAINT_UNIQUE) {
            set_error(err, errlen, "an environment named \"%s\" already exists", new_name);
            return AFK_ENV_ERR_DUPLICATE_NAME;
        }
        set_error(err, errlen, "cannot rename environment: %s", sqlite3_errmsg(catalog->db));
        return AFK_ENV_ERR_STORAGE;
    }
    if (sqlite3_changes(catalog->db) == 0) {
        set_error(err, errlen, "environment not found");
        return AFK_ENV_ERR_NOT_FOUND;
    }
    return AFK_ENV_OK;
}

int afk_catalog_set_active(struct afk_catalog *catalog, const char *id, char *err, size_t errlen) {
    if (!catalog) return AFK_ENV_ERR_INVALID;
    if (id && *id) {
        struct afk_env_record record;
        int found = afk_catalog_get(catalog, id, &record);
        if (found != AFK_ENV_OK) {
            set_error(err, errlen, "environment not found");
            return AFK_ENV_ERR_NOT_FOUND;
        }
    } else {
        id = "";
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(catalog->db,
                           "INSERT INTO catalog_meta(key, value) VALUES('active_environment', ?)"
                           " ON CONFLICT(key) DO UPDATE SET value = excluded.value;",
                           -1, &stmt, NULL) != SQLITE_OK) {
        set_error(err, errlen, "cannot prepare active selection: %s", sqlite3_errmsg(catalog->db));
        return AFK_ENV_ERR_STORAGE;
    }
    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        set_error(err, errlen, "cannot record active environment: %s", sqlite3_errmsg(catalog->db));
        return AFK_ENV_ERR_STORAGE;
    }
    return AFK_ENV_OK;
}

int afk_catalog_active(struct afk_catalog *catalog, char out[AFK_ENV_ID_LEN + 1]) {
    if (!catalog || !out) return AFK_ENV_ERR_INVALID;
    out[0] = '\0';
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(catalog->db,
                           "SELECT value FROM catalog_meta WHERE key = 'active_environment';",
                           -1, &stmt, NULL) != SQLITE_OK)
        return AFK_ENV_ERR_STORAGE;
    int result = AFK_ENV_OK;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *value = (const char *) sqlite3_column_text(stmt, 0);
        if (value && afk_env_validate_uuid(value)) {
            copy_str(out, AFK_ENV_ID_LEN + 1, value);
        } else {
            /* A stale selection must not block deletion or startup. */
            result = AFK_ENV_ERR_NOT_FOUND;
        }
    }
    sqlite3_finalize(stmt);
    return result;
}

int afk_catalog_delete(struct afk_catalog *catalog, const char *id, char *err, size_t errlen) {
    if (!catalog || !afk_env_validate_uuid(id)) {
        set_error(err, errlen, "invalid environment id");
        return AFK_ENV_ERR_INVALID;
    }
    char active[AFK_ENV_ID_LEN + 1];
    if (afk_catalog_active(catalog, active) == AFK_ENV_OK && strcmp(active, id) == 0) {
        set_error(err, errlen, "the active environment cannot be deleted");
        return AFK_ENV_ERR_ACTIVE;
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(catalog->db, "DELETE FROM environments WHERE id = ?;",
                           -1, &stmt, NULL) != SQLITE_OK) {
        set_error(err, errlen, "cannot prepare delete: %s", sqlite3_errmsg(catalog->db));
        return AFK_ENV_ERR_STORAGE;
    }
    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        set_error(err, errlen, "cannot delete environment: %s", sqlite3_errmsg(catalog->db));
        return AFK_ENV_ERR_STORAGE;
    }
    if (sqlite3_changes(catalog->db) == 0) {
        set_error(err, errlen, "environment not found");
        return AFK_ENV_ERR_NOT_FOUND;
    }
    return AFK_ENV_OK;
}

int afk_catalog_add_desktop(struct afk_catalog *catalog, const char *id, const char *desktop_id,
                            char *err, size_t errlen) {
    if (!catalog || !afk_env_validate_uuid(id) || !desktop_id || !*desktop_id ||
        strlen(desktop_id) > AFK_ENV_DISTRO_LEN) {
        set_error(err, errlen, "invalid desktop or environment id");
        return AFK_ENV_ERR_INVALID;
    }
    struct afk_env_record record;
    if (afk_catalog_get(catalog, id, &record) != AFK_ENV_OK) {
        set_error(err, errlen, "environment not found");
        return AFK_ENV_ERR_NOT_FOUND;
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(catalog->db,
                           "INSERT OR IGNORE INTO env_desktops(env_id, desktop_id) VALUES (?, ?);",
                           -1, &stmt, NULL) != SQLITE_OK) {
        set_error(err, errlen, "cannot prepare desktop insert: %s", sqlite3_errmsg(catalog->db));
        return AFK_ENV_ERR_STORAGE;
    }
    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, desktop_id, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        set_error(err, errlen, "cannot record desktop: %s", sqlite3_errmsg(catalog->db));
        return AFK_ENV_ERR_STORAGE;
    }
    return AFK_ENV_OK;
}

int afk_catalog_remove_desktop(struct afk_catalog *catalog, const char *id, const char *desktop_id) {
    if (!catalog || !afk_env_validate_uuid(id) || !desktop_id) return AFK_ENV_ERR_INVALID;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(catalog->db,
                           "DELETE FROM env_desktops WHERE env_id = ? AND desktop_id = ?;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return AFK_ENV_ERR_STORAGE;
    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, desktop_id, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? AFK_ENV_OK : AFK_ENV_ERR_STORAGE;
}

int afk_catalog_list_desktops(struct afk_catalog *catalog, const char *id,
                              char *joined, size_t joinedlen) {
    if (!catalog || !afk_env_validate_uuid(id) || !joined || joinedlen == 0)
        return AFK_ENV_ERR_INVALID;
    joined[0] = '\0';
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(catalog->db,
                           "SELECT desktop_id FROM env_desktops WHERE env_id = ? ORDER BY desktop_id;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return AFK_ENV_ERR_STORAGE;
    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
    size_t used = 0;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *name = (const char *) sqlite3_column_text(stmt, 0);
        size_t need = strlen(name) + (used ? 1 : 0);
        if (used + need + 1 > joinedlen) break;
        if (used) joined[used++] = ',';
        memcpy(joined + used, name, strlen(name));
        used += strlen(name);
        joined[used] = '\0';
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE || rc == SQLITE_ROW ? AFK_ENV_OK : AFK_ENV_ERR_STORAGE;
}

int afk_catalog_resolve_root(struct afk_catalog *catalog, const struct afk_env_record *env,
                             char *out, size_t outlen) {
    if (!catalog || !env || !out || outlen == 0) return AFK_ENV_ERR_INVALID;
    /* Reject anything that escapes the catalog directory. */
    if (env->relative_path[0] == '/' || strstr(env->relative_path, ".."))
        return AFK_ENV_ERR_INVALID;
    int written = snprintf(out, outlen, "%s/%s", catalog->directory, env->relative_path);
    if (written < 0 || (size_t) written >= outlen) return AFK_ENV_ERR_INVALID;
    return AFK_ENV_OK;
}
