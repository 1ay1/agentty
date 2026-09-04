# agentty.org

The homepage and documentation site for [**agentty**](https://github.com/1ay1/agentty) — blazing-fast Claude in your terminal.

Built with **Next.js 15** (App Router) as a fully static export, deployed behind nginx with a Let's Encrypt SSL cert. Live at **[agentty.org](https://agentty.org)**.

## Develop

```bash
npm install
npm run dev        # http://localhost:3000
```

## Build

```bash
npm run build      # static export → out/
```

## Deploy

The static export in `out/` is rsynced to the nginx webroot at `/var/www/agentty.org`:

```bash
npm run build
sudo rsync -a --delete out/ /var/www/agentty.org/
```

nginx serves it over HTTPS (TLS 1.2/1.3, HSTS, gzip, immutable cache on
`/_next/static/`); the cert is issued and renewed via certbot + Let's Encrypt.
The vhost lives at `/etc/nginx/sites-available/agentty.org`.

## Structure

| Path | Contents |
|------|----------|
| `app/page.tsx` | Landing page (hero, speed, features, compare, tools, CTA) |
| `app/docs/` | Documentation + user manual (18 pages) |
| `app/{contributing,security,community,…}` | OSS essentials |
| `components/` | Nav, footer, sidebar, doc helpers, animated logo + TUI replica |
| `components/HeroBackground.tsx` | Digital-rain hero backdrop |
| `lib/site.ts` | Site config + navigation tree |
| `deploy.sh` | Build + deploy + cert helper |
