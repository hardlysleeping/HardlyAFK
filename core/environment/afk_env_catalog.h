#ifndef AFK_ENV_CATALOG_H
#define AFK_ENV_CATALOG_H

/*
 * Environment catalog: durable metadata for independent Linux environments.
 *
 * One catalog lives inside the app container. Each environment owns a
 * directory beneath <catalog>/environments/<uuid>/ holding its fakefs.
 * Only the RELATIVE path is stored: iOS rewrites the container path on
 * reinstall, so absolute paths must never be persisted.
 *
 * Filesystem identity (the guest filesystem itself) is not created here;
 * the host layer materialises it at afk_catalog_resolve_root(). This module
 * only records, validates and orders metadata.
 */

#include <stddef.h>
#include <stdint.h>

#define AFK_ENV_ID_LEN 36
#define AFK_ENV_NAME_LEN 64
#define AFK_ENV_DISTRO_LEN 32
#define AFK_ENV_VERSION_LEN 32
#define AFK_ENV_ARCH_LEN 16
#define AFK_ENV_USER_LEN 32
#define AFK_ENV_SESSION_LEN 16
#define AFK_ENV_RELPATH_LEN 96
#define AFK_ENV_PATH_MAX 1024

#define AFK_ENV_SESSION_TERMINAL "terminal"
#define AFK_ENV_SESSION_DESKTOP "desktop"

enum afk_env_result {
    AFK_ENV_OK = 0,
    AFK_ENV_ERR_INVALID = -1,
    AFK_ENV_ERR_DUPLICATE_NAME = -2,
    AFK_ENV_ERR_NOT_FOUND = -3,
    AFK_ENV_ERR_ACTIVE = -4,
    AFK_ENV_ERR_STORAGE = -5
};

struct afk_env_record {
    char id[AFK_ENV_ID_LEN + 1];
    char name[AFK_ENV_NAME_LEN + 1];
    char distro_id[AFK_ENV_DISTRO_LEN + 1];
    char distro_version[AFK_ENV_VERSION_LEN + 1];
    char architecture[AFK_ENV_ARCH_LEN + 1];
    char relative_path[AFK_ENV_RELPATH_LEN + 1];
    int64_t created_at;
    char default_user[AFK_ENV_USER_LEN + 1];
    char default_session[AFK_ENV_SESSION_LEN + 1];
};

struct afk_env_spec {
    const char *name;
    const char *distro_id;
    const char *distro_version;
    const char *architecture;
    const char *default_user;
    const char *default_session;
    int64_t created_at; /* 0 means "use the current time" */
};

typedef void (*afk_env_visit_fn)(const struct afk_env_record *env, void *ctx);

struct afk_catalog;

/* Opens (creating when absent) <directory>/catalog.db. directory must exist. */
int afk_catalog_open(const char *directory, struct afk_catalog **out,
                     char *err, size_t errlen);
void afk_catalog_close(struct afk_catalog *catalog);

/* Inserts a record and generates its UUID and relative root path. */
int afk_catalog_create(struct afk_catalog *catalog, const struct afk_env_spec *spec,
                       struct afk_env_record *out, char *err, size_t errlen);
int afk_catalog_get(struct afk_catalog *catalog, const char *id,
                    struct afk_env_record *out);
/* Creation order. */
int afk_catalog_list(struct afk_catalog *catalog, afk_env_visit_fn visit, void *ctx);

int afk_catalog_rename(struct afk_catalog *catalog, const char *id, const char *new_name,
                       char *err, size_t errlen);
/* Refuses while the environment is the active one. */
int afk_catalog_delete(struct afk_catalog *catalog, const char *id, char *err, size_t errlen);

/* Exactly one environment may be active; "" clears the selection. */
int afk_catalog_set_active(struct afk_catalog *catalog, const char *id, char *err, size_t errlen);
/* Copies the active id, or leaves out empty when none is set. */
int afk_catalog_active(struct afk_catalog *catalog, char out[AFK_ENV_ID_LEN + 1]);

int afk_catalog_add_desktop(struct afk_catalog *catalog, const char *id, const char *desktop_id,
                            char *err, size_t errlen);
int afk_catalog_remove_desktop(struct afk_catalog *catalog, const char *id, const char *desktop_id);
int afk_catalog_list_desktops(struct afk_catalog *catalog, const char *id,
                              char *joined, size_t joinedlen);

/* Resolves the environment's filesystem directory inside the current container. */
int afk_catalog_resolve_root(struct afk_catalog *catalog, const struct afk_env_record *env,
                             char *out, size_t outlen);

/* Shared validation, exposed for the UI layer and tests. */
int afk_env_validate_name(const char *name);
int afk_env_validate_uuid(const char *id);

#endif
