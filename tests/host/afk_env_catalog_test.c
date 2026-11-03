/* Host regression for core/environment. Build with -lsqlite3. */
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/environment/afk_env_catalog.h"

struct collector {
    struct afk_env_record items[8];
    int count;
};

static void collect(const struct afk_env_record *env, void *ctx) {
    struct collector *c = ctx;
    if (c->count < 8) c->items[c->count] = *env;
    c->count++;
}

static void make_temp_dir(char *out, size_t len) {
    const char *base = getenv("TMPDIR");
    snprintf(out, len, "%s/afk-env-XXXXXX", base ? base : "/tmp");
    char *made = mkdtemp(out);
    assert(made != NULL);
}

int main(void) {
    char dir[512];
    char err[512] = {0};
    make_temp_dir(dir, sizeof dir);

    struct afk_catalog *catalog = NULL;
    assert(afk_catalog_open(dir, &catalog, err, sizeof err) == AFK_ENV_OK);
    assert(catalog != NULL);

    /* Missing directory is rejected rather than silently created. */
    struct afk_catalog *bogus = NULL;
    assert(afk_catalog_open("/nonexistent/hardlyafk", &bogus, err, sizeof err) == AFK_ENV_ERR_INVALID);
    assert(bogus == NULL);

    struct afk_env_spec spec = {
        .name = "Debian Work",
        .distro_id = "debian",
        .distro_version = "13",
        .architecture = "aarch64",
        .default_user = "hardly",
        .default_session = AFK_ENV_SESSION_TERMINAL,
        .created_at = 1700000000,
    };
    struct afk_env_record a, b;
    assert(afk_catalog_create(catalog, &spec, &a, err, sizeof err) == AFK_ENV_OK);
    assert(afk_env_validate_uuid(a.id));
    assert(strcmp(a.name, "Debian Work") == 0);
    assert(a.created_at == 1700000000);
    assert(strcmp(a.default_session, "terminal") == 0);
    assert(strncmp(a.relative_path, "environments/", 13) == 0);
    assert(strstr(a.relative_path, a.id) != NULL);

    /* Names are unique case-insensitively, and bad names never reach the store. */
    struct afk_env_record duplicate;
    assert(afk_catalog_create(catalog, &spec, &duplicate, err, sizeof err) == AFK_ENV_ERR_DUPLICATE_NAME);
    assert(strstr(err, "already exists") != NULL);

    const char *bad_names[] = {"", ".", "..", "a/b", "../escape", "trailing ", "colon:name", NULL};
    for (int i = 0; bad_names[i]; i++) {
        struct afk_env_spec bad = spec;
        bad.name = bad_names[i];
        assert(afk_catalog_create(catalog, &bad, &duplicate, err, sizeof err) == AFK_ENV_ERR_INVALID);
    }
    char long_name[80];
    memset(long_name, 'x', sizeof long_name - 1);
    long_name[sizeof long_name - 1] = '\0';
    struct afk_env_spec bad_spec = spec;
    bad_spec.name = long_name;
    assert(afk_catalog_create(catalog, &bad_spec, &duplicate, err, sizeof err) == AFK_ENV_ERR_INVALID);

    struct afk_env_spec desktop_spec = {
        .name = "Alpine Test",
        .distro_id = "alpine",
        .distro_version = "3.24",
        .architecture = "aarch64",
        .default_user = "root",
        .default_session = AFK_ENV_SESSION_DESKTOP,
        .created_at = 1700000100,
    };
    assert(afk_catalog_create(catalog, &desktop_spec, &b, err, sizeof err) == AFK_ENV_OK);
    assert(strcmp(a.id, b.id) != 0);
    assert(strcmp(a.relative_path, b.relative_path) != 0);

    struct afk_env_spec bad_session = spec;
    bad_session.name = "Bad Session";
    bad_session.default_session = "wayland";
    assert(afk_catalog_create(catalog, &bad_session, &duplicate, err, sizeof err) == AFK_ENV_ERR_INVALID);

    /* Listing is creation order and complete. */
    struct collector seen = {0};
    assert(afk_catalog_list(catalog, collect, &seen) == 2);
    assert(seen.count == 2);
    assert(strcmp(seen.items[0].id, a.id) == 0);
    assert(strcmp(seen.items[1].id, b.id) == 0);

    /* Lookup by id. */
    struct afk_env_record fetched;
    assert(afk_catalog_get(catalog, a.id, &fetched) == AFK_ENV_OK);
    assert(strcmp(fetched.name, "Debian Work") == 0);
    assert(afk_catalog_get(catalog, "00000000-0000-4000-8000-000000000000", &fetched) == AFK_ENV_ERR_NOT_FOUND);
    assert(afk_catalog_get(catalog, "not-a-uuid", &fetched) == AFK_ENV_ERR_INVALID);

    /* Rename changes metadata only; the filesystem path is untouched. */
    assert(afk_catalog_rename(catalog, b.id, "Alpine Renamed", err, sizeof err) == AFK_ENV_OK);
    assert(afk_catalog_get(catalog, b.id, &fetched) == AFK_ENV_OK);
    assert(strcmp(fetched.name, "Alpine Renamed") == 0);
    assert(strcmp(fetched.relative_path, b.relative_path) == 0);
    assert(afk_catalog_rename(catalog, b.id, "debian work", err, sizeof err) == AFK_ENV_ERR_DUPLICATE_NAME);
    assert(afk_catalog_rename(catalog, b.id, "", err, sizeof err) == AFK_ENV_ERR_INVALID);

    /* The active environment is protected from deletion, others are not. */
    assert(afk_catalog_set_active(catalog, a.id, err, sizeof err) == AFK_ENV_OK);
    char active[AFK_ENV_ID_LEN + 1];
    assert(afk_catalog_active(catalog, active) == AFK_ENV_OK);
    assert(strcmp(active, a.id) == 0);
    assert(afk_catalog_delete(catalog, a.id, err, sizeof err) == AFK_ENV_ERR_ACTIVE);

    assert(afk_catalog_set_active(catalog, b.id, err, sizeof err) == AFK_ENV_OK);
    assert(afk_catalog_delete(catalog, a.id, err, sizeof err) == AFK_ENV_OK);
    assert(afk_catalog_get(catalog, a.id, &fetched) == AFK_ENV_ERR_NOT_FOUND);
    assert(afk_catalog_get(catalog, b.id, &fetched) == AFK_ENV_OK);
    assert(afk_catalog_delete(catalog, a.id, err, sizeof err) == AFK_ENV_ERR_NOT_FOUND);

    /* Desktop installs are per environment and replace duplicates. */
    assert(afk_catalog_add_desktop(catalog, b.id, "lxqt", err, sizeof err) == AFK_ENV_OK);
    assert(afk_catalog_add_desktop(catalog, b.id, "lxqt", err, sizeof err) == AFK_ENV_OK);
    assert(afk_catalog_add_desktop(catalog, b.id, "plasma", err, sizeof err) == AFK_ENV_OK);
    char desktops[128];
    assert(afk_catalog_list_desktops(catalog, b.id, desktops, sizeof desktops) == AFK_ENV_OK);
    assert(strcmp(desktops, "lxqt,plasma") == 0);
    assert(afk_catalog_remove_desktop(catalog, b.id, "lxqt") == AFK_ENV_OK);
    assert(afk_catalog_list_desktops(catalog, b.id, desktops, sizeof desktops) == AFK_ENV_OK);
    assert(strcmp(desktops, "plasma") == 0);
    assert(afk_catalog_add_desktop(catalog, a.id, "lxqt", err, sizeof err) == AFK_ENV_ERR_NOT_FOUND);

    /* Root resolution is relative to the current container, never absolute-stored. */
    char root[AFK_ENV_PATH_MAX];
    assert(afk_catalog_resolve_root(catalog, &fetched, root, sizeof root) == AFK_ENV_OK);
    assert(strncmp(root, dir, strlen(dir)) == 0);
    struct afk_env_record escape = fetched;
    snprintf(escape.relative_path, sizeof escape.relative_path, "environments/../../../etc");
    assert(afk_catalog_resolve_root(catalog, &escape, root, sizeof root) == AFK_ENV_ERR_INVALID);
    snprintf(escape.relative_path, sizeof escape.relative_path, "/etc");
    assert(afk_catalog_resolve_root(catalog, &escape, root, sizeof root) == AFK_ENV_ERR_INVALID);

    /* Desktops are removed with their environment. */
    afk_catalog_close(catalog);
    assert(afk_catalog_open(dir, &catalog, err, sizeof err) == AFK_ENV_OK);
    assert(afk_catalog_list(catalog, collect, &seen) == 1);
    assert(afk_catalog_get(catalog, b.id, &fetched) == AFK_ENV_OK);
    assert(strcmp(fetched.name, "Alpine Renamed") == 0);
    assert(strcmp(fetched.default_session, "desktop") == 0);
    assert(afk_catalog_list_desktops(catalog, b.id, desktops, sizeof desktops) == AFK_ENV_OK);
    assert(strcmp(desktops, "plasma") == 0);
    assert(afk_catalog_active(catalog, active) == AFK_ENV_OK);
    assert(strcmp(active, b.id) == 0);

    /* Clearing the selection lets every environment be deleted again. */
    assert(afk_catalog_set_active(catalog, "", err, sizeof err) == AFK_ENV_OK);
    assert(afk_catalog_active(catalog, active) == AFK_ENV_ERR_NOT_FOUND);
    assert(afk_catalog_delete(catalog, b.id, err, sizeof err) == AFK_ENV_OK);
    afk_catalog_close(catalog);

    char catalog_path[600];
    snprintf(catalog_path, sizeof catalog_path, "%s/catalog.db", dir);
    struct stat st;
    assert(stat(catalog_path, &st) == 0);
    printf("env-catalog-test: PASS (%s, catalog.db %lld bytes)\n", dir, (long long) st.st_size);
    return 0;
}
