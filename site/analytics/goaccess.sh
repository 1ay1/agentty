#!/usr/bin/env bash
# Generate / live-update the GoAccess HTML dashboard for agentty.org.
#
# Two modes:
#   ./goaccess.sh            -> one-shot static report (good for cron)
#   ./goaccess.sh --live     -> real-time daemonised report with WebSocket
#
# Output is written to /var/lib/goaccess/html/index.html and served at
# https://stats.agentty.org (see the stats vhost in agentty.org.nginx).
# Kept OUTSIDE the deploy webroot so `rsync --delete` never wipes it.
set -euo pipefail

CONF="$(dirname "$(readlink -f "$0")")/goaccess.conf"
LOG="/var/log/nginx/agentty.access.log"
OUT_DIR="/var/lib/goaccess/html"
OUT="$OUT_DIR/index.html"

sudo mkdir -p "$OUT_DIR" /var/lib/goaccess

# Include rotated logs too, so the report covers full history.
LOGS=("$LOG")
for f in "$LOG".*; do [ -e "$f" ] && LOGS+=("$f"); done

zcat_or_cat() { case "$1" in *.gz) sudo zcat "$1";; *) sudo cat "$1";; esac; }

if [[ "${1:-}" == "--live" ]]; then
  echo "==> Starting real-time GoAccess (foreground; for systemd use)"
  exec sudo goaccess "$LOG" \
    --config-file="$CONF" \
    --output="$OUT" \
    --real-time-html \
    --ws-url=wss://stats.agentty.org/ws \
    --addr=127.0.0.1 --port=7890
else
  echo "==> Building static report from ${#LOGS[@]} log file(s)"
  for f in "${LOGS[@]}"; do zcat_or_cat "$f"; done | \
    sudo goaccess - --config-file="$CONF" \
      --output="$OUT"
  echo "==> Wrote $OUT"
fi
