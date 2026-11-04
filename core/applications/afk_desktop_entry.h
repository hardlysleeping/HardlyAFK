#ifndef AFK_DESKTOP_ENTRY_H
#define AFK_DESKTOP_ENTRY_H

/*
 * Minimal, deliberate reader for freedesktop .desktop files.
 *
 * The launcher only needs a handful of keys, and the real files in Debian
 * packages are full of traps for a naive parser: localized duplicates
 * (Name[de]=...), extra action groups ([Desktop Action Dropdown] with its own
 * Exec and Name), and field codes in Exec (%U, %f, %i). This reader takes the
 * keys from the [Desktop Entry] group only, ignores localized variants, and
 * strips field codes so the app can exec the line without a shell.
 */

#include <stddef.h>

#define AFK_APP_ID_LEN 128
#define AFK_APP_NAME_LEN 192
#define AFK_APP_EXEC_LEN 512
#define AFK_APP_ICON_LEN 192
#define AFK_APP_CATEGORIES_LEN 256

enum afk_app_result {
    AFK_APP_OK = 0,
    AFK_APP_ERR_INVALID = -40,
    AFK_APP_ERR_IO = -41,
    AFK_APP_ERR_NO_ENTRY_GROUP = -42
};

struct afk_app_entry {
    /* File name without the .desktop suffix, e.g. "org.kde.konsole". */
    char id[AFK_APP_ID_LEN];
    char name[AFK_APP_NAME_LEN];
    char exec[AFK_APP_EXEC_LEN]; /* field codes removed */
    char icon[AFK_APP_ICON_LEN];
    char categories[AFK_APP_CATEGORIES_LEN];
    char try_exec[AFK_APP_EXEC_LEN];
    int type_is_application;
    int terminal;
    int no_display;
    int hidden;
};

/* Parses from memory so callers (and tests) can feed exact bytes. */
int afk_app_parse(const char *contents, size_t length, const char *id,
                  struct afk_app_entry *out, char *err, size_t errlen);

int afk_app_parse_file(const char *path, struct afk_app_entry *out, char *err, size_t errlen);

/*
 * True when the launcher should show this entry: an application, not marked
 * hidden, and with something to execute.
 */
int afk_app_should_list(const struct afk_app_entry *entry);

typedef void (*afk_app_visit)(const struct afk_app_entry *entry, void *context);

/*
 * Scans a directory of .desktop files (not recursive), calling visit for every
 * entry that should be listed. Returns the number listed, or a negative error;
 * unreadable or malformed files are skipped rather than aborting the scan, and
 * their count is reported through skipped when it is not NULL.
 */
int afk_app_scan_dir(const char *directory, afk_app_visit visit, void *context, int *skipped,
                     char *err, size_t errlen);

#endif
