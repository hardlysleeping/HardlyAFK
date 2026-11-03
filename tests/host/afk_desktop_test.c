/*
 * Host regression for the desktop recipes.
 * usage: afk_desktop_test /path/to/trixie-arm64-Packages        (or a plain name list)
 *
 * Takes either a real Debian Packages index or a file with one package name per
 * line, so the same assertions run against live distribution data.
 */
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/desktop/afk_desktop_catalog.h"

struct index {
    char **names;
    size_t count;
};

static void index_free(struct index *idx) {
    for (size_t i = 0; i < idx->count; i++) free(idx->names[i]);
    free(idx->names);
    idx->names = NULL;
    idx->count = 0;
}

static int index_append(struct index *idx, const char *name) {
    char **grown = realloc(idx->names, (idx->count + 1) * sizeof *grown);
    if (!grown) return -1;
    idx->names = grown;
    idx->names[idx->count] = strdup(name);
    if (!idx->names[idx->count]) return -1;
    idx->count++;
    return 0;
}

static void index_load(struct index *idx, const char *path) {
    FILE *file = fopen(path, "r");
    assert(file != NULL);
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    while ((length = getline(&line, &capacity, file)) > 0) {
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
            line[--length] = '\0';
        if (length == 0) continue;
        const char *name = line;
        if (strncmp(name, "Package: ", 9) == 0) {
            name += 9;
        } else {
            /* Skip the other continuation fields of a Packages stanza. */
            if (strchr(name, ':') && strncmp(name, "Package:", 8) != 0) continue;
        }
        while (*name == ' ') name++;
        if (!*name) continue;
        assert(index_append(idx, name) == 0);
    }
    free(line);
    fclose(file);
    assert(idx->count > 1000); /* a real index, not an empty or truncated file */
}

static int lookup(void *context, const char *name) {
    struct index *idx = context;
    for (size_t i = 0; i < idx->count; i++)
        if (strcmp(idx->names[i], name) == 0) return 1;
    return 0;
}

static int rejected(void *context, const char *name) {
    (void) context;
    return strcmp(name, "obox") != 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s /path/to/Packages\n", argv[0]);
        return 2;
    }

    struct index idx = {0};
    index_load(&idx, argv[1]);

    size_t count = 0;
    const struct afk_desktop *all = afk_desktop_all(&count);
    assert(all != NULL);
    assert(count == 3);

    int recommended = 0;
    size_t total_packages = 0;
    char err[512] = {0};

    for (size_t i = 0; i < count; i++) {
        const struct afk_desktop *desktop = &all[i];
        assert(afk_desktop_validate(desktop, NULL, NULL, err, sizeof err) == AFK_DESKTOP_OK);

        /* The recipe must hold up against the distribution that will install it. */
        memset(err, 0, sizeof err);
        int rc = afk_desktop_validate(desktop, lookup, &idx, err, sizeof err);
        if (rc != AFK_DESKTOP_OK) fprintf(stderr, "%s: %s\n", desktop->id, err);
        assert(rc == AFK_DESKTOP_OK);

        for (size_t j = 0; j < count; j++)
            if (j != i) assert(strcmp(all[i].id, all[j].id) != 0);

        assert(afk_desktop_package_count(desktop) > 0);
        total_packages += afk_desktop_package_count(desktop);
        assert(afk_desktop_has_package(desktop, "xserver-xorg"));
        assert(afk_desktop_has_package(desktop, desktop->session_provider));
        assert(afk_desktop_has_package(desktop, "dbus-x11"));

        /* We are the display manager: no session may drag one in. */
        assert(!afk_desktop_has_package(desktop, "sddm"));
        assert(!afk_desktop_has_package(desktop, "lightdm"));
        assert(!afk_desktop_has_package(desktop, "gdm3"));

        if (desktop->recommended) recommended++;
        printf("compact: %-8s %2zu packages  %s\n", desktop->id,
               afk_desktop_package_count(desktop), desktop->session_command);
    }
    assert(recommended == 1);

    const struct afk_desktop *lxqt = afk_desktop_find("lxqt");
    assert(lxqt != NULL && lxqt->recommended);
    assert(lxqt->kind == AFK_DESKTOP_LXQT);
    assert(afk_desktop_has_package(lxqt, "openbox")); /* X11 window manager */
    assert(strcmp(lxqt->session_command, "startlxqt") == 0);

    const struct afk_desktop *plasma = afk_desktop_find("plasma");
    assert(plasma != NULL);
    assert(afk_desktop_has_package(plasma, "plasma-workspace"));
    assert(afk_desktop_has_package(plasma, "plasma-desktop"));
    assert(strcmp(plasma->session_command, "startplasma-x11") == 0); /* X11, not Wayland */

    assert(afk_desktop_find("gnome") == NULL);
    assert(afk_desktop_find("") == NULL);
    assert(afk_desktop_find(NULL) == NULL);

    /* A package the distribution does not have is reported by name. */
    memset(err, 0, sizeof err);
    assert(afk_desktop_validate(afk_desktop_find("openbox"), rejected, NULL, err, sizeof err) ==
           AFK_DESKTOP_OK);
    struct afk_desktop broken = *afk_desktop_find("openbox");
    static const char *const broken_packages[] = {"openbox", "obox", NULL};
    broken.packages = broken_packages;
    memset(err, 0, sizeof err);
    assert(afk_desktop_validate(&broken, rejected, NULL, err, sizeof err) ==
           AFK_DESKTOP_ERR_UNKNOWN_PACKAGE);
    assert(strstr(err, "obox") != NULL);

    /* Structural failures are refused before any lookup happens. */
    struct afk_desktop no_session = *afk_desktop_find("lxqt");
    no_session.session_command = "";
    assert(afk_desktop_validate(&no_session, lookup, &idx, err, sizeof err) ==
           AFK_DESKTOP_ERR_INVALID);

    struct afk_desktop no_packages = *afk_desktop_find("lxqt");
    static const char *const empty[] = {NULL};
    no_packages.packages = empty;
    assert(afk_desktop_validate(&no_packages, NULL, NULL, err, sizeof err) ==
           AFK_DESKTOP_ERR_INVALID);

    /* A session command whose package is missing would install and then not start. */
    struct afk_desktop orphan_command = *afk_desktop_find("plasma");
    orphan_command.session_provider = "kde-plasma-desktop";
    assert(afk_desktop_validate(&orphan_command, lookup, &idx, err, sizeof err) ==
           AFK_DESKTOP_ERR_INVALID);
    assert(strstr(err, "does not install the package that provides") != NULL);

    struct afk_desktop duplicate = *afk_desktop_find("openbox");
    static const char *const twice[] = {"openbox", "openbox", NULL};
    duplicate.packages = twice;
    assert(afk_desktop_validate(&duplicate, NULL, NULL, err, sizeof err) ==
           AFK_DESKTOP_ERR_INVALID);

    printf("desktop-test: PASS (%zu desktops, %zu package entries, index %zu names)\n", count,
           total_packages, idx.count);
    index_free(&idx);
    return 0;
}
