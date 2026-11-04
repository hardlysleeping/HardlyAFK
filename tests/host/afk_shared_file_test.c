#define _POSIX_C_SOURCE 200809L
#include "core/shared/afk_shared_file.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void content(int root, const char *path, const char *expected) {
    int fd = -1;
    assert(afk_shared_open(root, path, AFK_SHARED_READ, &fd) == AFK_SHARED_OK);
    char buf[64] = {0};
    assert(read(fd, buf, sizeof buf - 1) == (ssize_t)strlen(expected));
    assert(strcmp(buf, expected) == 0);
    assert((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
    assert(close(fd) == 0);
}
static void refused(int root, const char *path, enum afk_shared_operation op) {
    int fd = 123;
    assert(afk_shared_open(root, path, op, &fd) < 0);
    assert(fd == -1);
}
int main(void) {
    const char *tmp = getenv("TMPDIR");
    assert(tmp && *tmp); /* Never silently use system /tmp. */
    char fixture[4096];
    assert(snprintf(fixture, sizeof fixture, "%s/afk-shared-XXXXXX", tmp) < (int)sizeof fixture);
    assert(mkdtemp(fixture));
    int base = open(fixture, O_RDONLY | O_DIRECTORY);
    assert(base >= 0);
    assert(mkdirat(base, "env-a", 0700) == 0);
    assert(mkdirat(base, "env-b", 0700) == 0);
    int a = openat(base, "env-a", O_RDONLY | O_DIRECTORY);
    int b = openat(base, "env-b", O_RDONLY | O_DIRECTORY);
    assert(a >= 0 && b >= 0);
    assert(mkdirat(a, "nested", 0700) == 0);
    int fd = -1;
    assert(afk_shared_open(a, "nested/file", AFK_SHARED_CREATE_EXCLUSIVE, &fd) == 0);
    struct stat st;
    assert(fstat(fd, &st) == 0 && (st.st_mode & 0777 & ~0600) == 0);
    assert(write(fd, "alpha", 5) == 5);
    assert(close(fd) == 0);
    assert(afk_shared_open(b, "file", AFK_SHARED_CREATE_EXCLUSIVE, &fd) == 0);
    assert(write(fd, "bravo", 5) == 5);
    assert(close(fd) == 0);
    content(a, "nested/file", "alpha");
    content(b, "file", "bravo");
    refused(a, "file", AFK_SHARED_READ);
    refused(b, "nested/file", AFK_SHARED_READ);
    refused(a, "nested/file", AFK_SHARED_CREATE_EXCLUSIVE);
    content(a, "nested/file", "alpha");
    assert(afk_shared_open(a, "nested/file", AFK_SHARED_WRITE, &fd) == 0);
    assert(write(fd, "A", 1) == 1);
    assert(close(fd) == 0);
    content(a, "nested/file", "Alpha"); /* Existing write does not truncate. */
    assert(afk_shared_open(a, "nested/file", AFK_SHARED_READ_WRITE, &fd) == 0);
    assert(pwrite(fd, "!", 1, 4) == 1);
    assert(close(fd) == 0);
    content(a, "nested/file", "Alph!");

    const char *bad[] = {"", "/mnt/shared/file", "/etc/passwd", ".", "..",
        "../env-b/file", "nested/../file", "nested/./file", "nested//file",
        "nested/", "nested/..", "nested/new/../../oops"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        refused(a, bad[i], AFK_SHARED_READ);
        refused(a, bad[i], AFK_SHARED_CREATE_EXCLUSIVE);
    }
    refused(a, NULL, AFK_SHARED_READ);
    refused(-1, "file", AFK_SHARED_READ);
    assert(afk_shared_open(a, "nested/file", AFK_SHARED_READ, NULL) == AFK_SHARED_ERR_INVALID);
    const int invalid[] = {-1, 4, 99, O_WRONLY | O_TRUNC, O_CREAT | O_TRUNC, O_APPEND};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i)
        refused(a, "nested/file", (enum afk_shared_operation)invalid[i]);
    content(a, "nested/file", "Alph!");

    assert(symlinkat("../env-b", a, "escape") == 0);
    assert(symlinkat("../env-b/file", a, "link") == 0);
    assert(symlinkat("nested/file", a, "inside") == 0);
    assert(symlinkat("/does-not-exist-afk", a, "dangling") == 0);
    assert(symlinkat(fixture, a, "absolute") == 0);
    const char *links[] = {"escape/file", "link", "inside", "dangling", "absolute/env-b/file"};
    for (size_t i = 0; i < sizeof links / sizeof links[0]; ++i) {
        refused(a, links[i], AFK_SHARED_READ);
        refused(a, links[i], AFK_SHARED_WRITE);
        refused(a, links[i], AFK_SHARED_CREATE_EXCLUSIVE);
    }
    content(b, "file", "bravo");
    assert(mkfifoat(a, "fifo", 0600) == 0);
    refused(a, "fifo", AFK_SHARED_READ);
    refused(a, "fifo", AFK_SHARED_WRITE);
    refused(a, "nested", AFK_SHARED_WRITE);
    refused(a, "missing", AFK_SHARED_WRITE);
    content(a, "nested/file", "Alph!");
    assert(fcntl(a, F_GETFD) >= 0 && fcntl(b, F_GETFD) >= 0);

    const char *remove[] = {"escape", "link", "inside", "dangling", "absolute", "fifo", "nested/file"};
    for (size_t i = 0; i < sizeof remove / sizeof remove[0]; ++i)
        assert(unlinkat(a, remove[i], 0) == 0);
    assert(unlinkat(a, "nested", AT_REMOVEDIR) == 0);
    assert(unlinkat(b, "file", 0) == 0);
    assert(close(a) == 0 && close(b) == 0);
    assert(unlinkat(base, "env-a", AT_REMOVEDIR) == 0);
    assert(unlinkat(base, "env-b", AT_REMOVEDIR) == 0);
    assert(close(base) == 0 && rmdir(fixture) == 0);
    puts("shared-file: operations, isolation, traversal, symlinks, flags, non-clobber and special-node checks passed");
    return 0;
}
