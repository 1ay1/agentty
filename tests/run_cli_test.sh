#!/bin/sh
# run_cli_test.sh: `agentty run` against the fake OpenAI server.
# Checks what a script relies on: clean stdout, exit codes, --json, -o,
# --max-turns, --timeout, and that Ctrl-C / SIGTERM stop the run and its
# tool processes.
#   usage: run_cli_test.sh <agentty>
set -u
A="$1"
HERE=$(cd "$(dirname "$0")" && pwd)
S=$(mktemp -d "${TMPDIR:-/tmp}/agentty_run_XXXXXX")
python3 "$HERE/acp/fake_openai_server.py" 0 > "$S/port" 2>/dev/null </dev/null &
SRV=$!
trap 'kill $SRV 2>/dev/null; rm -rf "$S"' EXIT INT TERM
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do [ -s "$S/port" ] && break; sleep 0.2; done

export AGENTTY_HOME="$S/home" AGENTTY_NO_PREWARM=1
mkdir -p "$AGENTTY_HOME" "$S/ws/.git"
printf '{"provider":"127.0.0.1:%s","model_id":"o4-mini"}\n' "$(cat "$S/port")" > "$AGENTTY_HOME/settings.json"
unset ANTHROPIC_API_KEY CLAUDE_CODE_OAUTH_TOKEN OPENAI_API_KEY AGENTTY_THREADS_DIR AGENTTY_EVENTS
cd "$S/ws" || exit 1

fails=0
pass() { printf 'ok   %s\n' "$1"; }
fail() { printf 'FAIL %s\n' "$1"; fails=$((fails+1)); }
check() { if [ "$2" = "$3" ]; then pass "$1"; else fail "$1 (got '$2', want '$3')"; fi; }

# plain answer: stdout is exactly the answer, exit 0
out=$("$A" run "say hi" 2>"$S/err"); rc=$?
check "plain: exit 0" "$rc" 0
check "plain: stdout is only the answer" "$out" "hello from fake"

# piped input with a prompt
out=$(echo "some data" | "$A" run "summarize" 2>/dev/null); rc=$?
check "pipe: exit 0" "$rc" 0

# a tool call then an answer, with --json
out=$("$A" run --json "please write a file" 2>/dev/null); rc=$?
check "json: exit 0" "$rc" 0
j() { printf '%s' "$out" | python3 -c "import json,sys; d=json.load(sys.stdin); print($1)"; }
check "json: status" "$(j 'd["status"]')" ok
check "json: result" "$(j 'd["result"]')" done
check "json: turns" "$(j 'd["turns"]')" 2
check "json: tool_calls" "$(j 'd["tool_calls"]')" 1
check "json: tokens summed per request" "$(j 'd["usage"]["input_tokens"], d["usage"]["output_tokens"]')" "20 10"
check "json: model" "$(j 'bool(d["model"])')" True

# -o writes the answer to a file, nothing on stdout
out=$("$A" run -o "$S/answer.txt" "say hi" 2>/dev/null); rc=$?
check "-o: exit 0" "$rc" 0
check "-o: stdout empty" "$out" ""
check "-o: file has the answer" "$(cat "$S/answer.txt" 2>/dev/null)" "hello from fake"

# provider error: exit 3, message on stderr, nothing on stdout
out=$("$A" run "please fail" 2>"$S/err"); rc=$?
check "provider error: exit 3" "$rc" 3
check "provider error: stdout empty" "$out" ""
grep -q "provider_error: .*upstream exploded" "$S/err" && pass "provider error: says why" \
    || fail "provider error: says why ($(cat "$S/err"))"
grep -q "Activity:" "$S/err" && fail "provider error: no internal activity dump" \
    || pass "provider error: no internal activity dump"

# a small context window filling up says so (#85): exit 5, sizes on stderr
SMALL="$S/home_small"; mkdir -p "$SMALL"
printf '{"provider":"127.0.0.1:%s","model_id":"small-ctx"}\n' "$(cat "$S/port")" > "$SMALL/settings.json"
AGENTTY_HOME="$SMALL" "$A" run "CTXLEN go" >/dev/null 2>"$S/err"; rc=$?
check "context cut: exit 5" "$rc" 5
grep -q "context_full: context window full (25000 tokens, last prompt 24500)" "$S/err" \
    && pass "context cut: says window and prompt size" || fail "context cut: says why ($(cat "$S/err"))"
