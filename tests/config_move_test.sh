#!/bin/sh
# config_move_test.sh — `agentty config move` end to end, against the real
# binary in a throwaway AGENTTY_HOME.
#   usage: config_move_test.sh <agentty>
set -u
A="$1"
S=$(mktemp -d "${TMPDIR:-/tmp}/agentty_cfgmove_XXXXXX")
trap 'rm -rf "$S"' EXIT
export AGENTTY_HOME="$S/home"
unset AGENTTY_THREADS_DIR AGENTTY_LOGS_DIR AGENTTY_CACHE_DIR AGENTTY_STATE_DIR AGENTTY_CREDENTIALS_DIR
fails=0
check() { if eval "$2"; then echo "  ok   $1"; else echo "  FAIL $1"; fails=$((fails + 1)); fi; }

mkdir -p "$AGENTTY_HOME/threads/blobs"
for i in 1 2 3; do printf '{"n":%s}\n' "$i" > "$AGENTTY_HOME/threads/aaaa00000000000$i.jsonl"; done
printf 'blob' > "$AGENTTY_HOME/threads/blobs/b1"
printf '{"model_id":"m"}' > "$AGENTTY_HOME/settings.json"

echo "move to a new folder"
"$A" config move threads "$S/big/threads" >/dev/null; rc=$?
check "exit 0"                    '[ $rc -eq 0 ]'
check "data arrived"              '[ -f "$S/big/threads/aaaa000000000002.jsonl" ] && [ -f "$S/big/threads/blobs/b1" ]'
check "old copy gone"             '[ ! -e "$AGENTTY_HOME/threads" ]'
check "settings records it"       'grep -q "\"threads\": \"$S/big/threads\"" "$AGENTTY_HOME/settings.json"'
check "other settings kept"       'grep -q "\"model_id\"" "$AGENTTY_HOME/settings.json"'
check "agentty now resolves it"   '"$A" config threads | grep -q "$S/big/threads"'
check "still 0700"                '[ "$(stat -c %a "$S/big/threads")" = 700 ]'

echo "refusals"
mkdir -p "$S/full" && touch "$S/full/x"
"$A" config move threads "$S/full" >/dev/null 2>&1; rc=$?
check "non-empty target refused"  '[ $rc -ne 0 ] && [ -f "$S/big/threads/aaaa000000000001.jsonl" ]'
"$A" config move threads "$S/big/threads/inner" >/dev/null 2>&1; rc=$?
check "target inside source"      '[ $rc -ne 0 ]'
AGENTTY_THREADS_DIR="$S/elsewhere" "$A" config move threads "$S/t3" >/dev/null 2>&1; rc=$?
check "user env var wins, refused" '[ $rc -ne 0 ]'
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
    "$A" config move threads default >/dev/null 2>&1; rc=$?
    check "refused while agentty runs" '[ $rc -ne 0 ] && [ -f "$S/big/threads/aaaa000000000003.jsonl" ]'
    wait
fi

echo "move back"
"$A" config move threads default >/dev/null; rc=$?
check "exit 0"                    '[ $rc -eq 0 ]'
check "data home again"           '[ -f "$AGENTTY_HOME/threads/aaaa000000000003.jsonl" ] && [ -f "$AGENTTY_HOME/threads/blobs/b1" ]'
check "settings key removed"      '! grep -q "\"dirs\"" "$AGENTTY_HOME/settings.json"'

echo "doctor"
chmod 755 "$AGENTTY_HOME/threads"
"$A" config doctor >/dev/null; rc=$?
check "doctor flags open threads"  '[ $rc -eq 1 ]'
chmod 700 "$AGENTTY_HOME/threads"
mkdir -p "$AGENTTY_HOME/credentials" && chmod 700 "$AGENTTY_HOME/credentials"
"$A" config doctor >/dev/null; rc=$?
check "doctor healthy after fix"   '[ $rc -eq 0 ]'

[ "$fails" -eq 0 ] && echo "all passed" || echo "$fails failed"
exit "$fails"
