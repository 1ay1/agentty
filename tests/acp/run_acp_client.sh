#!/bin/sh
# run_acp_client.sh — agentty acp against a fake OpenAI server, checked
# message by message against the ACP v1 schema (tests/acp/schema/v1).
#   usage: run_acp_client.sh <agentty> [schema.json]
# Schema checks need python's jsonschema; without it the scenarios still run.
set -u
A="$1"
HERE=$(cd "$(dirname "$0")" && pwd)
SCHEMA="${2:-$HERE/schema/v1/schema.json}"
S=$(mktemp -d "${TMPDIR:-/tmp}/agentty_acp_XXXXXX")
export FAKE_TOOLS_LOG="$S/tools.log"
python3 "$HERE/fake_openai_server.py" 0 > "$S/port" 2>/dev/null </dev/null &
SRV=$!
trap 'kill $SRV 2>/dev/null; rm -rf "$S"' EXIT INT TERM
# Up to 30s (see run_cli_test.sh): an empty port reads as "no credentials".
for _ in $(seq 1 150); do [ -s "$S/port" ] && break; sleep 0.2; done
if [ ! -s "$S/port" ]; then echo "FAIL fake server did not start"; exit 1; fi
PORT=$(cat "$S/port")

export AGENTTY_HOME="$S/home"
export AGENTTY_NO_PREWARM=1
mkdir -p "$AGENTTY_HOME"
printf '{"provider":"127.0.0.1:%s","model_id":"o4-mini"}\n' "$PORT" > "$AGENTTY_HOME/settings.json"
unset ANTHROPIC_API_KEY CLAUDE_CODE_OAUTH_TOKEN OPENAI_API_KEY AGENTTY_THREADS_DIR

PY=python3
[ -x "$HERE/../../.venv-acp/bin/python" ] && PY="$HERE/../../.venv-acp/bin/python"
"$PY" "$HERE/acp_client.py" "$A" "$SCHEMA" </dev/null