AGENTTY_HOME="$SMALL" "$A" run "CTX400 go" >/dev/null 2>"$S/err"; rc=$?
check "context 400: exit 5" "$rc" 5
grep -q "context_full: .*exceeds the available context size" "$S/err" \
    && pass "context 400: says why" || fail "context 400: says why ($(cat "$S/err"))"
out=$(AGENTTY_HOME="$SMALL" "$A" run --json "CTXLEN go" 2>/dev/null)
check "context: json window" "$(j 'd["usage"]["context_window"]')" 25000

# --max-turns stops a run that never finishes: exit 1, status turn_limit
out=$("$A" run --json --max-turns 3 "LOOP forever" 2>/dev/null); rc=$?
check "max-turns: exit 1" "$rc" 1
check "max-turns: status" "$(j 'd["status"]')" turn_limit
check "max-turns: turns" "$(j 'd["turns"]')" 3

# --timeout stops a long tool: exit 124, quickly
t0=$(date +%s)
"$A" run --timeout 2 'CALL shell {"command":"sleep 30"}' >/dev/null 2>&1; rc=$?
el=$(( $(date +%s) - t0 ))
check "timeout: exit 124" "$rc" 124
[ "$el" -lt 15 ] && pass "timeout: stopped in ${el}s" || fail "timeout: took ${el}s"

# bad flag values are usage errors (exit 2) before any work
"$A" run --max-turns 0 "x" >/dev/null 2>&1; check "max-turns 0: exit 2" "$?" 2
"$A" run --timeout 5x "x" >/dev/null 2>&1;  check "timeout 5x: exit 2" "$?" 2
"$A" run --effort huge "x" >/dev/null 2>&1; check "effort huge: exit 2" "$?" 2
"$A" run --resume ../etc "x" >/dev/null 2>&1; check "resume ../etc: exit 2" "$?" 2

# --events jsonl streams reasoning and answer text, stdout stays the answer (#81)
out=$("$A" run --events jsonl "please think" 2>"$S/ev"); rc=$?
check "events: exit 0" "$rc" 0
check "events: stdout is only the answer" "$out" "thought done"
ev() { python3 - "$S/ev" "$1" <<'EOF'
import json, sys
kinds = {}
for line in open(sys.argv[1]):
    line = line.strip()
    if not line.startswith("{"): continue
    d = json.loads(line)
    kinds.setdefault(d["ev"], []).append(d.get("text", ""))
print("".join(kinds.get(sys.argv[2], [])))
EOF
}
check "events: reasoning line" "$(ev reasoning)" "let me think"
check "events: text line" "$(ev text)" "thought done"

# --save then --resume: the second run sees the first conversation
out=$("$A" run --save --json "first question" 2>/dev/null)
tid=$(j 'd.get("thread_id", "")')
[ -n "$tid" ] && pass "save: thread id in json" || fail "save: thread id in json"
out=$("$A" run --json --resume "$tid" "second question" 2>/dev/null); rc=$?
check "resume: exit 0" "$rc" 0
check "resume: same thread" "$(j 'd.get("thread_id")')" "$tid"
out=$("$A" run --json --resume last "third" 2>/dev/null)
check "resume last: same thread" "$(j 'd.get("thread_id")')" "$tid"
"$A" run --resume 0123456789abcdef "x" >/dev/null 2>&1; check "resume missing: exit 4" "$?" 4

# --read-only: a write the model asks for is refused, nothing written
rm -f out.txt
out=$("$A" run --json --read-only "please write a file" 2>/dev/null)
check "read-only: write refused" "$(j 'd["tool_errors"]')" 1
[ ! -e out.txt ] && pass "read-only: no file written" || fail "read-only: no file written"

# --no-network: a shell command can't reach the network. Needs claybin; on a
# host without it the flag must refuse instead (exit 2).
"$A" run --no-network 'CALL shell {"command":"curl -sS -m 4 -o /dev/null https://example.com 2>/dev/null; echo $? > net_rc.txt"}' >/dev/null 2>&1; rc=$?
if [ "$rc" -eq 2 ]; then
    pass "no-network: refused without claybin"
else
    check "no-network: exit 0" "$rc" 0
    nrc=$(cat net_rc.txt 2>/dev/null)
    [ -n "$nrc" ] && [ "$nrc" != 0 ] && pass "no-network: curl blocked (rc $nrc)" \
        || fail "no-network: curl blocked (rc '$nrc')"
