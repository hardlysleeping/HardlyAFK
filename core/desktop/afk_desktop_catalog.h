#ifndef AFK_DESKTOP_CATALOG_H
#define AFK_DESKTOP_CATALOG_H

/*
 * Desktops HardlyAFK can install, as data.
 *
 * The app never runs apt itself: it shows these definitions, and the installer
 * feeds the package list to the guest. Keeping the recipes here means the UI
 * cannot invent a package name, and CI can prove every name still exists in the
 * distribution it targets.
 *
 * We are the session launcher (see the plan: no SDDM/LightDM/GDM for normal
 * operation), so each definition names the session command to start rather than
 * relying on a display manager.
 */

#include <stddef.h>

enum afk_desktop_kind {
    AFK_DESKTOP_OPENBOX = 0, /* minimal window manager, the GUI smoke test */
    AFK_DESKTOP_LXQT = 1,    /* recommended desktop */
    AFK_DESKTOP_PLASMA = 2   /* full desktop, X11 session */
};

enum afk_desktop_result {
    AFK_DESKTOP_OK = 0,
    AFK_DESKTOP_ERR_INVALID = -30,
    AFK_DESKTOP_ERR_NOT_FOUND = -31,
    AFK_DESKTOP_ERR_UNKNOWN_PACKAGE = -32
};

struct afk_desktop {
    const char *id;
    const char *display_name;
    const char *summary;
    enum afk_desktop_kind kind;
    /* NULL-terminated; every entry must be a real package name. */
    const char *const *packages;
    /* Started by the app, never by a display manager. */
    const char *session_command;
    /* The package that provides session_command, so CI can check it is pulled in. */
    const char *session_provider;
    int recommended;
};

/* Returns 1 when the package exists. Used to validate against a real index. */
typedef int (*afk_package_lookup)(void *context, const char *name);

const struct afk_desktop *afk_desktop_all(size_t *count);
const struct afk_desktop *afk_desktop_find(const char *id);
size_t afk_desktop_package_count(const struct afk_desktop *desktop);

/*
 * Structural checks plus a lookup of every package. Returns AFK_DESKTOP_OK, or
 * AFK_DESKTOP_ERR_UNKNOWN_PACKAGE naming the first package the lookup rejected.
 * lookup may be NULL to run only the structural checks.
 */
int afk_desktop_validate(const struct afk_desktop *desktop, afk_package_lookup lookup, void *context,
                         char *err, size_t errlen);

/* True when the distribution's package list already includes name. */
int afk_desktop_has_package(const struct afk_desktop *desktop, const char *name);

#endif
