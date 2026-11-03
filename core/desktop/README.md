# Desktop recipes

Three desktops, as data: `openbox`, `lxqt`, `plasma`. Each carries its package list, the session
command to start, and the package that provides that command.

| id | packages | session | provider |
|---|---:|---|---|
| openbox | 7 | `openbox-session` | openbox |
| lxqt | 12 | `startlxqt` | lxqt-session |
| plasma | 13 | `startplasma-x11` | plasma-workspace |

## Deviations from the plan worth knowing

The plan's repository layout puts these in `/recipes/desktops/*.json`. They are compiled-in
`struct afk_desktop` tables instead, for three reasons: a JSON parser in the core would be a new
dependency whose only job is reading three constants; typed data cannot be malformed at runtime
on a user's phone; and the table is directly testable from a plain `cc` invocation. The app still
never constructs a package command of its own — it reads these definitions.

The plan also says not to use a display manager for normal operation. The recipes encode that: no
desktop lists `sddm`, `lightdm` or `gdm3`, and the regression fails if one appears. We start the
session, the system does not.

## How the package names are kept honest

Every name was checked against the Debian 13 (trixie) `main`, `binary-arm64` Packages index
(68,191 unique packages) before being written down, and the regression re-checks the live index
in CI:

```sh
xz -dc Packages.xz > /tmp/Packages
cc -std=c11 -Wall -Wextra -Werror -I. core/desktop/afk_desktop_catalog.c \
  tests/host/afk_desktop_test.c -o /tmp/afk-desktop-test
/tmp/afk-desktop-test /tmp/Packages
```

If Debian renames or drops a package we list, CI fails naming it:

```
plasma needs kwin-x11, which the distribution does not provide
```

That is deliberately coupled to the live archive rather than to a pinned snapshot: a stale pin
would keep passing while the install broke on a device. The trade is that an unrelated upstream
rename can turn a green PR red — which is the correct time to notice.

`session_command` is a binary, not a package: `startlxqt` ships inside `lxqt-session` and
`startplasma-x11` inside `plasma-workspace`. `session_provider` records that mapping, and
validation refuses a recipe whose command package it does not install — otherwise the desktop
would install fine and then fail to start.

## What is not proven yet

These are package sets and launch commands, not a working desktop. Nothing here has run a session:
no Xvfb, no window manager, no Qt, no Plasma. G5–G8 remain device-gated.
