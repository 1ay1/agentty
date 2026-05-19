#!/bin/sh
# scripts/bump.sh — one-line release: bump → build everything → tag → publish.
#
# Usage:
#   scripts/bump.sh 0.2.0          # full release
#   scripts/bump.sh 0.2.0 --dry    # everything except `git push` and `gh release`
#
# Flow:
#   1. Verify the working tree is clean (no uncommitted changes outside CMakeLists).
#   2. Rewrite `project(agentty VERSION X.Y.Z ...)` in CMakeLists.txt.
#   3. `cmake --build build -j` (sanity: the new version still compiles).
#   4. git commit "release: vX.Y.Z" + git tag vX.Y.Z.
#   5. scripts/release.sh --tag vX.Y.Z (builds every artifact + uploads via gh).
#   6. git push origin master --tags.
#
# Single source of truth: `CMakeLists.txt`. Everything downstream — User-Agent
# strings baked into the binary, deb/rpm/arch/scoop/homebrew manifests,
# install.sh's `--version v…` resolver, the release tag itself — derives
# from that one line.

set -eu

NEW_VERSION=${1:-}
DRY=0
[ "${2:-}" = "--dry" ] && DRY=1

if [ -z "$NEW_VERSION" ] || ! echo "$NEW_VERSION" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then
    echo "usage: bump.sh <major.minor.patch> [--dry]" >&2
    echo "  e.g. bump.sh 0.2.0" >&2
    exit 2
fi

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

# ---- ui ----------------------------------------------------------------------
hr()   { printf '\n\033[1;34m== %s ==\033[0m\n' "$*"; }
info() { printf '\033[1;34m::\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m\xe2\x9c\x93\033[0m %s\n' "$*"; }
err()  { printf '\033[1;31m\xe2\x9c\x97\033[0m %s\n' "$*" >&2; exit 1; }

# ---- 1. tree clean -----------------------------------------------------------
hr "1/6  preflight"
current=$(sed -nE 's/.*project\(agentty VERSION ([0-9.]+).*/\1/p' CMakeLists.txt | head -1)
[ -n "$current" ] || err "could not read current VERSION from CMakeLists.txt"
info "$current  ->  $NEW_VERSION"

# Allow changes confined to CMakeLists.txt (in case bump.sh is rerun); reject
# anything else dirty so we don't accidentally tag a half-finished feature.
dirty=$(git status --porcelain | grep -v ' CMakeLists.txt$' | grep -v '^?? ' || true)
[ -z "$dirty" ] || err "uncommitted changes present outside CMakeLists.txt:
$dirty
commit or stash them first."

[ "$current" != "$NEW_VERSION" ] || err "version already $NEW_VERSION — nothing to bump"

# ---- 2. rewrite CMakeLists.txt ----------------------------------------------
hr "2/6  bump CMakeLists.txt"
sed -i -E "s/(project\(agentty VERSION )[0-9.]+/\1$NEW_VERSION/" CMakeLists.txt
grep -E "^project\(agentty VERSION $NEW_VERSION" CMakeLists.txt >/dev/null \
    || err "sed rewrite failed"
ok "project(agentty VERSION $NEW_VERSION ...)"

# ---- 3. compile sanity-check ------------------------------------------------
hr "3/6  build (sanity)"
if [ -d build ]; then
    cmake --build build -j10 >/dev/null
    ok "rebuild green"
    actual=$("$root/build/agentty" --version | awk '{print $2}')
    [ "$actual" = "$NEW_VERSION" ] || err "binary reports $actual, expected $NEW_VERSION"
    ok "binary --version reports $actual"
else
    info "no build/ directory — skipping local rebuild sanity check"
fi

# ---- 4. commit + tag --------------------------------------------------------
hr "4/6  commit + tag"
git add CMakeLists.txt
git commit -m "release: v$NEW_VERSION" >/dev/null
ok "committed"
git tag "v$NEW_VERSION"
ok "tagged v$NEW_VERSION"

# ---- 5. release.sh ----------------------------------------------------------
hr "5/6  build + upload artifacts"
if [ "$DRY" -eq 1 ]; then
    info "--dry: skipping release.sh upload, building only"
    "$root/scripts/release.sh"
else
    "$root/scripts/release.sh" --tag "v$NEW_VERSION"
fi

# ---- 6. push ----------------------------------------------------------------
hr "6/6  push"
if [ "$DRY" -eq 1 ]; then
    info "--dry: skipping git push"
else
    git push origin master --tags
    ok "pushed master + tags"
fi

hr "done — v$NEW_VERSION"
info "https://github.com/1ay1/agentty/releases/tag/v$NEW_VERSION"
