#!/bin/sh
# config_move_test.sh — `agentty config move` and `config doctor` end to end,
# against the real binary in a throwaway AGENTTY_HOME. Storage is configured
# by environment only, so move moves the data and prints the export line.
#   usage: config_move_test.sh <agentty>
set -u
A="$1"
S=$(mktemp -d "${TMPDIR:-/tmp}/agentty_cfgmove_XXXXXX")
trap 'rm -rf "$S"' EXIT
export AGENTTY_HOME="$S/home"
unset AGENTTY_THREADS_DIR AGENTTY_LOGS_DIR AGENTTY_CACHE_DIR AGENTTY_STATE_DIR \
      AGENTTY_CREDENTIALS_DIR AGENTTY_THREADS_KEEP_DAYS
fails=0
check() { if eval "$2"; then echo "  ok   $1"; else echo "  FAIL $1"; fails=$((fails + 1)); fi; }

mkdir -p "$AGENTTY_HOME/threads/blobs"
for i in 1 2 3; do printf '{"n":%s}\n' "$i" > "$AGENTTY_HOME/threads/aaaa00000000000$i.jsonl"; done
printf 'blob' > "$AGENTTY_HOME/threads/blobs/b1"
printf '{"model_id":"m"}' > "$AGENTTY_HOME/settings.json"
cp "$AGENTTY_HOME/settings.json" "$S/settings.before"

echo "move to a new folder"
out=$("$A" config move threads "$S/big/threads"); rc=$?
check "exit 0"                    '[ $rc -eq 0 ]'
check "data arrived"              '[ -f "$S/big/threads/aaaa000000000002.jsonl" ] && [ -f "$S/big/threads/blobs/b1" ]'
check "old copy gone"             '[ ! -e "$AGENTTY_HOME/threads" ]'
check "prints the export line"    'echo "$out" | grep -q "export AGENTTY_THREADS_DIR=.$S/big/threads."'
check "settings.json untouched"   'cmp -s "$AGENTTY_HOME/settings.json" "$S/settings.before"'
check "still 0700"                '[ "$(stat -c %a "$S/big/threads")" = 700 ]'
check "env var points agentty at it" 'AGENTTY_THREADS_DIR="$S/big/threads" "$A" config threads | grep -q "$S/big/threads"'

echo "refusals"
mkdir -p "$AGENTTY_HOME/threads" && echo x > "$AGENTTY_HOME/threads/new.jsonl"
mkdir -p "$S/full" && touch "$S/full/x"
"$A" config move threads "$S/full" >/dev/null 2>&1; rc=$?
check "non-empty target refused"  '[ $rc -ne 0 ] && [ -f "$AGENTTY_HOME/threads/new.jsonl" ]'
"$A" config move threads "$AGENTTY_HOME/threads/inner" >/dev/null 2>&1; rc=$?
check "target inside source"      '[ $rc -ne 0 ]'
"$A" config move bogus "$S/x" >/dev/null 2>&1; rc=$?
check "unknown dir refused"       '[ $rc -eq 2 ]'

if command -v python3 >/dev/null 2>&1; then
    python3 - "$AGENTTY_HOME/agentty.running.lock" <<'EOF' &
import fcntl, os, sys, time
fd = os.open(sys.argv[1], os.O_RDWR | os.O_CREAT, 0o600)
fcntl.lockf(fd, fcntl.LOCK_SH)
time.sleep(2)
EOF
    sleep 0.5
    "$A" config move threads "$S/t4" >/dev/null 2>&1; rc=$?
    check "refused while agentty runs" '[ $rc -ne 0 ] && [ -f "$AGENTTY_HOME/threads/new.jsonl" ] && [ ! -e "$S/t4" ]'
    wait
fi

echo "move back"
rm -rf "$AGENTTY_HOME/threads"
out=$(AGENTTY_THREADS_DIR="$S/big/threads" "$A" config move threads default); rc=$?
check "exit 0"                    '[ $rc -eq 0 ]'
check "data home again"           '[ -f "$AGENTTY_HOME/threads/aaaa000000000003.jsonl" ] && [ -f "$AGENTTY_HOME/threads/blobs/b1" ]'
check "tells you to unset"        'echo "$out" | grep -q "unset AGENTTY_THREADS_DIR"'

echo "doctor"
chmod 755 "$AGENTTY_HOME/threads"
"$A" config doctor >/dev/null; rc=$?
check "flags open threads"        '[ $rc -eq 1 ]'
chmod 700 "$AGENTTY_HOME/threads"
mkdir -p "$AGENTTY_HOME/credentials" && chmod 700 "$AGENTTY_HOME/credentials"
"$A" config doctor >/dev/null; rc=$?
check "healthy after fix"         '[ $rc -eq 0 ]'
AGENTTY_THREADS_KEEP_DAYS=soon "$A" config doctor >/dev/null; rc=$?
check "flags bad KEEP_DAYS"       '[ $rc -eq 1 ]'

[ "$fails" -eq 0 ] && echo "all passed" || echo "$fails failed"
exit "$fails"
