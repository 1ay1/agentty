#!/usr/bin/env node
// sync-content.mjs — pull ALL site content (blog + SEO landing pages) from the
// agentty repo, so the agentty.org site is a pure rendering runtime with zero
// hand-authored content. This is the sibling of sync-docs.mjs (which handles
// docs/website/*.md); together they make the agentty repo the single source of
// truth for everything on the site.
//
// Source of truth in the agentty repo:  docs/website/content/<section>/*.md
//   • blog/          → content/blog/
//   • compare/       → content/compare/
//   • guides/        → content/guides/
//   • alternatives/  → content/alternatives/
//
// Source resolution, in order (same contract as sync-docs.mjs):
//   1. AGENTTY_CONTENT_DIR env var (absolute path to docs/website/content)
//   2. sibling local checkout: ../agentty/docs/website/content
//   3. GitHub (raw.githubusercontent.com + contents API) at AGENTTY_DOCS_REF
//
// NEVER hard-fails the deploy: on any error per section it leaves the committed
// content in place and moves on, so a network blip can't take the site down.

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

const REF = process.env.AGENTTY_DOCS_REF || "master";
const REPO = "1ay1/agentty";
const BASE_PATH = "docs/website/content";
const RAW = `https://raw.githubusercontent.com/${REPO}/${REF}/${BASE_PATH}`;
const API = `https://api.github.com/repos/${REPO}/contents/${BASE_PATH}`;

// The content sections we sync, in source → destination mapping.
const SECTIONS = ["blog", "compare", "guides", "alternatives", "pages"];

function log(msg) {
  console.log(`   [sync-content] ${msg}`);
}

// ── source resolvers ─────────────────────────────────────────────────────────
function localBase() {
  if (process.env.AGENTTY_CONTENT_DIR && existsSync(process.env.AGENTTY_CONTENT_DIR)) {
    return process.env.AGENTTY_CONTENT_DIR;
  }
  // Unified repo: site is at <agentty>/site/, content is the parent's
  // docs/website/content — same checkout, no fetch.
  const inRepo = join(ROOT, "..", "docs", "website", "content");
  if (existsSync(inRepo)) return inRepo;
  const sibling = join(ROOT, "..", "agentty", "docs", "website", "content");
  if (existsSync(sibling)) return sibling;
  return null;
}

function fromLocalSection(base, section) {
  const dir = join(base, section);
  if (!existsSync(dir)) return null;
  return readdirSync(dir)
    .filter((f) => f.endsWith(".md") && f !== "README.md")
    .map((name) => ({ name, content: readFileSync(join(dir, name), "utf8") }));
}

async function fromGitHubSection(section) {
  const listRes = await fetch(`${API}/${section}?ref=${REF}`, {
    headers: { "User-Agent": "agentty-site-sync", Accept: "application/vnd.github+json" },
  });
  if (listRes.status === 404) return null; // section doesn't exist upstream yet
  if (!listRes.ok) throw new Error(`GitHub API ${listRes.status}`);
  const entries = await listRes.json();
  const names = entries
    .filter((e) => e.type === "file" && e.name.endsWith(".md") && e.name !== "README.md")
    .map((e) => e.name);
  const out = [];
  for (const name of names) {
    const r = await fetch(`${RAW}/${section}/${name}`, {
      headers: { "User-Agent": "agentty-site-sync" },
    });
    if (!r.ok) throw new Error(`fetch ${section}/${name} → ${r.status}`);
    out.push({ name, content: await r.text() });
  }
  return out;
}

// ── write a section's files, replacing wholesale so deletions propagate ───────
function writeSection(section, files) {
  const outDir = join(ROOT, "content", section);
  rmSync(outDir, { recursive: true, force: true });
  mkdirSync(outDir, { recursive: true });
  for (const f of files) writeFileSync(join(outDir, f.name), f.content);
}

// ── main ─────────────────────────────────────────────────────────────────────
const base = localBase();
if (base) log(`source: local ${base}`);
else log(`source: GitHub ${REPO}@${REF} (${BASE_PATH})`);

let synced = 0;
let skipped = 0;
for (const section of SECTIONS) {
  try {
    const files = base
      ? fromLocalSection(base, section)
      : await fromGitHubSection(section);
    if (files === null) {
      // Section not present upstream yet → keep whatever is committed.
      log(`${section}: not in source yet — keeping committed content`);
      skipped++;
      continue;
    }
    if (files.length === 0) {
      log(`${section}: source is empty — keeping committed content`);
      skipped++;
      continue;
    }
    writeSection(section, files);
    log(`${section}: synced ${files.length} file(s)`);
    synced++;
  } catch (err) {
    console.warn(`   [sync-content] ${section}: SKIPPED (${err.message}); using committed content`);
    skipped++;
  }
}

log(`done — ${synced} section(s) synced, ${skipped} left as committed`);
process.exit(0);
