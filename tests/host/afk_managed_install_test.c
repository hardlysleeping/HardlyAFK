#define _GNU_SOURCE
#include <assert.h>
#include <archive.h>
#include <archive_entry.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "core/environment/afk_env_store.h"
#include <unistd.h>
#include <sqlite3.h>
#include "tools/afk_managed_install.h"
#include "core/environment/afk_ios_boot.h"
#include "core/distro/afk_sha256.h"

static char err[512];
static void visit(const struct afk_env_record *record, void *ctx) { (void)record; (void)ctx; }
static int rows(struct afk_catalog *c) { return afk_catalog_list(c, visit, NULL); }
static void cancel(void *ctx, double value, const char *text, bool *out) {
    (void)value; (void)text;
    int *remaining = ctx;
    *out = !remaining || --*remaining == 0;
}
static void interrupt_import(void *ctx, double value, const char *text, bool *out) {
    (void)value; (void)text; (void)out;
    if (--*(int *)ctx == 0) _exit(77); // No cleanup: abrupt process termination.
}
static void find_interrupted(const struct afk_env_record *record, void *ctx) {
    if (!strcmp(record->name, "Interrupted")) *(struct afk_env_record *)ctx = *record;
}
static int children(const char *root) {
    DIR *d = opendir(root); assert(d);
    int n = 0; struct dirent *e;
    while ((e = readdir(d))) if (e->d_name[0] != '.') n++;
    closedir(d); return n;
}
static void fixture(const char *path, const char *member, const char *hardlink) {
    struct archive *a = archive_write_new();
    archive_write_set_format_pax_restricted(a);
    archive_write_add_filter_gzip(a);
    assert(archive_write_open_filename(a, path) == ARCHIVE_OK);
    struct archive_entry *e = archive_entry_new();
    archive_entry_set_pathname(e, member);
    archive_entry_set_filetype(e, AE_IFREG);
    archive_entry_set_perm(e, 0644);
    archive_entry_set_size(e, 0);
    if (hardlink) archive_entry_set_hardlink(e, hardlink);
    assert(archive_write_header(a, e) == ARCHIVE_OK);
    archive_entry_free(e);
    assert(archive_write_free(a) == ARCHIVE_OK);
}
static void conflicting_fixture(const char *path) {
    struct archive *a = archive_write_new();
    archive_write_set_format_pax_restricted(a);
    archive_write_add_filter_gzip(a);
    assert(archive_write_open_filename(a, path) == ARCHIVE_OK);
    const char *members[] = {"parent", "parent/child"};
    for (size_t i = 0; i < 2; i++) {
        struct archive_entry *e = archive_entry_new();
        archive_entry_set_pathname(e, members[i]);
        archive_entry_set_filetype(e, AE_IFREG);
        archive_entry_set_perm(e, 0644);
        archive_entry_set_size(e, 0);
        assert(archive_write_header(a, e) == ARCHIVE_OK);
        archive_entry_free(e);
    }
    assert(archive_write_free(a) == ARCHIVE_OK);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    const struct afk_distro *alpine = afk_distro_find("alpine", "3.24");
    char temp[2048];
    snprintf(temp, sizeof temp, "%s/afk-managed-XXXXXX", getenv("TMPDIR") ?: ".");
    assert(mkdtemp(temp));
    struct afk_catalog *c = NULL;
    assert(afk_catalog_open(temp, &c, err, sizeof err) == 0);
    struct afk_env_record one, two, failed;
    assert(afk_managed_install(c, alpine, argv[1], "First", (struct progress){0}, &one, err, sizeof err) == 0);
    char root[AFK_ENV_PATH_MAX];
    assert(afk_ios_boot_root(c, one.id, root, err, sizeof err) == 0);
    char sentinel[4096]; snprintf(sentinel, sizeof sentinel, "%s/data/sentinel", root);
    FILE *f = fopen(sentinel, "w"); assert(f); fputs("preserve", f); fclose(f);
    assert(afk_catalog_set_active(c, one.id, err, sizeof err) == 0);
    assert(afk_managed_install(c, alpine, argv[1], "Second", (struct progress){0}, &two, err, sizeof err) == 0);
    assert(strcmp(one.id, two.id));
    assert(afk_ios_boot_root(c, two.id, root, err, sizeof err) == 0);
    char path[4096]; snprintf(path, sizeof path, "%s/data/bin/busybox", root);
    f = fopen(path, "rb"); assert(f);
    unsigned char elf[20]; assert(fread(elf, 1, sizeof elf, f) == sizeof elf); fclose(f);
    assert(!memcmp(elf, "\177ELF", 4) && elf[18] == 183 && elf[19] == 0);
    snprintf(path, sizeof path, "%s/data/bin/sh", root);
    struct stat st; assert(lstat(path, &st) == 0 && S_ISREG(st.st_mode));
    f = fopen(path, "r"); assert(f); char link[32] = {0}; assert(fread(link, 1, sizeof link - 1, f) > 0); fclose(f);
    assert(!strcmp(link, "/bin/busybox") || !strcmp(link, "busybox"));
    snprintf(path, sizeof path, "%s/meta.db", root);
    sqlite3 *db = NULL; sqlite3_stmt *q = NULL;
    assert(sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    assert(sqlite3_prepare_v2(db, "SELECT stat FROM stats JOIN paths USING(inode) WHERE path=CAST('/bin/sh' AS BLOB)", -1, &q, NULL) == SQLITE_OK);
    assert(sqlite3_step(q) == SQLITE_ROW);
    /* First uint32_t in the real runtime stat blob is guest mode. */
    uint32_t mode; memcpy(&mode, sqlite3_column_blob(q, 0), sizeof mode);
    assert((mode & S_IFMT) == S_IFLNK);
    sqlite3_finalize(q); sqlite3_close(db);
    snprintf(path, sizeof path, "%s/environments", temp);
    assert(children(path) == 2);
    for (int i = 0; i < 5; i++) {
        int remaining = 1 + i * 100;
        assert(afk_managed_install(c, alpine, argv[1], "Cancelled", (struct progress){&remaining, cancel}, &failed, err, sizeof err) < 0);
        assert(remaining == 0 && strstr(err, "Rootfs conversion failed"));
        assert(!failed.id[0] && rows(c) == 2 && children(path) == 2);
    }
    // Exercise a real interrupted conversion, not ordinary rollback. The row
    // remains visible after reopening; the existing boot validator must refuse
    // its uncommitted fakefs schema. No automatic cleanup/recovery is claimed.
    afk_catalog_close(c);
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        struct afk_catalog *child_catalog = NULL;
        assert(afk_catalog_open(temp, &child_catalog, err, sizeof err) == 0);
        int remaining = 100;
        afk_managed_install(child_catalog, alpine, argv[1], "Interrupted",
            (struct progress){&remaining, interrupt_import}, NULL, err, sizeof err);
        _exit(78);
    }
    int status; assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 77);
    assert(afk_catalog_open(temp, &c, err, sizeof err) == 0);
    struct afk_env_record interrupted = {0};
    assert(afk_catalog_list(c, find_interrupted, &interrupted) == 3);
    assert(interrupted.id[0] && children(path) == 3);
    assert(afk_ios_boot_root(c, interrupted.id, root, err, sizeof err) < 0 && !root[0]);
    // Explicit test-only deletion of the disposable residue, not app recovery.
    assert(afk_store_delete(c, interrupted.id, err, sizeof err) == 0);
    assert(rows(c) == 2 && children(path) == 2);
    char fixture_path[4096]; snprintf(fixture_path, sizeof fixture_path, "%s/input.tar.gz", temp);
    FILE *src = fopen(argv[1], "rb"); assert(src);
    f = fopen(fixture_path, "wb"); assert(f);
    int ch = fgetc(src); assert(ch != EOF); fputc(ch ^ 1, f);
    char chunk[65536]; size_t length;
    while ((length = fread(chunk, 1, sizeof chunk, src))) assert(fwrite(chunk, 1, length, f) == length);
    fclose(src); fclose(f);
    assert(afk_managed_install(c, alpine, fixture_path, "Flipped real archive", (struct progress){0}, &failed, err, sizeof err) < 0);
    assert(!failed.id[0] && rows(c) == 2 && children(path) == 2);
    unlink(fixture_path);
    assert(symlink(argv[1], fixture_path) == 0);
    assert(afk_managed_install(c, alpine, fixture_path, "Symlink archive", (struct progress){0}, &failed, err, sizeof err) < 0);
    unlink(fixture_path);
    fixture(fixture_path, "safe", NULL);
    assert(afk_managed_install(c, alpine, fixture_path, "Bad checksum", (struct progress){0}, &failed, err, sizeof err) < 0);
    struct afk_distro pinned = *alpine;
    char digest[65]; pinned.sha256 = digest;
    conflicting_fixture(fixture_path);
    assert(afk_sha256_file_hex(fixture_path, digest, err, sizeof err) == 0);
    assert(afk_managed_install(c, &pinned, fixture_path, "Conversion error", (struct progress){0}, &failed, err, sizeof err) == AFK_ENV_ERR_STORAGE);
    assert(strstr(err, "Rootfs conversion failed") && !failed.id[0]);
    assert(rows(c) == 2 && children(path) == 2);
    const char *unsafe[] = {"../escape", "/absolute", "a/../../escape"};
    for (size_t i = 0; i < sizeof unsafe / sizeof *unsafe; i++) {
        fixture(fixture_path, unsafe[i], NULL);
        assert(afk_sha256_file_hex(fixture_path, digest, err, sizeof err) == 0);
        assert(afk_managed_install(c, &pinned, fixture_path, "Unsafe", (struct progress){0}, &failed, err, sizeof err) < 0);
        assert(!failed.id[0] && children(path) == 2);
    }
    char long_name[5000]; memset(long_name, 'a', sizeof long_name - 1); long_name[sizeof long_name - 1] = 0;
    fixture(fixture_path, long_name, NULL);
    assert(afk_sha256_file_hex(fixture_path, digest, err, sizeof err) == 0);
    assert(afk_managed_install(c, &pinned, fixture_path, "Oversized path", (struct progress){0}, &failed, err, sizeof err) < 0);
    fixture(fixture_path, "safe", "../outside");
    assert(afk_sha256_file_hex(fixture_path, digest, err, sizeof err) == 0);
    assert(afk_managed_install(c, &pinned, fixture_path, "Hardlink", (struct progress){0}, &failed, err, sizeof err) < 0);
    /* Valid pin, invalid compressed/tar structure, before creation. */
    f = fopen(fixture_path, "w"); assert(f); fputs("not a tar", f); fclose(f);
    assert(afk_sha256_file_hex(fixture_path, digest, err, sizeof err) == 0);
    assert(afk_managed_install(c, &pinned, fixture_path, "Corrupt", (struct progress){0}, &failed, err, sizeof err) < 0);
    assert(children(path) == 2);
    char active[AFK_ENV_ID_LEN + 1];
    assert(afk_catalog_active(c, active) == 0 && !strcmp(active, one.id));
    f = fopen(sentinel, "r"); assert(f); char saved[16] = {0}; assert(fread(saved, 1, sizeof saved - 1, f) == 8); fclose(f); assert(!strcmp(saved, "preserve"));
    afk_catalog_close(c);
    assert(afk_catalog_open(temp, &c, err, sizeof err) == 0);
    assert(rows(c) == 2);
    assert(afk_ios_boot_root(c, two.id, root, err, sizeof err) == 0);
    afk_catalog_close(c);
    printf("managed-install: real pinned Alpine -> two independent fakefs v3 roots; ELF ARM64, guest symlink metadata, cancellation rollback, unsafe/corrupt archives, sentinel + active identity + reopen preserved\n");
    /* Test runner owns this disposable tree; no user-data paths. */
    char command[8192]; snprintf(command, sizeof command, "rm -rf -- '%s'", temp);
    assert(system(command) == 0);
    return 0;
}
