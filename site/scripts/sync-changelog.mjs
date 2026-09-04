#!/usr/bin/env node
// sync-changelog.mjs — pull CHANGELOG.md from the agentty repo into
// content/changelog.md so the /changelog page always reflects the real release
// history with zero human intervention. Same 3-tier source + never-fails
// contract as sync-docs.mjs / sync-content.mjs.

import { existsSync, readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT = join(__dirname, "..");
const OUT = join(ROOT, "content", "changelog.md");

const REF = process.env.AGENTTY_DOCS_REF || "master";
const REPO = "1ay1/agentty";
const RAW = `https://raw.githubusercontent.com/${REPO}/${REF}/CHANGELOG.md`;

function localSource() {
  if (process.env.AGENTTY_DIR && existsSync(join(process.env.AGENTTY_DIR, "CHANGELOG.md")))
    return join(process.env.AGENTTY_DIR, "CHANGELOG.md");
  // Unified repo: site is at <agentty>/site/, CHANGELOG is the parent's.
  const inRepo = join(ROOT, "..", "CHANGELOG.md");
  if (existsSync(inRepo)) return inRepo;
  const sibling = join(ROOT, "..", "agentty", "CHANGELOG.md");
  if (existsSync(sibling)) return sibling;
  return null;
}

try {
  let text;
  const local = localSource();
  if (local) {
    text = readFileSync(local, "utf8");
    console.log(`   [sync-changelog] source: local ${local}`);
  } else {
    const r = await fetch(RAW, { headers: { "User-Agent": "agentty-site-sync" } });
    if (!r.ok) throw new Error(`GitHub ${r.status}`);
    text = await r.text();
    console.log(`   [sync-changelog] source: GitHub ${REPO}@${REF}`);
  }
  if (!text.trim()) throw new Error("empty changelog");
  mkdirSync(dirname(OUT), { recursive: true });
  writeFileSync(OUT, text);
  console.log(`   [sync-changelog] wrote content/changelog.md (${text.split("\n").length} lines)`);
} catch (err) {
  console.warn(`   [sync-changelog] SKIPPED (${err.message}); using committed content/changelog.md`);
  process.exit(0);
}
