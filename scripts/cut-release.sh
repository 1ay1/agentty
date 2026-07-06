#!/bin/sh
# scripts/cut-release.sh — the ONE manual step to ship a release.
#
# This does NOT build anything. It performs the small, human-owned bookkeeping
# that precedes a release, then pushes the tag that makes GitHub Actions
# (.github/workflows/release.yml) build + package + publish EVERYTHING:
# linux/macos/windows binaries, .deb/.rpm/.apk/arch/.msi packages, and
# submissions to winget, homebrew, scoop, AUR, plus attached nix/snap/gentoo
# manifests. All of that is automatic once the tag lands.
#
# So the entire release ritual is:
#
#     scripts/cut-release.sh X.Y.Z
#
# What it does, in order:
#   1. Verify a clean tree on the default branch, up to date with origin.
#   2. Rewrite the single source of truth: project(agentty VERSION X.Y.Z) in
#      CMakeLists.txt. Every manifest derives its version from this line, so
#      nothing else needs editing.
#   3. Promote CHANGELOG.md's [Unreleased] section to [X.Y.Z] (dated), and
#      open a fresh empty [Unreleased] on top.
#   4. Commit ("release: vX.Y.Z"), create an annotated tag vX.Y.Z, and push
#      both the branch and the tag to origin.
#   5. The tag push triggers the release workflow. Print its URL.
#
# Guards:
#   * X.Y.Z must be strictly greater than the current CMake version (no
#     accidental re-release / downgrade).
#   * Tag vX.Y.Z must not already exist (locally or on origin).
#   * Working tree must be clean (no surprise files swept into the release
#     commit).
#
# Flags:
#   --dry-run   Do steps 1-3 in memory and show the diff, but DON'T commit,
#               tag, or push. Nothing is written to disk. For a preview.
#   --no-push   Commit + tag locally but don't push (inspect, then push by
#               hand: `git push origin <branch> && git push origin vX.Y.Z`).
#   --allow-dirty  Skip the clean-tree guard (you know what you're doing).
#
# Requires: git, sed, awk. gh is NOT required (the tag push does the work).

set -eu

# ---- args --------------------------------------------------------------------
NEWVER=""
DRY_RUN=0
NO_PUSH=0
ALLOW_DIRTY=0
while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run)     DRY_RUN=1; shift ;;
        --no-push)     NO_PUSH=1; shift ;;
        --allow-dirty) ALLOW_DIRTY=1; shift ;;
        -h|--help)     sed -n '2,45p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*)            echo "cut-release.sh: unknown flag: $1" >&2; exit 2 ;;
        *)
            [ -z "$NEWVER" ] || { echo "cut-release.sh: version given twice" >&2; exit 2; }
            NEWVER=$1; shift ;;
    esac
done

# ---- ui ----------------------------------------------------------------------
info() { printf '\033[1;34m::\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!\033[0m  %s\n' "$*"; }
ok()   { printf '\033[1;32m\xe2\x9c\x93\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31mx\033[0m  %s\n' "$*" >&2; exit 1; }

[ -n "$NEWVER" ] || die "usage: cut-release.sh X.Y.Z  [--dry-run] [--no-push] [--allow-dirty]"

# ---- paths -------------------------------------------------------------------
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
cml="$root/CMakeLists.txt"
chg="$root/CHANGELOG.md"
[ -f "$cml" ] || die "CMakeLists.txt not found at $cml"
[ -f "$chg" ] || die "CHANGELOG.md not found at $chg"

# ---- validate version --------------------------------------------------------
case "$NEWVER" in
    *[!0-9.]*|.*|*.|*..*) die "version '$NEWVER' is not X.Y.Z (digits + dots only)" ;;
esac
# Exactly three dot-separated numeric components.
echo "$NEWVER" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' \
    || die "version '$NEWVER' must be exactly MAJOR.MINOR.PATCH"

CURVER=$(sed -nE 's/.*project\(agentty VERSION ([0-9.]+).*/\1/p' "$cml" | head -1)
[ -n "$CURVER" ] || die "could not read current VERSION from CMakeLists.txt"

# Strictly-greater check via sort -V (version sort). If the max of the two is
# the current version, or they're equal, refuse.
if [ "$CURVER" = "$NEWVER" ]; then
    die "version $NEWVER is already the current version — nothing to cut"
fi
greater=$(printf '%s\n%s\n' "$CURVER" "$NEWVER" | sort -V | tail -1)
[ "$greater" = "$NEWVER" ] || die "version $NEWVER is LOWER than current $CURVER — refusing to downgrade"

TAG="v$NEWVER"
info "current version : $CURVER"
info "new version     : $NEWVER  (tag $TAG)"

# ---- git preflight -----------------------------------------------------------
git rev-parse --git-dir >/dev/null 2>&1 || die "not inside a git repository"
BRANCH=$(git rev-parse --abbrev-ref HEAD)

