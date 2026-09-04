#!/usr/bin/env bash
# Headless self-deploy driver for agentty.org — UNIFIED REPO edition.
#
# The site is now part of the agentty repo itself (1ay1/agentty), living under
# site/. There is ONE source of truth and ONE push target: 1ay1/agentty. A push
# to it (docs, content, or the site runtime) is the only trigger; nothing is
# ever pushed to a separate site repo.
#
# What it does:
#   1. Fast-forward the agentty repo to origin/master.
#   2. cd site/ and run deploy.sh, which syncs docs+content FROM THE SAME
#      CHECKOUT (no fetch), refetches version/sizes/stars, builds, rsyncs to
#      /var/www, reloads nginx.
set -uo pipefail

REPO="/home/ayush/projects/agentty"
SITE="$REPO/site"
LOCK="/tmp/agentty-autodeploy.lock"
BRANCH="master"

if mkdir -p /var/log/agentty-deploy 2>/dev/null && [ -w /var/log/agentty-deploy ]; then
  LOG="/var/log/agentty-deploy/autodeploy.log"
else
  mkdir -p "$HOME/.agentty-deploy" 2>/dev/null || true
  LOG="$HOME/.agentty-deploy/autodeploy.log"
fi

log() { echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] $*" | tee -a "$LOG"; }

# Coalescing lock: burst of pushes → exactly one extra deploy.
exec 9>"$LOCK"
if ! flock -n 9; then
  log "deploy in progress — requesting a follow-up run"
  touch "${LOCK}.rerun"
  exit 0
fi

deploy_once() {
  cd "$REPO" || { log "FATAL: cannot cd $REPO"; return 1; }

  log "fetching origin/$BRANCH (1ay1/agentty)"
  git fetch --quiet origin "$BRANCH" || { log "git fetch failed"; return 1; }

  local before after
  before=$(git rev-parse HEAD)
  after=$(git rev-parse "origin/$BRANCH")

  # Reset hard to origin so a headless deploy never wedges on local drift.
  # NB: site/ build artifacts + synced content are gitignored, so this is safe.
  git reset --hard "origin/$BRANCH" --quiet || { log "git reset failed"; return 1; }
  # Bring submodules in line too (agentty has maya/mcp-cpp/etc.).
  git submodule update --init --recursive --quiet 2>/dev/null || true

  if [ "$before" = "$after" ] && [ "${FORCE:-0}" != "1" ]; then
    log "already at $after — site rebuild still runs to refresh live GitHub data"
  else
    log "agentty repo $before → $after"
  fi

  cd "$SITE" || { log "FATAL: cannot cd $SITE"; return 1; }

  # Ensure deps are present (first deploy / lockfile change). npm ci is fast when
  # node_modules is warm; falls back to install if the lockfile drifted.
  if [ ! -d node_modules ] || [ package-lock.json -nt node_modules ]; then
    log "installing site deps"
    npm ci >>"$LOG" 2>&1 || npm install >>"$LOG" 2>&1 || { log "npm install failed"; return 1; }
  fi

  log "running deploy.sh"
  if ./deploy.sh >>"$LOG" 2>&1; then
    log "deploy OK — https://agentty.org is live"
  else
    log "deploy.sh failed — retrying once"
    if ./deploy.sh >>"$LOG" 2>&1; then
      log "deploy OK on retry — https://agentty.org is live"
    else
      log "deploy.sh FAILED twice (see above)"
      return 1
    fi
  fi
}

rc=0
deploy_once || rc=1

if [ -f "${LOCK}.rerun" ]; then
  rm -f "${LOCK}.rerun"
  log "follow-up run requested during build — deploying once more"
  deploy_once || rc=1
fi

exit "$rc"
