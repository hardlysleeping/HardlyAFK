#ifndef AFK_SHARED_FILE_H
#define AFK_SHARED_FILE_H

#define AFK_SHARED_GUEST_MOUNT "/mnt/shared"

enum afk_shared_result {
    AFK_SHARED_OK = 0,
    AFK_SHARED_ERR_INVALID = -50,
    AFK_SHARED_ERR_IO = -51,
    AFK_SHARED_ERR_TYPE = -52
};

/* These are operation selectors, NOT host open(2) flags or combinable bits. */
enum afk_shared_operation {
    AFK_SHARED_READ = 0,
    AFK_SHARED_WRITE = 1,
    AFK_SHARED_READ_WRITE = 2,
    AFK_SHARED_CREATE_EXCLUSIVE = 3
};

/* Host supplies an already approved directory descriptor (borrowed, not closed).
 * relative_path is relative to that directory, NOT a guest absolute path.
 * On success *out_fd is a caller-owned regular-file descriptor with CLOEXEC.
 * Existing-file writes start at offset zero without truncation. Exclusive create
 * returns a read/write descriptor with mode 0600 subject to umask.
 * On failure *out_fd is -1 (when non-NULL). No raw open flags are accepted.
 * See README for trusted-directory, hardlink and concurrent-rename assumptions.
 */
int afk_shared_open(int approved_directory_fd, const char *relative_path,
                    enum afk_shared_operation operation, int *out_fd);

#endif