if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null; then
    die "tag $TAG already exists locally"
fi
if git ls-remote --exit-code --tags origin "refs/tags/$TAG" >/dev/null 2>&1; then
    die "tag $TAG already exists on origin"
fi

if [ "$ALLOW_DIRTY" -eq 0 ]; then
    if [ -n "$(git status --porcelain)" ]; then
        die "working tree is dirty — commit/stash first, or pass --allow-dirty"
    fi
fi

# Warn (don't block) if the branch is behind origin — the push would fail
# later anyway, better to say so now.
if git rev-parse --abbrev-ref "@{upstream}" >/dev/null 2>&1; then
    git fetch -q origin "$BRANCH" || warn "could not fetch origin/$BRANCH"
    behind=$(git rev-list --count "HEAD..@{upstream}" 2>/dev/null || echo 0)
    [ "$behind" -eq 0 ] || warn "local $BRANCH is $behind commit(s) behind origin — push may be rejected"
fi

# ---- 1. bump CMakeLists ------------------------------------------------------
info "bumping CMakeLists.txt: $CURVER -> $NEWVER"
new_cml=$(sed -E "s/(project\(agentty VERSION )[0-9.]+/\1$NEWVER/" "$cml")
printf '%s\n' "$new_cml" | grep -q "project(agentty VERSION $NEWVER" \
    || die "failed to rewrite version line in CMakeLists.txt"

# ---- 2. promote CHANGELOG ----------------------------------------------------
# Turn the top-of-file:
#     ## [Unreleased]
#     <entries...>
#     ## [<prev>]
# into:
#     ## [Unreleased]
#
#     ## [X.Y.Z] - YYYY-MM-DD
#     <entries...>
#     ## [<prev>]
# The awk below finds the first "## [Unreleased]" and the NEXT "## [" heading,
# inserting the new version header just after a fresh empty [Unreleased].
today=$(date +%Y-%m-%d)
info "promoting CHANGELOG [Unreleased] -> [$NEWVER] ($today)"
new_chg=$(awk -v ver="$NEWVER" -v day="$today" '
    BEGIN { done=0 }
    {
        if (!done && $0 ~ /^## \[Unreleased\]/) {
            print "## [Unreleased]"
            print ""
            print "## [" ver "] - " day
            done=1
            next
        }
        print
    }
    END {
        if (!done) {
            # No [Unreleased] section found — should not happen, but fail loud
            # rather than silently produce a changelog with no new heading.
            exit 3
        }
    }
' "$chg") || die "CHANGELOG has no '## [Unreleased]' section to promote"

# ---- dry run: show what WOULD change, touch nothing --------------------------
if [ "$DRY_RUN" -eq 1 ]; then
    warn "--dry-run: no files written, no commit, no tag, no push"
    printf '\n\033[1mCMakeLists.txt version line:\033[0m\n'
    printf '  - %s\n' "project(agentty VERSION $CURVER ...)"
    printf '  + %s\n' "project(agentty VERSION $NEWVER ...)"
    printf '\n\033[1mCHANGELOG.md new heading:\033[0m\n'
    printf '  + ## [%s] - %s\n' "$NEWVER" "$today"
    printf '\n\033[1mWould then:\033[0m git commit -m "release: %s"  ->  git tag -a %s  ->  git push origin %s %s\n' \
        "$TAG" "$TAG" "$BRANCH" "$TAG"
    exit 0
fi

# ---- write files -------------------------------------------------------------
printf '%s\n' "$new_cml" > "$cml"
printf '%s\n' "$new_chg" > "$chg"
ok "CMakeLists.txt + CHANGELOG.md updated"

# ---- 3. commit + tag ---------------------------------------------------------
git add "$cml" "$chg"
git commit -q -m "release: $TAG"
ok "committed: release: $TAG"

git tag -a "$TAG" -m "agentty $TAG"
ok "tagged: $TAG"

# ---- 4. push -----------------------------------------------------------------
if [ "$NO_PUSH" -eq 1 ]; then
    warn "--no-push: commit + tag created locally but NOT pushed."
    info "to ship: git push origin $BRANCH && git push origin $TAG"
    exit 0
fi

info "pushing branch $BRANCH + tag $TAG to origin (this triggers the release workflow)"
git push -q origin "$BRANCH"
git push -q origin "$TAG"
ok "pushed $BRANCH and $TAG"

# ---- 5. point at the run -----------------------------------------------------
printf '\n'
ok "release $TAG cut. GitHub Actions is now building + publishing everything."
info "watch it:  https://github.com/1ay1/agentty/actions/workflows/release.yml"
if command -v gh >/dev/null 2>&1; then
    info "or:        gh run watch --repo 1ay1/agentty \$(gh run list --repo 1ay1/agentty --workflow release.yml -L1 --json databaseId -q '.[0].databaseId')"
fi