fi
"$A" run --sandbox off --no-network "x" >/dev/null 2>&1; check "no-network + sandbox off: exit 2" "$?" 2

# A fork reopened in a new process can still read its parent's transcript,
# which lives outside the workspace (#83). Other files there stay closed.
TD="$AGENTTY_HOME/threads"; mkdir -p "$TD"
printf '# Transcript: parent\nPARENT_MARKER\n' > "$TD/bbbbbbbbbbbbbbbb.transcript.md"
printf 'secret\n' > "$TD/cccccccccccccccc.transcript.md"
python3 - "$TD" <<'EOF'
import json, sys, time
td = sys.argv[1]
now = int(time.time())
note = {"id": "1111111111111111", "role": "user", "timestamp": now, "tool_calls": [],
        "text": "This conversation is a fork. Transcript: " + td + "/bbbbbbbbbbbbbbbb.transcript.md",
        "fork_note": True, "fork_transcript": td + "/bbbbbbbbbbbbbbbb.transcript.md"}
json.dump({"id": "aaaaaaaaaaaaaaaa", "title": "Fork: parent", "forked_from": "bbbbbbbbbbbbbbbb",
           "created_at": now, "updated_at": now, "messages": [note]},
          open(td + "/aaaaaaaaaaaaaaaa.json", "w"))
EOF
"$A" run --resume aaaaaaaaaaaaaaaa --events jsonl \
    "CALL read {\"path\":\"$TD/bbbbbbbbbbbbbbbb.transcript.md\"}" >/dev/null 2>"$S/fk"
grep '"tool":"read"' "$S/fk" | grep -q '"ok":true' && pass "fork: parent transcript readable after reopen" \
    || fail "fork: parent transcript readable after reopen ($(grep '"tool"' "$S/fk"))"
"$A" run --resume aaaaaaaaaaaaaaaa --events jsonl \
    "CALL read {\"path\":\"$TD/cccccccccccccccc.transcript.md\"}" >/dev/null 2>"$S/fk"
grep '"tool":"read"' "$S/fk" | grep -q '"ok":false' && pass "fork: other transcripts stay closed" \
    || fail "fork: other transcripts stay closed ($(grep '"tool"' "$S/fk"))"

# a signal mid-tool stops the run and kills the tool. python runs it so
# SIGINT isn't pre-ignored the way sh does for background jobs.
sig_case() {
    python3 - "$A" "$1" "$2" <<'EOF'
import json, os, signal, subprocess, sys, time
a, name, want = sys.argv[1], sys.argv[2], int(sys.argv[3])
marker = "agentty_run_sig_%d" % os.getpid()
# A symlink named after the marker, not `exec -a`: that is a bash extension,
# and on Ubuntu /bin/sh is dash, where the tool failed instantly and the run
# exited 0 before the signal was ever sent. argv[0] = ./marker puts the
# marker in the cmdline for pgrep -f on every POSIX sh.
cmd = 'ln -sf "$(command -v sleep)" ./%s && exec ./%s 40' % (marker, marker)
p = subprocess.Popen([a, "run", 'CALL shell ' + json.dumps({"command": cmd})],
                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, stdin=subprocess.DEVNULL)
time.sleep(2.0)
t0 = time.time()
p.send_signal(getattr(signal, name))
try:
    p.wait(timeout=15)
except subprocess.TimeoutExpired:
    p.kill(); print("FAIL %s: did not exit" % name); sys.exit(1)
el = time.time() - t0
time.sleep(0.5)
left = subprocess.run(["pgrep", "-f", marker], capture_output=True, text=True).stdout.strip()
if left:
    subprocess.run(["pkill", "-9", "-f", marker])
ok = p.returncode == want and el < 10 and not left
print("%s %s: exit %d (want %d), %.1fs, orphans: %s"
      % ("ok  " if ok else "FAIL", name, p.returncode, want, el, left or "none"))
sys.exit(0 if ok else 1)
EOF
}
sig_case SIGINT 130  || fails=$((fails+1))
sig_case SIGTERM 143 || fails=$((fails+1))

echo
if [ "$fails" -ne 0 ]; then echo "$fails failure(s)"; exit 1; fi
echo "agentty run: all checks passed"
