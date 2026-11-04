# Application launcher

`afk_desktop_entry.h` reads freedesktop `.desktop` files so the app can list what is installed in
an environment and exec it, without asking the user to know a command.

Result band: `-40..-42`, following the project rule that each subsystem owns its own range.

## What it reads, and what it refuses to be fooled by

Only keys inside `[Desktop Entry]`. Real Debian files make that a requirement rather than a
nicety:

- `konsole.desktop` carries `Name` and `Keywords` translated into ~40 locales. A parser that
  takes the last match shows a Japanese name on an English phone; this one ignores every
  `Key[locale]` and uses the untagged key.
- `qterminal.desktop` has an `[Desktop Action Dropdown]` group with its own `Exec=qterminal --drop`
  and `Name=Drop-down terminal`. Scanning the whole file would give the launcher the drop-down
  variant and the wrong label. The test asserts both, by name.
- `Exec` field codes (`%U`, `%F`, `%i`, `%c`, `%k`) are stripped, because they are placeholders
  for a file manager to fill in, not arguments to exec. `%%` becomes `%`.

Listing rules: `Type=Application`, not `NoDisplay`, not `Hidden`, and a non-empty `Exec`. A
`Terminal=true` app is still listed — the app decides whether to open it in a terminal tab.

## Fixtures come from real packages

Hand-written fixtures drift from reality, so the tests extract `.desktop` files out of Debian 13
arm64 packages resolved through the live `Packages` index:

```sh
sh tests/host/fetch-desktop-fixtures.sh /tmp/fixtures
cc -std=c11 -Wall -Wextra -Werror -I. core/applications/afk_desktop_entry.c \
  tests/host/afk_desktop_entry_test.c -o /tmp/afk-apps-test
/tmp/afk-apps-test /tmp/fixtures/entries
```

`qterminal`, `konsole`, `pcmanfm-qt` and `lximage-qt` are pulled, which is enough to cover a
Qt/KDE terminal, a file manager and a settings dialog. The fixture fetcher is test-only: nothing
here downloads at runtime, and the app never parses a package.

## What is not proven yet

Parsing only. Nothing launches: there is no process, no display, no terminal yet on a device, and
no scan of a real guest rootfs — the fixtures are extracted from packages rather than read through
fakefs from a running Linux environment.
