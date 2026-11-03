#!/bin/sh
# End-to-end check of the environment lifecycle through afk-envctl.
# Proves the G3 acceptance shape (two systems, isolated, independent deletion)
# without claiming that Linux boots: nothing here runs a guest.
set -eu

BIN="${1:?usage: afk_envctl_test.sh /path/to/afk-envctl}"
ROOT="$(mktemp -d "${TMPDIR:-/tmp}/afk-envctl-XXXXXX")"
trap 'rm -rf "$ROOT"' EXIT
export AFK_CATALOG_DIR="$ROOT"

fail() { echo "FAIL: $*" >&2; exit 1; }

id_a="$("$BIN" --root "$ROOT" create --name "Debian A" --distro debian --version 13 --user hardly)"
id_b="$("$BIN" --root "$ROOT" create --name "Debian B" --distro debian --version 13 --user hardly)"
id_c="$("$BIN" --root "$ROOT" create --name "Alpine" --distro alpine --version 3.24 --session desktop)"

[ "$id_a" != "$id_b" ] || fail "ids are not unique"
[ "$(printf '%s' "$id_a" | wc -c)" -eq 36 ] || fail "id is not a 36-character uuid"

path_a="$("$BIN" --root "$ROOT" path "$id_a")"
path_b="$("$BIN" --root "$ROOT" path "$id_b")"
[ "$path_a" != "$path_b" ] || fail "two environments share a path"
[ -d "$path_a/rootfs" ] || fail "environment A has no rootfs directory"
[ -d "$path_b/rootfs" ] || fail "environment B has no rootfs directory"

# Changes in one environment must not appear in the other.
echo "debian-a-only" > "$path_a/rootfs/marker"
[ ! -e "$path_b/rootfs/marker" ] || fail "environment B sees A's file"

# Names stay unique case-insensitively and traversal is rejected.
if "$BIN" --root "$ROOT" create --name "debian a" --distro debian 2>/dev/null; then
    fail "duplicate name accepted"
fi
if "$BIN" --root "$ROOT" create --name "../escape" --distro debian 2>/dev/null; then
    fail "traversal name accepted"
fi
[ ! -e "$ROOT/escape" ] || fail "traversal created something outside environments/"

# Restart: a second process reads the same catalog.
count="$("$BIN" --root "$ROOT" list | grep -c '^[0-9a-f]\{8\}-' || true)"
[ "$count" -eq 3 ] || fail "expected 3 environments after reopening, saw $count"

# The active environment cannot be deleted.
"$BIN" --root "$ROOT" activate "$id_b" > /dev/null
if "$BIN" --root "$ROOT" delete "$id_b" 2>/dev/null; then
    fail "active environment was deleted"
fi
[ -d "$path_b/rootfs" ] || fail "active environment tree disappeared"

# Deleting an inactive environment leaves the others untouched.
"$BIN" --root "$ROOT" delete "$id_a" > /dev/null
[ ! -e "$path_a" ] || fail "environment A tree survived deletion"
[ -f "$path_b/rootfs/marker" ] && fail "environment B gained A's file"
[ -d "$path_b/rootfs" ] || fail "environment B tree was removed with A"

# Rename does not move the filesystem.
"$BIN" --root "$ROOT" rename "$id_b" "Debian Renamed" > /dev/null
[ "$("$BIN" --root "$ROOT" path "$id_b")" = "$path_b" ] || fail "rename moved the filesystem"
"$BIN" --root "$ROOT" list | grep -q "Debian Renamed" || fail "rename not visible"

# Switching environments and deleting the rest works.
"$BIN" --root "$ROOT" activate --none > /dev/null
"$BIN" --root "$ROOT" delete "$id_b" > /dev/null
"$BIN" --root "$ROOT" delete "$id_c" > /dev/null
remaining="$("$BIN" --root "$ROOT" list | grep -c '^[0-9a-f]\{8\}-' || true)"
[ "$remaining" -eq 0 ] || fail "expected an empty catalog, saw $remaining"

echo "envctl-test: PASS (3 environments created, isolated, renamed, deleted independently)"
