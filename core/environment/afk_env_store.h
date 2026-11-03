#ifndef AFK_ENV_STORE_H
#define AFK_ENV_STORE_H

/*
 * Filesystem side of an environment: creates and removes the directory tree
 * beneath the catalog, and measures its usage.
 *
 * Every path is derived from the catalog's own directory plus a validated
 * relative path, and directory walks use openat(..., O_NOFOLLOW) so a symlink
 * planted inside the tree cannot redirect a delete outside it.
 */

#include <stdint.h>
#include <stddef.h>

#include "afk_env_catalog.h"

struct afk_env_usage {
    uint64_t bytes;
    uint64_t files;
    uint64_t directories;
};

/* Inserts the record, then creates environments/<uuid>/rootfs. Rolls the row
 * back when the directory cannot be created, so metadata never describes a
 * filesystem that does not exist. */
int afk_store_create(struct afk_catalog *catalog, const struct afk_env_spec *spec,
                     struct afk_env_record *out, char *err, size_t errlen);

/* Creates the tree for an existing record (used after importing metadata). */
int afk_store_materialise(struct afk_catalog *catalog, const struct afk_env_record *env,
                          char *err, size_t errlen);

/* Refuses the active environment, removes the tree, then the row. If the row
 * cannot be removed the tree is already gone; the caller must re-run delete. */
int afk_store_delete(struct afk_catalog *catalog, const char *id, char *err, size_t errlen);

int afk_store_usage(struct afk_catalog *catalog, const char *id, struct afk_env_usage *out,
                    char *err, size_t errlen);

#endif
