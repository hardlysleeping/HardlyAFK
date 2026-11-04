#include "afk_desktop_entry.h"

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void app_error(char *err, size_t errlen, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static void app_error(char *err, size_t errlen, const char *format, ...) {
    if (!err || errlen == 0) return;
    va_list args;
    va_start(args, format);
    vsnprintf(err, errlen, format, args);
    va_end(args);
}

static void copy_field(char *out, size_t outlen, const char *value) {
    if (!out || outlen == 0) return;
    size_t length = strlen(value);
    if (length >= outlen) length = outlen - 1;
    memcpy(out, value, length);
    out[length] = '\0';
}

/* "true"/"false" per the spec; anything else is false. */
static int parse_bool(const char *value) {
    return strcmp(value, "true") == 0;
}

/* Keys may carry a locale suffix: Name[de], Keywords[pt_BR]. Those are not ours. */
static int has_locale_suffix(const char *key) {
    return strchr(key, '[') != NULL;
}

/*
 * Removes field codes (%f %F %u %U %d %D %n %N %i %c %k %v %m %%) and collapses
 * the whitespace they leave behind, so the result can be executed directly.
 */
static void strip_field_codes(const char *value, char *out, size_t outlen) {
    size_t written = 0;
    for (size_t i = 0; value[i] && written + 1 < outlen; i++) {
        if (value[i] == '%') {
            char next = value[i + 1];
            if (next == '\0') break;
            i++; /* skip the code */
            if (next == '%') {
                out[written++] = '%';
                continue;
            }
            /* Drop the space that separated the code from the command. */
            while (written > 0 && out[written - 1] == ' ') written--;
            continue;
        }
        out[written++] = value[i];
    }
    while (written > 0 && out[written - 1] == ' ') written--;
    out[written] = '\0';
}

int afk_app_parse(const char *contents, size_t length, const char *id,
                  struct afk_app_entry *out, char *err, size_t errlen) {
    if (out) memset(out, 0, sizeof *out);
    if (!contents || !out || !id || !*id) {
        app_error(err, errlen, "contents, an id and an output entry are required");
        return AFK_APP_ERR_INVALID;
    }
    copy_field(out->id, sizeof out->id, id);

    int in_entry_group = 0;
    int saw_entry_group = 0;
    int have_exec = 0;

    char *copy = malloc(length + 1);
    if (!copy) {
        app_error(err, errlen, "out of memory");
        return AFK_APP_ERR_INVALID;
    }
    memcpy(copy, contents, length);
    copy[length] = '\0';

    char *cursor = copy;
    while (cursor && *cursor) {
        char *line = cursor;
        char *newline = strchr(cursor, '\n');
        if (newline) {
            *newline = '\0';
            cursor = newline + 1;
        } else {
            cursor = NULL;
        }

        size_t line_length = strlen(line);
        while (line_length > 0 && (line[line_length - 1] == '\r' || line[line_length - 1] == ' '))
            line[--line_length] = '\0';
        while (*line == ' ' || *line == '\t') line++;

        if (*line == '\0' || *line == '#') continue;

        if (*line == '[') {
            char *close = strchr(line, ']');
            if (!close) continue;
            *close = '\0';
            in_entry_group = strcmp(line + 1, "Desktop Entry") == 0;
            if (in_entry_group) saw_entry_group = 1;
            continue;
        }
        /* Keys outside [Desktop Entry] belong to action groups and are ignored. */
        if (!in_entry_group) continue;

        char *equals = strchr(line, '=');
        if (!equals) continue;
        *equals = '\0';
        char *key = line;
        char *value = equals + 1;
        if (has_locale_suffix(key)) continue;

        if (strcmp(key, "Name") == 0) {
            copy_field(out->name, sizeof out->name, value);
        } else if (strcmp(key, "Exec") == 0) {
            strip_field_codes(value, out->exec, sizeof out->exec);
            have_exec = 1;
        } else if (strcmp(key, "Icon") == 0) {
            copy_field(out->icon, sizeof out->icon, value);
        } else if (strcmp(key, "Categories") == 0) {
            copy_field(out->categories, sizeof out->categories, value);
        } else if (strcmp(key, "TryExec") == 0) {
            copy_field(out->try_exec, sizeof out->try_exec, value);
        } else if (strcmp(key, "Type") == 0) {
            out->type_is_application = strcmp(value, "Application") == 0;
        } else if (strcmp(key, "Terminal") == 0) {
            out->terminal = parse_bool(value);
        } else if (strcmp(key, "NoDisplay") == 0) {
            out->no_display = parse_bool(value);
        } else if (strcmp(key, "Hidden") == 0) {
            out->hidden = parse_bool(value);
        }
    }
    free(copy);

    if (!saw_entry_group) {
        app_error(err, errlen, "%s has no [Desktop Entry] group", id);
        return AFK_APP_ERR_NO_ENTRY_GROUP;
    }
    /* A file with only action groups still lands here as not-an-application. */
    if (!have_exec) out->exec[0] = '\0';
    if (!out->name[0]) copy_field(out->name, sizeof out->name, out->id);
    return AFK_APP_OK;
}

int afk_app_parse_file(const char *path, struct afk_app_entry *out, char *err, size_t errlen) {
    if (out) memset(out, 0, sizeof *out);
    if (!path || !*path) {
        app_error(err, errlen, "a path is required");
        return AFK_APP_ERR_INVALID;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        app_error(err, errlen, "cannot open %s", path);
        return AFK_APP_ERR_IO;
    }
    size_t capacity = 8192;
    size_t used = 0;
    char *buffer = malloc(capacity);
    if (!buffer) {
        fclose(file);
        app_error(err, errlen, "out of memory");
        return AFK_APP_ERR_INVALID;
    }
    for (;;) {
        if (used + 4096 + 1 > capacity) {
            size_t grown = capacity * 2;
            char *bigger = realloc(buffer, grown);
            if (!bigger) {
                free(buffer);
                fclose(file);
                app_error(err, errlen, "out of memory");
                return AFK_APP_ERR_INVALID;
            }
            buffer = bigger;
            capacity = grown;
        }
        size_t read = fread(buffer + used, 1, capacity - used - 1, file);
        used += read;
        if (read == 0) break;
    }
    int failed = ferror(file);
    fclose(file);
    if (failed) {
        free(buffer);
        app_error(err, errlen, "cannot read %s", path);
        return AFK_APP_ERR_IO;
    }
    buffer[used] = '\0';

    /* id = file name without .desktop */
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char id[AFK_APP_ID_LEN];
    copy_field(id, sizeof id, base);
    char *dot = strstr(id, ".desktop");
    if (dot) *dot = '\0';

    int rc = afk_app_parse(buffer, used, id, out, err, errlen);
    free(buffer);
    return rc;
}

int afk_app_should_list(const struct afk_app_entry *entry) {
    if (!entry) return 0;
    if (!entry->type_is_application) return 0;
    if (entry->hidden || entry->no_display) return 0;
    if (!entry->exec[0]) return 0;
    return 1;
}

int afk_app_scan_dir(const char *directory, afk_app_visit visit, void *context, int *skipped,
                     char *err, size_t errlen) {
    if (skipped) *skipped = 0;
    if (!directory || !*directory || !visit) {
        app_error(err, errlen, "a directory and a visitor are required");
        return AFK_APP_ERR_INVALID;
    }
    DIR *dir = opendir(directory);
    if (!dir) {
        app_error(err, errlen, "cannot open %s", directory);
        return AFK_APP_ERR_IO;
    }
    int listed = 0;
    int bad = 0;
    struct dirent *item;
    while ((item = readdir(dir)) != NULL) {
        size_t name_length = strlen(item->d_name);
        if (name_length < 9) continue;
        if (strcmp(item->d_name + name_length - 8, ".desktop") != 0) continue;

        char path[1024];
        int written = snprintf(path, sizeof path, "%s/%s", directory, item->d_name);
        if (written < 0 || (size_t) written >= sizeof path) {
            bad++;
            continue;
        }
        struct afk_app_entry entry;
        char ignored[128] = {0};
        if (afk_app_parse_file(path, &entry, ignored, sizeof ignored) != AFK_APP_OK) {
            bad++; /* one broken file must not hide the rest */
            continue;
        }
        if (!afk_app_should_list(&entry)) continue;
        visit(&entry, context);
        listed++;
    }
    closedir(dir);
    if (skipped) *skipped = bad;
    return listed;
}
