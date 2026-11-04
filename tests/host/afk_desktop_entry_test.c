/*
 * Host regression for the .desktop launcher reader.
 * usage: afk_apps_test /path/to/fixtures/dir   (from fetch-desktop-fixtures.sh)
 */
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/applications/afk_desktop_entry.h"

struct collected {
    int count;
    char names[32][AFK_APP_NAME_LEN];
    char execs[32][AFK_APP_EXEC_LEN];
};

static void collect(const struct afk_app_entry *entry, void *context) {
    struct collected *found = context;
    assert(found->count < 32);
    snprintf(found->names[found->count], AFK_APP_NAME_LEN, "%s", entry->name);
    snprintf(found->execs[found->count], AFK_APP_EXEC_LEN, "%s", entry->exec);
    found->count++;
}

static int has_name(const struct collected *found, const char *name) {
    for (int i = 0; i < found->count; i++)
        if (strcmp(found->names[i], name) == 0) return 1;
    return 0;
}

static void expect_parse(const char *text, const char *id, const char *expect_name,
                         const char *expect_exec, int expect_list) {
    struct afk_app_entry entry;
    char err[256] = {0};
    assert(afk_app_parse(text, strlen(text), id, &entry, err, sizeof err) == AFK_APP_OK);
    if (strcmp(entry.name, expect_name) != 0)
        fprintf(stderr, "%s: name was '%s', wanted '%s'\n", id, entry.name, expect_name);
    assert(strcmp(entry.name, expect_name) == 0);
    if (strcmp(entry.exec, expect_exec) != 0)
        fprintf(stderr, "%s: exec was '%s', wanted '%s'\n", id, entry.exec, expect_exec);
    assert(strcmp(entry.exec, expect_exec) == 0);
    assert(afk_app_should_list(&entry) == expect_list);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s /path/to/fixture/entries\n", argv[0]);
        return 2;
    }

    /* --- real files from Debian packages ------------------------------------ */
    struct collected found;
    memset(&found, 0, sizeof found);
    int skipped = -1;
    char err[512] = {0};
    int listed = afk_app_scan_dir(argv[1], collect, &found, &skipped, err, sizeof err);
    assert(listed >= 4);
    assert(listed == found.count);
    assert(skipped == 0);

    for (int i = 0; i < found.count; i++) {
        assert(found.execs[i][0] != '\0');
        /* Field codes must never survive into something we exec. */
        assert(strchr(found.execs[i], '%') == NULL);
        assert(found.names[i][0] != '\0');
    }

    /* Konsole keeps its Name in every locale: we must take the untagged one. */
    assert(has_name(&found, "Konsole"));
    assert(has_name(&found, "QTerminal"));

    struct afk_app_entry konsole;
    char path[1024];
    snprintf(path, sizeof path, "%s/org.kde.konsole.desktop", argv[1]);
    assert(afk_app_parse_file(path, &konsole, err, sizeof err) == AFK_APP_OK);
    assert(strcmp(konsole.id, "org.kde.konsole") == 0);
    assert(strcmp(konsole.name, "Konsole") == 0); /* not "Konsole (de)" or an action name */
    assert(strcmp(konsole.exec, "konsole") == 0);
    assert(strcmp(konsole.try_exec, "konsole") == 0);
    assert(strcmp(konsole.icon, "utilities-terminal") == 0);
    assert(strstr(konsole.categories, "TerminalEmulator") != NULL);
    assert(konsole.type_is_application);
    assert(afk_app_should_list(&konsole));

    /* qterminal.desktop has an [Desktop Action Dropdown] group with its own
     * Exec=qterminal --drop and Name=Drop-down terminal. The parser must not
     * borrow either of them for the application itself. */
    struct afk_app_entry qterminal;
    snprintf(path, sizeof path, "%s/qterminal.desktop", argv[1]);
    assert(afk_app_parse_file(path, &qterminal, err, sizeof err) == AFK_APP_OK);
    assert(strcmp(qterminal.name, "QTerminal") == 0);
    assert(strcmp(qterminal.name, "Drop-down terminal") != 0);
    assert(strcmp(qterminal.exec, "qterminal") == 0);
    assert(strcmp(qterminal.exec, "qterminal --drop") != 0);

    /* --- spec edge cases ---------------------------------------------------- */
    expect_parse("[Desktop Entry]\nType=Application\nName=With Field Codes\n"
                 "Exec=myapp %U\nIcon=app\n",
                 "codes", "With Field Codes", "myapp", 1);
    expect_parse("[Desktop Entry]\nType=Application\nName=Multi\n"
                 "Exec=env FOO=1 myapp --flag %F %i\n",
                 "multi", "Multi", "env FOO=1 myapp --flag", 1);
    expect_parse("[Desktop Entry]\nType=Application\nName=Pct\nExec=myapp --rate=100%%\n",
                 "pct", "Pct", "myapp --rate=100%", 1);
    expect_parse("[Desktop Entry]\nType=Application\nName=Hidden App\nExec=hidden\n"
                 "NoDisplay=true\n",
                 "nodisplay", "Hidden App", "hidden", 0);
    expect_parse("[Desktop Entry]\nType=Application\nName=Gone\nExec=gone\nHidden=true\n", "gone",
                 "Gone", "gone", 0);
    expect_parse("[Desktop Entry]\nType=Link\nName=A Link\nURL=https://example.invalid\n", "link",
                 "A Link", "", 0);
    expect_parse("[Desktop Entry]\nType=Application\nName=Terminal App\nExec=top\nTerminal=true\n",
                 "term", "Terminal App", "top", 1);

    /* Localized first, then the untagged key: the untagged one wins regardless
     * of order, and a locale-only file falls back to the id. */
    expect_parse("[Desktop Entry]\nType=Application\nName[de]=Deutsch\nName=English\nExec=app\n",
                 "locale", "English", "app", 1);
    expect_parse("[Desktop Entry]\nType=Application\nName[de]=Nur Deutsch\nExec=app\n",
                 "locale-only", "locale-only", "app", 1);

    /* Keys before the group, or in another group, are not ours. */
    expect_parse("Name=Too Early\n[Desktop Entry]\nType=Application\nName=Right\nExec=app\n",
                 "early", "Right", "app", 1);

    /* A file with no [Desktop Entry] group is an error the scanner can skip. */
    struct afk_app_entry broken;
    const char *action_only = "[Desktop Action X]\nName=Nope\nExec=nope\n";
    assert(afk_app_parse(action_only, strlen(action_only), "nope", &broken, err, sizeof err) ==
           AFK_APP_ERR_NO_ENTRY_GROUP);

    /* One junk file in a directory must not hide the good ones. */
    char junk_dir[] = "/tmp/afk-apps-XXXXXX";
    assert(mkdtemp(junk_dir) != NULL);
    char copy[1024];
    snprintf(copy, sizeof copy, "%s/org.kde.konsole.desktop", junk_dir);
    snprintf(path, sizeof path, "%s/org.kde.konsole.desktop", argv[1]);
    FILE *source = fopen(path, "rb");
    assert(source != NULL);
    FILE *target = fopen(copy, "wb");
    assert(target != NULL);
    char buffer[4096];
    size_t read;
    while ((read = fread(buffer, 1, sizeof buffer, source)) > 0)
        assert(fwrite(buffer, 1, read, target) == read);
    fclose(source);
    fclose(target);
    snprintf(copy, sizeof copy, "%s/broken.desktop", junk_dir);
    target = fopen(copy, "wb");
    assert(target != NULL);
    fputs("Type=Application\nExec=x\n", target);
    fclose(target);
    snprintf(copy, sizeof copy, "%s/notes.txt", junk_dir);
    target = fopen(copy, "wb");
    assert(target != NULL);
    fputs("not a desktop file\n", target);
    fclose(target);

    struct collected mixed;
    memset(&mixed, 0, sizeof mixed);
    skipped = -1;
    assert(afk_app_scan_dir(junk_dir, collect, &mixed, &skipped, err, sizeof err) == 1);
    assert(mixed.count == 1);
    assert(skipped == 1); /* the file without [Desktop Entry] */
    snprintf(copy, sizeof copy, "rm -rf %s", junk_dir);
    assert(system(copy) == 0);

    /* Bad arguments do not crash or report success. */
    assert(afk_app_scan_dir("/nonexistent/dir", collect, &mixed, &skipped, err, sizeof err) ==
           AFK_APP_ERR_IO);
    assert(afk_app_scan_dir(NULL, collect, &mixed, &skipped, err, sizeof err) ==
           AFK_APP_ERR_INVALID);
    assert(afk_app_parse_file("/nonexistent/x.desktop", &broken, err, sizeof err) ==
           AFK_APP_ERR_IO);

    printf("apps-test: PASS (listed=%d skipped=%d from %s)\n", listed, skipped, argv[1]);
    return 0;
}
