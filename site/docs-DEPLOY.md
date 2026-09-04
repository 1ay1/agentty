# Docs pipeline & auto-deploy

The `/docs` section of agentty.org is **rendered from Markdown that lives in the
agentty repo** — not from page components in this repo. You edit docs next to the
code; a push rebuilds and redeploys the site automatically.

## Where things live

| Piece | Location |
|-------|----------|
| **Docs source of truth** | `1ay1/agentty` → `docs/website/*.md` (one file per page) |
| Format spec for authors | `1ay1/agentty` → `docs/website/README.md` |
| Synced copy (committed fallback) | this repo → `content/docs/*.md` |
| Loader (frontmatter → nav + HTML) | `lib/docs.ts`, `lib/docs-tokens.ts` |
| Markdown renderer (kbd, callouts, tokens, directives) | `lib/markdown.ts` |
| Generated sidebar order | `lib/docs-nav.generated.ts` |
| The one route that renders every doc | `app/docs/[[...slug]]/page.tsx` |
| Sync script | `scripts/sync-docs.mjs` (run by `deploy.sh`) |

## The flow

```
edit docs/website/*.md in the agentty repo
        │  push to master (paths: docs/website/**)
        ▼
agentty  .github/workflows/docs-site.yml
        │  repository_dispatch: docs-updated  →  1ay1/agentty.org
        ▼
agentty.org  .github/workflows/deploy.yml   (self-hosted runner on the web box)
        │  ./deploy.sh
        │     ├─ scripts/sync-docs.mjs   (fetch docs/website from GitHub@master)
        │     ├─ measure-stats / fetch-release / fetch-repo
        │     ├─ next build  →  out/
        │     └─ rsync out/ → /var/www/agentty.org  + reload nginx
        ▼
https://agentty.org/docs   (live)
```

Pushing to **this** repo's `master` also triggers the deploy (site-code changes).
`workflow_dispatch` lets you run it manually from the Actions tab.

## Editing docs (the common case)

1. Edit or add a `.md` file under `docs/website/` in the **agentty** repo.
2. Frontmatter drives everything — see `docs/website/README.md`. To add a page,
   add a file with `title`, `description`, `nav_section`, `nav_order`, `slug`.
3. Push. The site redeploys in ~1 min. No change needed in this repo.

Preview locally from a sibling checkout (`../agentty`): `npm run dev` — the
loader reads `../agentty/docs/website` automatically. Or run
`node scripts/sync-docs.mjs` then `npm run build`.

## One-time setup for full automation

Two things must be configured once (they can't be committed):

### 1. `SITE_DISPATCH_TOKEN` secret on the agentty repo

`docs-site.yml` needs a token to POST a `repository_dispatch` to this repo.
Create a fine-grained PAT with **Contents: read + Metadata: read** on
`1ay1/agentty.org`, then add it as an Actions secret named
`SITE_DISPATCH_TOKEN` on the **agentty** repo. Without it, the dispatch step
no-ops (docs still deploy on the next push to this repo, just not instantly).

### 2. Self-hosted runner on the web box

`deploy.yml` runs on `[self-hosted, agentty-web]` because `deploy.sh` writes to
`/var/www` and reloads nginx locally. On the production server:

```bash
# GitHub → agentty.org repo → Settings → Actions → Runners → New self-hosted runner
# follow the shown steps, then add the label:
./config.sh --url https://github.com/1ay1/agentty.org --token <RUNNER_TOKEN> --labels agentty-web
sudo ./svc.sh install && sudo ./svc.sh start     # run as a service
```

`deploy.sh` uses `sudo` for the rsync/nginx steps, so the runner's user needs
passwordless sudo for exactly those commands. Add a sudoers drop-in:

```
# /etc/sudoers.d/agentty-deploy   (visudo -f)
<runner-user> ALL=(root) NOPASSWD: /usr/bin/mkdir, /usr/bin/rsync, /usr/bin/chown, /usr/bin/cp, /usr/bin/ln, /usr/sbin/nginx, /usr/bin/systemctl reload nginx
```

Until the runner exists, deploy by hand on the box: `./deploy.sh`.
</content>
