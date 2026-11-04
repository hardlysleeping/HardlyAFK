#define _POSIX_C_SOURCE 200809L
#include "afk_shared_file.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Validate the ENTIRE path before any filesystem operation (including create). */
static int valid_path(const char *path) {
    if (!path || !*path || *path == '/') return 0;
    const char *start = path;
    for (const char *p = path;; ++p) {
        if (*p != '/' && *p != '\0') continue;
        size_t n = (size_t)(p - start);
        if (n == 0 || (n == 1 && start[0] == '.') ||
            (n == 2 && start[0] == '.' && start[1] == '.')) return 0;
        if (*p == '\0') return 1;
        start = p + 1;
    }
}

int afk_shared_open(int approved_directory_fd, const char *relative_path,
                    enum afk_shared_operation operation, int *out_fd) {
    if (out_fd) *out_fd = -1;
    if (!out_fd || approved_directory_fd < 0 || !valid_path(relative_path) ||
        (operation != AFK_SHARED_READ && operation != AFK_SHARED_WRITE &&
         operation != AFK_SHARED_READ_WRITE && operation != AFK_SHARED_CREATE_EXCLUSIVE))
        return AFK_SHARED_ERR_INVALID;

    /* openat(".") obtains our own CLOEXEC directory descriptor, without sharing
     * ownership of the caller's fd. All subsequent opens are descriptor-relative. */
    int dir = openat(approved_directory_fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) return AFK_SHARED_ERR_IO;
    char *path = strdup(relative_path);
    if (!path) { close(dir); return AFK_SHARED_ERR_IO; }
    int result = AFK_SHARED_ERR_IO;
    char *leaf = path;
    char *slash;
    while ((slash = strchr(leaf, '/')) != NULL) {
        *slash = '\0';
        int next = openat(dir, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) goto done;
        close(dir);
        dir = next;
        leaf = slash + 1;
    }

    struct stat st;
    int flags = O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK;
    if (operation == AFK_SHARED_CREATE_EXCLUSIVE) {
        flags |= O_RDWR | O_CREAT | O_EXCL;
    } else {
        /* Refuse known special nodes before opening them. The trusted-root
         * contract excludes concurrent replacement with devices (see README). */
        if (fstatat(dir, leaf, &st, AT_SYMLINK_NOFOLLOW) != 0) goto done;
        if (!S_ISREG(st.st_mode)) { result = AFK_SHARED_ERR_TYPE; goto done; }
        flags |= operation == AFK_SHARED_READ ? O_RDONLY :
                 operation == AFK_SHARED_WRITE ? O_WRONLY : O_RDWR;
    }
    int fd = openat(dir, leaf, flags, 0600);
    if (fd < 0) goto done;
    if (fstat(fd, &st) != 0) { close(fd); goto done; }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        result = AFK_SHARED_ERR_TYPE;
        goto done;
    }
    /* O_NONBLOCK prevents a raced FIFO from blocking; regular files don't need it. */
    int current = fcntl(fd, F_GETFL);
    if (current < 0 || fcntl(fd, F_SETFL, current & ~O_NONBLOCK) < 0) {
        close(fd);
        goto done;
    }
    *out_fd = fd;
    result = AFK_SHARED_OK;
done:
    free(path);
    close(dir);
    return result;
}
