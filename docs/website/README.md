# Website docs — source of truth for agentty.org/docs

Every file in this directory is **one page** on <https://agentty.org/docs>. The
site (repo `1ay1/agentty.org`) fetches this directory at build time and renders
each `.md` through a dynamic route — there are no hand-written page components on
the site anymore. Edit docs *here*, next to the code.

Deploys are driven by a **GitHub webhook** on this repo pointed at
`https://agentty.org/_deploy-hook`, handled by the `agentty-deploy-hook`
service on the web host: it pulls this directory, rebuilds, and reloads nginx.
A systemd timer re-runs the same deploy every 30 minutes as a backstop, so the
site also picks up anything it fetches live from GitHub (release version,
binary sizes, star count) without a push.

There is deliberately NO workflow in this repo that deploys the site. There was
one — it sent a `repository_dispatch` and needed a `SITE_DISPATCH_TOKEN` that
was never set, so it logged "skipping site dispatch" and exited 0 on every run
for months. A green check that does nothing is worse than no check, and the
webhook had been doing the real work the whole time.

## File → page mapping

- `index.md` → `/docs`
- `installation.md` → `/docs/installation`
- `<name>.md` → `/docs/<name>`

## Frontmatter (required)

```yaml
---
title: Quick Start                 # page <h1> + <title> + sidebar label
description: One-line summary.      # <meta description> + og
nav_section: Getting Started       # sidebar group (see order below)
nav_order: 30                      # position within the group (ascending)
slug: quick-start                  # URL segment; omit on index.md (empty = /docs)
---
```

`nav_section` must be one of, in this order:
`Getting Started`, `User Manual`, `Tools`, `Advanced`, `Help`.
The sidebar, prev/next, and breadcrumb are all derived from frontmatter — adding
a page is just adding a file with correct `nav_section` + `nav_order`.

## Markdown dialect

Standard Markdown (headings, lists, tables, fenced code, links, bold/italic,
blockquote, hr) **plus** these site extensions:

- **Keys:** `[[Ctrl+K]]` → a `<kbd>` chip. Use for every keyboard shortcut.
- **Callouts:** a fenced block with `:::`
  ```
  :::tip
  Bring your own model: an API key, a local Ollama model, or Claude OAuth.
  :::
  ```
  Types: `:::note`, `:::tip`, `:::warn`. Optional label: `:::warn Heads up`.
- **Template tokens** (substituted at build time from the live release/stats data):
  - `{{version}}` → e.g. `0.2.8`
  - `{{versionLabel}}` → e.g. `v0.2.8`
  - `{{sizeMB}}` → e.g. `13.6 MB`
  - `{{coldStartMs}}` → e.g. `~2 ms`
  - `{{installOneLiner}}` → the curl one-liner
  - `{{installOneLinerWindows}}` → the PowerShell one-liner
  - `{{github}}`, `{{releasesLatest}}`
- **Directives** for the few pieces of dynamic UI that can't be prose:
  - `:::release-table` on its own line → the per-platform download table
    (name/size/sha256), rendered from the live release data.

## Internal links

Link to other docs with root-relative paths: `[Authentication](/docs/authentication)`.
Anchor links to a heading on the same page: `[jump](#some-heading)` — heading ids
are the slugified heading text (lowercase, non-word → `-`).

## Headings

Do **not** put an `# H1` in the body — the page renders the `title` from
frontmatter as the `<h1>`. Start the body at `## H2`. Every `## H2` gets an
auto-generated `id` for the on-page table of contents.
