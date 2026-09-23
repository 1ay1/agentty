#!/bin/sh
# proof.sh — compile 11_zero_overhead.cpp at -O2 and compare the assembly
# of each weak/strong pair. See section 3 of the chapter.
#
# Usage: make proof   (or ./proof.sh)
#
# Two levels of "the same":
#   identical  - byte for byte the same instruction sequence
#   equivalent - the same MULTISET of instructions, scheduled differently
#
# Both mean the abstraction is free. Only a difference in the instructions
# THEMSELVES would mean the wrapper cost you something.

set -e
CXX="${CXX:-g++}"
SRC=11_zero_overhead.cpp
ASM=$(mktemp /tmp/ch01_proof.XXXXXX.s)
trap 'rm -f "$ASM" /tmp/ch01_w.$$ /tmp/ch01_s.$$' EXIT

"$CXX" -std=c++23 -O2 -S "$SRC" -o "$ASM"

# Pull one function's body out of the .s by mangled-name prefix.
# Strips directives and normalises local label numbers, which differ
# between two copies of the same code for no interesting reason.
body() {
    sym=$(grep -o "^_Z[0-9]*$1[A-Za-z0-9_]*:" "$ASM" | head -1)
    [ -n "$sym" ] || { echo "MISSING SYMBOL: $1" >&2; return 1; }
    awk -v s="$sym" '$0==s{f=1;next} f&&/\tret/{print;exit} f' "$ASM" \
        | grep -v '^[[:space:]]*\.' | sed 's/\.L[0-9][0-9]*/.L/g'
}

echo "comparing -O2 output, std::string vs Id<ThreadIdTag>"
echo

status=0
for pair in len empty total; do
    w=/tmp/ch01_w.$$ ; s=/tmp/ch01_s.$$
    body "weak_$pair"   > "$w"
    body "strong_$pair" > "$s"
    n=$(wc -l < "$w" | tr -d ' ')

    if cmp -s "$w" "$s"; then
        printf '  %-6s  identical    %2s instructions, same order\n' "$pair" "$n"
    elif [ "$(sort "$w" | md5sum)" = "$(sort "$s" | md5sum)" ]; then
        printf '  %-6s  equivalent   %2s instructions, scheduled differently:\n' "$pair" "$n"
        diff "$w" "$s" | sed 's/^/             /'
        printf '             (same instruction multiset. the scheduler just\n'
        printf '              hoisted an init. zero cost either way.)\n'
    else
        printf '  %-6s  DIFFERS      real difference:\n' "$pair"
        diff "$w" "$s" | sed 's/^/             /' || true
        status=1
    fi
done

echo
if [ "$status" -eq 0 ]; then
    echo "no pair costs an extra instruction. the strong type is free."
else
    echo "a pair genuinely differs. read the diff - that is the interesting part."
fi
exit "$status"
