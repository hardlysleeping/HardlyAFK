#include "afk_desktop_catalog.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*
 * Every package below was checked against Debian 13 (trixie) main,
 * binary-arm64, Packages index (68,191 packages). The regression in
 * tests/host/afk_desktop_test.c repeats that check against the live index, so a
 * renamed or dropped package fails CI instead of failing on a user's phone.
 *
 * Session commands are binaries, not packages: startlxqt ships in lxqt-session
 * and startplasma-x11 ships in plasma-workspace, which is why each definition
 * also names the package that provides its command.
 *
 * No desktop pulls a display manager: the app starts the session itself.
 */
static const char *const openbox_packages[] = {
    "openbox",
    "xserver-xorg",
    "xinit",
    "xterm",
    "dbus-x11",
    "x11-utils",
    "fonts-dejavu-core",
    NULL,
};

static const char *const lxqt_packages[] = {
    "lxqt",
    "lxqt-session",
    "qterminal",
    "pcmanfm-qt",
    "openbox",
    "xserver-xorg",
    "xinit",
    "dbus-x11",
    "libgl1-mesa-dri",
    "fonts-dejavu-core",
    "xdg-utils",
    "x11-xserver-utils",
    NULL,
};

static const char *const plasma_packages[] = {
    "plasma-desktop",
    "plasma-workspace",
    "systemsettings",
    "konsole",
    "dolphin",
    "xserver-xorg",
    "xinit",
    "dbus-x11",
    "libgl1-mesa-dri",
    "fonts-noto-core",
    "xdg-utils",
    "x11-xserver-utils",
    "kde-config-gtk-style",
    NULL,
};

static const struct afk_desktop kDesktops[] = {
    {
        .id = "openbox",
        .display_name = "Openbox",
        .summary = "Minimal window manager",
        .kind = AFK_DESKTOP_OPENBOX,
        .packages = openbox_packages,
        .session_command = "openbox-session",
        .session_provider = "openbox",
        .recommended = 0,
    },
    {
        .id = "lxqt",
        .display_name = "LXQt",
        .summary = "Lightweight, X11 friendly, small memory footprint",
        .kind = AFK_DESKTOP_LXQT,
        .packages = lxqt_packages,
        .session_command = "startlxqt",
        .session_provider = "lxqt-session",
        .recommended = 1,
    },
    {
        .id = "plasma",
        .display_name = "KDE Plasma",
        .summary = "Full desktop, X11 session",
        .kind = AFK_DESKTOP_PLASMA,
        .packages = plasma_packages,
        .session_command = "startplasma-x11",
        .session_provider = "plasma-workspace",
        .recommended = 0,
    },
};

const struct afk_desktop *afk_desktop_all(size_t *count) {
    if (count) *count = sizeof kDesktops / sizeof kDesktops[0];
    return kDesktops;
}

const struct afk_desktop *afk_desktop_find(const char *id) {
    if (!id || !*id) return NULL;
    size_t count = 0;
    const struct afk_desktop *all = afk_desktop_all(&count);
    for (size_t i = 0; i < count; i++)
        if (strcmp(all[i].id, id) == 0) return &all[i];
    return NULL;
}

size_t afk_desktop_package_count(const struct afk_desktop *desktop) {
    if (!desktop || !desktop->packages) return 0;
    size_t count = 0;
    while (desktop->packages[count]) count++;
    return count;
}

int afk_desktop_has_package(const struct afk_desktop *desktop, const char *name) {
    if (!desktop || !desktop->packages || !name) return 0;
    for (size_t i = 0; desktop->packages[i]; i++)
        if (strcmp(desktop->packages[i], name) == 0) return 1;
    return 0;
}

static void desktop_error(char *err, size_t errlen, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static void desktop_error(char *err, size_t errlen, const char *format, ...) {
    if (!err || errlen == 0) return;
    va_list args;
    va_start(args, format);
    vsnprintf(err, errlen, format, args);
    va_end(args);
}

/* A Debian package name: lowercase, no whitespace, no leading punctuation. */
static int valid_package_name(const char *name) {
    if (!name || !*name) return 0;
    if (name[0] == '-' || name[0] == '.') return 0;
    for (const char *c = name; *c; c++) {
        int ok = (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' ||
                 *c == '+' || *c == '.' || *c == '~';
        if (!ok) return 0;
    }
    return 1;
}

int afk_desktop_validate(const struct afk_desktop *desktop, afk_package_lookup lookup, void *context,
                         char *err, size_t errlen) {
    if (!desktop) {
        desktop_error(err, errlen, "a desktop definition is required");
        return AFK_DESKTOP_ERR_INVALID;
    }
    if (!desktop->id || !*desktop->id || strchr(desktop->id, ' ')) {
        desktop_error(err, errlen, "desktop id is missing or contains a space");
        return AFK_DESKTOP_ERR_INVALID;
    }
    if (!desktop->display_name || !*desktop->display_name) {
        desktop_error(err, errlen, "%s has no display name", desktop->id);
        return AFK_DESKTOP_ERR_INVALID;
    }
    if (!desktop->packages || !desktop->packages[0]) {
        desktop_error(err, errlen, "%s installs no packages", desktop->id);
        return AFK_DESKTOP_ERR_INVALID;
    }
    if (!desktop->session_command || !*desktop->session_command) {
        desktop_error(err, errlen, "%s has no session command", desktop->id);
        return AFK_DESKTOP_ERR_INVALID;
    }
    if (!desktop->session_provider || !afk_desktop_has_package(desktop, desktop->session_provider)) {
        desktop_error(err, errlen, "%s does not install the package that provides %s", desktop->id,
                      desktop->session_command);
        return AFK_DESKTOP_ERR_INVALID;
    }

    size_t count = afk_desktop_package_count(desktop);
    for (size_t i = 0; i < count; i++) {
        const char *name = desktop->packages[i];
        if (!valid_package_name(name)) {
            desktop_error(err, errlen, "%s lists an invalid package name: %s", desktop->id, name);
            return AFK_DESKTOP_ERR_INVALID;
        }
        for (size_t j = i + 1; j < count; j++) {
            if (strcmp(name, desktop->packages[j]) == 0) {
                desktop_error(err, errlen, "%s lists %s twice", desktop->id, name);
                return AFK_DESKTOP_ERR_INVALID;
            }
        }
        if (lookup && !lookup(context, name)) {
            desktop_error(err, errlen, "%s needs %s, which the distribution does not provide",
                          desktop->id, name);
            return AFK_DESKTOP_ERR_UNKNOWN_PACKAGE;
        }
    }
    return AFK_DESKTOP_OK;
}
