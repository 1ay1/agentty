#!/bin/sh
# check-submodules-pushed.sh — every pinned submodule commit must be fetchable.
#
# The failure this prevents: you commit in a submodule, bump the pointer in
# the superproject, and push ONLY the superproject. Your machine is happy —
# the commit is right there in .git/modules — so everything builds and every
# test passes locally. But no one else can fetch it, and CI dies at
# "Init submodules" before it compiles a single file:
#
#   fatal: remote error: upload-pack: not our ref 900f35fe…
#   fatal: Fetched in submodule path 'mcp-cpp', but it did not contain …
#
# Every job fails identically, which reads like infrastructure trouble rather
# than a missing push. Worse, `git status` in the superproject is CLEAN, so
# there is nothing to notice. This makes it noticeable in one second.
#
# Usage:
#   scripts/check-submodules-pushed.sh          # check, exit 1 on problems
#   scripts/check-submodules-pushed.sh --fix    # push the missing ones
#
# Install as a pre-push hook (recommended):
#   ln -sf ../../scripts/check-submodules-pushed.sh .git/hooks/pre-push

set -eu

cd "$(git rev-parse --show-toplevel)"

fix=0
[ "${1:-}" = "--fix" ] && fix=1

bad=0
checked=0

# `git submodule status` prints "<sha> <path> (<describe>)"; the sha may carry
# a leading marker (-, +, U) which we strip.
git submodule status | while read -r sha path _rest; do
    sha=${sha#[-+U]}
    [ -d "$path" ] || continue
    checked=$((checked + 1))

    # Is the pinned commit reachable from any REMOTE branch? Reachability is
    # the real question, not "does the object exist" — a commit can be present
    # locally and still be unfetchable by anyone else.
    if (cd "$path" && git branch -r --contains "$sha" >/dev/null 2>&1) \
       && [ -n "$(cd "$path" && git branch -r --contains "$sha" 2>/dev/null)" ]; then
        printf '  ok   %-10s %s\n' "$path" "$(echo "$sha" | cut -c1-8)"
        continue
    fi

    printf '  MISS %-10s %s — not on any remote branch\n' \
           "$path" "$(echo "$sha" | cut -c1-8)"
    bad=$((bad + 1))

    if [ "$fix" = "1" ]; then
        branch=$(cd "$path" && git branch --show-current)
        if [ -z "$branch" ]; then
            printf '       cannot auto-push: detached HEAD\n'
            continue
        fi
        printf '       pushing %s %s…\n' "$path" "$branch"
        (cd "$path" && git push origin "$branch")
    fi
done

# The while loop runs in a subshell, so recount here for the exit status.
missing=$(git submodule status | while read -r sha path _rest; do
    sha=${sha#[-+U]}
    [ -d "$path" ] || continue
    if [ -z "$(cd "$path" && git branch -r --contains "$sha" 2>/dev/null)" ]; then
        echo x
    fi
done | wc -l | tr -d ' ')

if [ "$missing" != "0" ]; then
    echo
    echo "$missing submodule commit(s) are not on a remote."
    echo "CI will fail at 'Init submodules' before it builds anything."
    echo "Fix: scripts/check-submodules-pushed.sh --fix"
    exit 1
fi

echo "all submodule pins are fetchable"
