# agentty.org analytics

Two complementary layers give full visibility into the site. Everything is
**self-hosted** — no third parties, matching the project ethos.

## Layer 1 — GoAccess (server-side: every single request)

Parses nginx's per-site detailed log (`/var/log/nginx/agentty.access.log`,
defined as `log_format agentty_detailed` in `../agentty.org.nginx`). Captures
*everything* over the wire: humans, bots, crawlers, `curl | sh` installer hits,
asset fetches, 404s, referrers, user agents, latency (`rt=`). Can't be blocked.

- **Dashboard:** https://stats.agentty.org  (HTTP basic auth)
- **Live service:** `goaccess-agentty.service` (systemd, auto-restart, real-time
  WebSocket on 127.0.0.1:7890, proxied at `/ws`)
- **Output:** `/var/lib/goaccess/html/index.html` — deliberately OUTSIDE the
  deploy webroot so `rsync --delete` never wipes it.
- **State:** persisted in `/var/lib/goaccess` (survives restarts).

Manage:
```sh
sudo systemctl status  goaccess-agentty
sudo systemctl restart goaccess-agentty
./goaccess.sh            # regenerate a one-shot static report
```

Add/reset the dashboard password:
```sh
sudo htpasswd /etc/nginx/agentty.htpasswd ayush
```

## Layer 2 — Umami (client-side: human behavior, events, funnels)

Self-hosted Umami (Docker) on `analytics.agentty.org`. First-party tracker
(`/script.js`) is injected in `app/layout.tsx`, so ad-blockers that target
third-party domains leave it alone. No cookies.

- **Dashboard:** https://analytics.agentty.org
- **Default login:** `admin` / `umami` — **change immediately.**
- **Website ID:** `4dc82793-234c-474c-837f-8bc736d6b954`
- **Backend:** container on 127.0.0.1:3001, Postgres in `umami-db` volume.

Manage:
```sh
cd analytics && docker compose up -d     # start / update
docker compose logs -f umami             # tail
docker compose pull && docker compose up -d   # upgrade
```

### Custom events
`lib/analytics.ts` exposes `track(event, data)`. Wired so far:
- `copy-install` — fired when the install one-liner is copied (`CopyRow.tsx`).
- `github-click` — fired on the nav "Star on GitHub" button (`SiteNav.tsx`).

Add more by importing `track` and calling it in an `onClick`.

## DNS (required to go live)
Add two records in Cloudflare for the apex `agentty.org` zone (the TLS cert is
already a wildcard `*.agentty.org`, so no cert work is needed):

| Type  | Name       | Value            |
|-------|------------|------------------|
| A/CNAME | `stats`     | (same as `@`)   |
| A/CNAME | `analytics` | (same as `@`)   |

## Secrets
The Umami `APP_SECRET` and Postgres password live in `docker-compose.yml`.
Rotate them there if the repo is ever made public.
