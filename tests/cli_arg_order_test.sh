#!/bin/sh
# cli_arg_order_test.sh — `agentty run` accepts its arguments in any order.
#
# The parser used to scan only the LEADING run of arguments after `run` and
# stop at the first option it didn't own, so the prompt was claimed only when
# nothing global preceded it:
#
#     agentty run --agent coder "prompt"        worked
#     agentty run -w dir "prompt"               "unknown arg: prompt"
#     agentty run --agent coder -w dir "prompt" "unknown arg: prompt"
#
# Reported as "agent flag positioning only accepts one version". The rule was
# real but invisible, because the error named the PROMPT rather than the
# ordering — so it read as a rejected prompt, not a rejected position.
#
# This is a shell test because parse_args is internal to main.cpp; the thing
# worth pinning is the BINARY's contract, which is what a user actually hits.
#
#   sh tests/cli_arg_order_test.sh [path/to/agentty]

set -u
BIN="${1:-./build/agentty}"
[ -x "$BIN" ] || { echo "SKIP: no binary at $BIN"; exit 0; }

fails=0

# Accepts: parsing must not report an unknown argument. --help keeps this
# fast and side-effect free — it exercises the same parse_args and exits.
ok() {
    desc="$1"; shift
    out=$("$BIN" "$@" --help 2>&1 | head -1)
    case "$out" in
        unknown\ arg*) printf 'FAIL %-44s -> %s\n' "$desc" "$out"; fails=$((fails+1)) ;;
        *)             printf 'ok   %-44s\n' "$desc" ;;
    esac
}

# Rejects: a genuinely bad argument must still fail. Without this the test
# would pass just as well against a parser that accepts everything.
rejects() {
    desc="$1"; shift
    out=$("$BIN" "$@" 2>&1 | head -1)
    case "$out" in
        unknown\ arg*) printf 'ok   %-44s (rejected)\n' "$desc" ;;
        *)             printf 'FAIL %-44s -> accepted, want reject\n' "$desc"; fails=$((fails+1)) ;;
    esac
}

# Says: the output CONTAINS this message. Used for the empty-prompt case,
# where the point is WHICH error you get, not merely that parsing
# succeeded — `run ""` used to report a blank `unknown arg:`. Matched
# against the whole output because a banner (e.g. the sandbox line) may
# precede it.
says() {
    desc="$1"; want="$2"; shift 2
    out=$("$BIN" "$@" 2>&1)
    case "$out" in
        *"$want"*) printf 'ok   %-44s\n' "$desc" ;;
        *)         printf 'FAIL %-44s -> %s\n' "$desc" "$(printf '%s' "$out" | tr '\n' ' ')"
                   fails=$((fails+1)) ;;
    esac
}

echo "argument order:"
ok 'run PROMPT'                       run "p"
ok 'run --agent R PROMPT'             run --agent coder "p"
ok 'run PROMPT --agent R'             run "p" --agent coder
ok 'run PROMPT -w DIR'                run "p" -w /tmp
ok 'run -w DIR PROMPT'                run -w /tmp "p"
ok 'run --agent R -w DIR PROMPT'      run --agent coder -w /tmp "p"
ok 'run -w DIR --agent R PROMPT'      run -w /tmp --agent coder "p"
ok 'run -m MODEL --agent R PROMPT'    run -m some-model --agent tester "p"
ok 'run - (stdin)'                    run -

echo
echo "empty prompt:"
# An empty positional is still a prompt the scan CLAIMED: it must produce
# the real "no prompt" error, never `unknown arg:` with a blank name.
says 'run ""'        'no prompt'      run ""
says 'run "" -w DIR' 'no prompt'      run "" -w /tmp

echo
echo "still strict:"
rejects 'run --bogus'                 run --bogus "p"
rejects 'top-level --bogus'           --totally-bogus

echo
if [ "$fails" -ne 0 ]; then
    echo "$fails failure(s)"
    exit 1
fi
echo "all argument orders accepted, bad args still rejected"
