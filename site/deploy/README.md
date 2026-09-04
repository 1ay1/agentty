# Fully automated deploys — you only ever edit Markdown

The site redeploys itself. **You never run `./deploy.sh`, never touch the
server, never edit a version number.** Edit docs in `1ay1/agentty`
(`docs/website/*.md`), push, and the live site updates within seconds.

## Architecture

```
edit docs/website/*.md (agentty repo)  ──push──▶  GitHub
     or push site code (agentty.org)                 │
     or publish an agentty release                   │  webhook (HMAC-signed)
                                                      ▼
                             https://agentty.org/_deploy-hook
                                   │  nginx → 127.0.0.1:9099
                                   ▼
                         webhook.mjs  (systemd: agentty-deploy-hook.service)
                                   │  verify signature, debounce 3s
                                   ▼
                         autodeploy.sh  ──flock──▶  deploy.sh
                                   │                   ├─ sync-docs.mjs  (pull docs/website from agentty)
                                   │                   ├─ fetch-release / measure-stats / fetch-repo
                                   │                   ├─ next build → out/
                                   │                   └─ rsync → /var/www + reload nginx
                                   ▼
                         https://agentty.org  (live)

  + agentty-deploy.timer  ──every 30 min──▶  autodeploy.sh   (self-healing backstop)
```

**Instant path:** GitHub webhook → listener → deploy. Reacts to a push in ~3 s.

**Backstop:** a systemd timer runs `autodeploy.sh` every 30 min regardless, so
even if a webhook is ever missed the site reconciles to the latest docs + live
GitHub data (version, sizes, stars) on its own.

Everything the site shows is refetched from GitHub on every deploy — nothing is
hardcoded. A new agentty release publishes → webhook (`release: published`) →
redeploy → the version badge updates itself.

## Components (all in this `deploy/` dir)

| File | Role |
|------|------|
| `webhook.mjs` | Zero-dep HTTP listener on `127.0.0.1:9099`; HMAC-verifies GitHub, triggers deploy. Watches `1ay1/agentty` (docs/website only) + `1ay1/agentty.org`. |
| `autodeploy.sh` | Headless deploy driver: `git reset --hard origin/master` → `deploy.sh`. `flock` prevents overlap; coalesces bursts. |
| `agentty-deploy-hook.service` | systemd unit for the webhook listener (always on). |
| `agentty-deploy.service` + `.timer` | 30-min backstop that runs `autodeploy.sh`. |
| `agentty-deploy.sudoers` | Narrow passwordless-sudo grant for the exact rsync/nginx/chown steps `deploy.sh` needs. |

The older `agentty-site-deploy.*` units + `scripts/redeploy-on-release.sh`
(release-tag polling only) are **superseded** by this webhook + backstop and can
be ignored.

## One-time server setup (already done on the production box)

For reference / rebuilding the box:

```bash
# 1. webhook secret (shared with the GitHub webhook config)
openssl rand -hex 32 | sed 's/^/WEBHOOK_SECRET=/' | sudo tee /etc/agentty-deploy.env
sudo chmod 0600 /etc/agentty-deploy.env && sudo chown ayush:ayush /etc/agentty-deploy.env

# 2. passwordless sudo for the deploy steps
sudo cp deploy/agentty-deploy.sudoers /etc/sudoers.d/agentty-deploy
sudo chmod 0440 /etc/sudoers.d/agentty-deploy && sudo visudo -cf /etc/sudoers.d/agentty-deploy

# 3. log dir
sudo mkdir -p /var/log/agentty-deploy && sudo chown ayush:ayush /var/log/agentty-deploy

# 4. systemd units
sudo cp deploy/agentty-deploy-hook.service deploy/agentty-deploy.service deploy/agentty-deploy.timer /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now agentty-deploy-hook.service agentty-deploy.timer

# 5. nginx already proxies /_deploy-hook → 127.0.0.1:9099 (see agentty.org.nginx)
```

## The GitHub webhooks (the only GitHub-side step)

Add the SAME webhook to **both** repos (`1ay1/agentty` and `1ay1/agentty.org`):

- **Settings → Webhooks → Add webhook**
- Payload URL: `https://agentty.org/_deploy-hook`
- Content type: `application/json`
- Secret: the value from `/etc/agentty-deploy.env`
- Events: **Just the push event** — plus, on the agentty repo, also check
  **Releases** (so a new release refreshes the version badge).

That's it. After the webhooks are added, the whole thing is hands-off forever.

## Observability

```bash
systemctl status agentty-deploy-hook.service     # listener
journalctl -u agentty-deploy-hook.service -f     # incoming webhooks
systemctl list-timers agentty-deploy.timer       # next backstop run
tail -f /var/log/agentty-deploy/autodeploy.log   # deploy log
curl -fsS http://127.0.0.1:9099/health           # → ok
```
