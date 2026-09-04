#!/usr/bin/env node
// Sync the docs source of truth (agentty repo: docs/website/*.md) into this
// site's content/docs/, then regenerate lib/docs-nav.generated.ts (the sidebar
// order derived from frontmatter). Run by deploy.sh before `next build`.
//
// Source resolution, in order:
//   1. AGENTTY_DOCS_DIR env var (an absolute path to a docs/website dir)
//   2. a sibling local checkout: ../agentty/docs/website
//   3. GitHub (raw.githubusercontent.com) at AGENTTY_DOCS_REF (default: master)
//
// Never hard-fails the deploy: on any error it leaves the committed content/docs
// and committed nav in place and exits 0 with a warning, so a network blip can't
// take the site down.

import {
  existsSync,
  mkdirSync,
  readdirSync,
  readFileSync,
  writeFileSync,
  rmSync,
} from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT = join(__dirname, "..");
const OUT_DIR = join(ROOT, "content", "docs");
const NAV_OUT = join(ROOT, "lib", "docs-nav.generated.ts");

const REF = process.env.AGENTTY_DOCS_REF || "master";
const REPO = "1ay1/agentty";
const RAW = `https://raw.githubusercontent.com/${REPO}/${REF}/docs/website`;
const API = `https://api.github.com/repos/${REPO}/contents/docs/website?ref=${REF}`;

const SECTION_ORDER = [
  "Getting Started",
  "User Manual",
  "Tools",
  "Advanced",
  "Help",
];

function log(msg) {
  console.log(`   [sync-docs] ${msg}`);
}

// ── 1. gather the markdown files (name -> content) ───────────────────────────
async function fromLocal(dir) {
  const files = readdirSync(dir).filter((f) => f.endsWith(".md") && f !== "README.md");
  return files.map((name) => ({ name, content: readFileSync(join(dir, name), "utf8") }));
}

async function fromGitHub() {
  const listRes = await fetch(API, {
    headers: { "User-Agent": "agentty-site-sync", Accept: "application/vnd.github+json" },
  });
  if (!listRes.ok) throw new Error(`GitHub API ${listRes.status}`);
  const entries = await listRes.json();
  const mdNames = entries
    .filter((e) => e.type === "file" && e.name.endsWith(".md") && e.name !== "README.md")
    .map((e) => e.name);
  const out = [];
  for (const name of mdNames) {
    const r = await fetch(`${RAW}/${name}`, { headers: { "User-Agent": "agentty-site-sync" } });
    if (!r.ok) throw new Error(`fetch ${name} → ${r.status}`);
    out.push({ name, content: await r.text() });
  }
  return out;
}

async function gather() {
  if (process.env.AGENTTY_DOCS_DIR && existsSync(process.env.AGENTTY_DOCS_DIR)) {
    log(`source: local dir ${process.env.AGENTTY_DOCS_DIR}`);
    return fromLocal(process.env.AGENTTY_DOCS_DIR);
  }
  // Unified repo: the site lives at <agentty>/site/, so the docs are the parent
  // repo's docs/website — no fetch needed, it's the same checkout.
  const inRepo = join(ROOT, "..", "docs", "website");
  if (existsSync(inRepo)) {
    log(`source: in-repo ${inRepo}`);
    return fromLocal(inRepo);
  }
  const sibling = join(ROOT, "..", "agentty", "docs", "website");
  if (existsSync(sibling)) {
    log(`source: sibling checkout ${sibling}`);
    return fromLocal(sibling);
  }
  log(`source: GitHub ${REPO}@${REF}`);
  return fromGitHub();
}

// ── 2. minimal frontmatter parse (matches lib/docs.ts) ───────────────────────
function parseFrontmatter(raw) {
  const m = raw.match(/^---\s*\n([\s\S]*?)\n---\s*\n?/);
  const data = {};
  if (m) {
    for (const line of m[1].split("\n")) {
      const kv = line.match(/^([A-Za-z0-9_]+)\s*:\s*(.*)$/);
      if (!kv) continue;
      let v = kv[2].trim();
      if ((v.startsWith('"') && v.endsWith('"')) || (v.startsWith("'") && v.endsWith("'")))
        v = v.slice(1, -1);
      data[kv[1]] = v;
    }
  }
  return data;
}

// ── 3. build nav array from frontmatter ──────────────────────────────────────
function buildNav(docs) {
  const bySection = new Map();
  for (const d of docs) {
    const fm = parseFrontmatter(d.content);
    const slug = fm.slug ?? d.name.replace(/\.md$/, "");
    const isIndex = d.name === "index.md" || slug === "";
    const item = {
      title: fm.title || slug,
      href: isIndex ? "/docs" : `/docs/${slug}`,
      section: fm.nav_section || "Advanced",
      order: Number(fm.nav_order ?? 999),
    };
    if (!bySection.has(item.section)) bySection.set(item.section, []);
    bySection.get(item.section).push(item);
  }
  const keys = [
    ...SECTION_ORDER.filter((s) => bySection.has(s)),
    ...[...bySection.keys()].filter((s) => !SECTION_ORDER.includes(s)),
  ];
  return keys.map((title) => ({
    title,
    items: bySection
      .get(title)
      .sort((a, b) => a.order - b.order)
      .map((i) => ({ title: i.title, href: i.href })),
  }));
}

function writeNav(nav) {
  const body = nav
    .map(
      (sec) =>
        `  {\n    title: ${JSON.stringify(sec.title)},\n    items: [\n` +
        sec.items
          .map((i) => `      { title: ${JSON.stringify(i.title)}, href: ${JSON.stringify(i.href)} },`)
          .join("\n") +
        `\n    ],\n  },`,
    )
    .join("\n");
  const out = `// AUTO-GENERATED by scripts/sync-docs.mjs from docs/website/*.md frontmatter.
// Do not edit by hand — change a doc's nav_section / nav_order instead.
// Committed so the site builds on a fresh checkout before the sync runs.

import type { NavSection } from "./site";

export const docsNav: NavSection[] = [
${body}
];
`;
  writeFileSync(NAV_OUT, out);
}

// ── main ─────────────────────────────────────────────────────────────────────
try {
  const docs = await gather();
  if (docs.length === 0) throw new Error("no docs found at source");

  // Replace content/docs wholesale so deletions in the source propagate.
  rmSync(OUT_DIR, { recursive: true, force: true });
  mkdirSync(OUT_DIR, { recursive: true });
  for (const d of docs) writeFileSync(join(OUT_DIR, d.name), d.content);

  writeNav(buildNav(docs));
  log(`synced ${docs.length} docs → content/docs/ and regenerated docs-nav.generated.ts`);
} catch (err) {
  console.warn(`   [sync-docs] SKIPPED (${err.message}); using committed content/docs + nav`);
  process.exit(0);
}

