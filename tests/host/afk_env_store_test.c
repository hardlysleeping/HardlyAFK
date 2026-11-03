/* Host regression for the filesystem side of environments. Build with -lsqlite3. */
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/environment/afk_env_catalog.h"
#include "core/environment/afk_env_store.h"

static int exists(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0;
}

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    fputs(content, f);
    fclose(f);
}

static int content_is(const char *path, const char *expected) {
    char buffer[256];
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(buffer, 1, sizeof buffer - 1, f);
    buffer[n] = '\0';
    fclose(f);
    return strcmp(buffer, expected) == 0;
}

static void temp_dir(char *out, size_t len) {
    const char *base = getenv("TMPDIR");
    snprintf(out, len, "%s/afk-store-XXXXXX", base ? base : "/tmp");
    assert(mkdtemp(out) != NULL);
}

static void join(char *out, size_t len, const char *a, const char *b) {
    snprintf(out, len, "%s%s", a, b);
}

int main(void) {
    char dir[512];
    char outside[512];
    char err[512] = {0};
    char path[AFK_ENV_PATH_MAX];
    temp_dir(dir, sizeof dir);
    temp_dir(outside, sizeof outside);

    char outside_keep[600];
    join(outside_keep, sizeof outside_keep, outside, "/keep");
    write_file(outside_keep, "outside-data");

    struct afk_catalog *catalog = NULL;
    assert(afk_catalog_open(dir, &catalog, err, sizeof err) == AFK_ENV_OK);

    struct afk_env_spec debian = {
        .name = "Debian Work", .distro_id = "debian", .distro_version = "13",
        .architecture = "aarch64", .default_user = "hardly",
        .default_session = AFK_ENV_SESSION_TERMINAL, .created_at = 1700000000,
    };
    struct afk_env_spec alpine = {
        .name = "Alpine Test", .distro_id = "alpine", .distro_version = "3.24",
        .architecture = "aarch64", .default_user = "root",
        .default_session = AFK_ENV_SESSION_DESKTOP, .created_at = 1700000100,
    };

    struct afk_env_record a, b;
    assert(afk_store_create(catalog, &debian, &a, err, sizeof err) == AFK_ENV_OK);
    assert(afk_store_create(catalog, &alpine, &b, err, sizeof err) == AFK_ENV_OK);

    char root_a[AFK_ENV_PATH_MAX], root_b[AFK_ENV_PATH_MAX];
    assert(afk_catalog_resolve_root(catalog, &a, root_a, sizeof root_a) == AFK_ENV_OK);
    assert(afk_catalog_resolve_root(catalog, &b, root_b, sizeof root_b) == AFK_ENV_OK);
    assert(strcmp(root_a, root_b) != 0);
    assert(exists(root_a) && exists(root_b));
    join(path, sizeof path, root_a, "/rootfs");
    assert(exists(path));

    /* A change in one environment is invisible to the other. */
    char marker_a[AFK_ENV_PATH_MAX], marker_b[AFK_ENV_PATH_MAX];
    join(marker_a, sizeof marker_a, root_a, "/rootfs/marker");
    join(marker_b, sizeof marker_b, root_b, "/rootfs/marker");
    write_file(marker_a, "debian-only");
    assert(content_is(marker_a, "debian-only"));
    assert(!exists(marker_b));

    struct afk_env_usage usage;
    assert(afk_store_usage(catalog, a.id, &usage, err, sizeof err) == AFK_ENV_OK);
    assert(usage.files == 1);
    assert(usage.directories == 2); /* environment dir + rootfs */
    assert(usage.bytes == strlen("debian-only"));

    /* A duplicate name fails without creating a third tree. */
    struct afk_env_record rejected;
    assert(afk_store_create(catalog, &debian, &rejected, err, sizeof err) == AFK_ENV_ERR_DUPLICATE_NAME);
    char envdir[600];
    join(envdir, sizeof envdir, dir, "/environments");
    int entries = 0;
    {
        char cmd[700];
        FILE *p;
        snprintf(cmd, sizeof cmd, "ls -1 '%s' | wc -l", envdir);
        p = popen(cmd, "r");
        assert(p && fscanf(p, "%d", &entries) == 1);
        pclose(p);
    }
    assert(entries == 2);

    /* A symlink planted inside a tree is unlinked, never followed. */
    join(path, sizeof path, root_b, "/rootfs/escape");
    assert(symlink(outside, path) == 0);

    /* The active environment is protected; the inactive one is removable. */
    assert(afk_catalog_set_active(catalog, b.id, err, sizeof err) == AFK_ENV_OK);
    assert(afk_store_delete(catalog, b.id, err, sizeof err) == AFK_ENV_ERR_ACTIVE);
    assert(exists(root_b));

    assert(afk_store_delete(catalog, a.id, err, sizeof err) == AFK_ENV_OK);
    assert(!exists(root_a));
    struct afk_env_record fetched;
    assert(afk_catalog_get(catalog, a.id, &fetched) == AFK_ENV_ERR_NOT_FOUND);
    assert(content_is(marker_b, "debian-only") == 0); /* B never had A's marker */

    /* Deleting B clears its tree but leaves the symlink target intact. */
    assert(afk_catalog_set_active(catalog, "", err, sizeof err) == AFK_ENV_OK);
    assert(afk_store_delete(catalog, b.id, err, sizeof err) == AFK_ENV_OK);
    assert(!exists(root_b));
    assert(content_is(outside_keep, "outside-data"));
    assert(afk_store_delete(catalog, b.id, err, sizeof err) == AFK_ENV_ERR_NOT_FOUND);

    afk_catalog_close(catalog);
    assert(exists(dir));
    printf("env-store-test: PASS (%s)\n", dir);
    return 0;
}
