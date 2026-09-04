#!/usr/bin/env bash
# Wrapper for the GoAccess real-time dashboard.
#
# GoAccess hard-codes its internal WS bind port into the generated HTML's
# `connection` object and ignores any port given in --ws-url. The browser must
# reach the feed over the PUBLIC TLS port (443) via nginx's /ws proxy, so after
# GoAccess writes the report we rewrite `"port": 7890` -> `"port": 443`.
#
# GoAccess only writes the HTML once at startup (updates afterwards stream over
# the WebSocket, the file is not rewritten), so a one-shot patch is sufficient;
# we still re-patch on any change for safety.
set -euo pipefail

LOG="/var/log/nginx/agentty.access.log"
CONF="/home/ayush/projects/agentpp-site/analytics/goaccess.conf"
OUT="/var/lib/goaccess/html/index.html"
BIND_PORT=7890
PUBLIC_PORT=443

patch_port() {
  [ -f "$OUT" ] || return 0
  if grep -q "\"port\": ${BIND_PORT}" "$OUT" 2>/dev/null; then
    sed -i "s/\"port\": ${BIND_PORT}/\"port\": ${PUBLIC_PORT}/g" "$OUT"
  fi
}

# Launch GoAccess in the background.
goaccess "$LOG" \
  --config-file="$CONF" \
  --output="$OUT" \
  --real-time-html \
  --ws-url=wss://stats.agentty.org/ws \
  --addr=127.0.0.1 --port="$BIND_PORT" &
GA_PID=$!

# Wait for the report to appear, then patch the port. Re-patch a few times in
# case GoAccess rewrites the file shortly after first render.
for _ in $(seq 1 10); do
  sleep 1
  patch_port
done

# Keep patching at a slow cadence as long as GoAccess runs; cheap and safe.
while kill -0 "$GA_PID" 2>/dev/null; do
  sleep 30
  patch_port
done

wait "$GA_PID"
