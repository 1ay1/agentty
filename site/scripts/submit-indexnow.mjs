#!/usr/bin/env node
// Ping IndexNow (Bing, and everything that consumes Bing's index — DuckDuckGo,
// Ecosia, Yandex) with every URL in the sitemap whenever it changes, so new
// posts/docs get crawled in minutes instead of waiting on the next organic
// crawl. Google doesn't consume IndexNow directly — for Google, submitting
// sitemap.xml in Search Console + a correct <lastmod> (already done in
// app/sitemap.ts) is what matters; this script is the free, zero-auth win
// for the rest of the search graph.
//
// Never fails the deploy: any network error just logs a warning and exits 0.
//
//   node scripts/submit-indexnow.mjs

import { readFileSync, existsSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT = join(__dirname, "..");
const HOST = "agentty.org";
const KEY = "91744a4beb9b4bc4cb8d38ab6011db02";
const KEY_LOCATION = `https://${HOST}/${KEY}.txt`;

function bail(msg) {
  console.warn(`[submit-indexnow] ${msg}`);
  process.exit(0);
}

const sitemapPath = join(ROOT, "out", "sitemap.xml");
if (!existsSync(sitemapPath)) bail("no out/sitemap.xml — run after `next build` export");

const xml = readFileSync(sitemapPath, "utf8");
const urlList = [...xml.matchAll(/<loc>([^<]+)<\/loc>/g)].map((m) => m[1]);
if (urlList.length === 0) bail("sitemap had no <loc> entries");

const body = JSON.stringify({
  host: HOST,
  key: KEY,
  keyLocation: KEY_LOCATION,
  urlList,
});

try {
  const res = await fetch("https://api.indexnow.org/indexnow", {
    method: "POST",
    headers: { "Content-Type": "application/json; charset=utf-8" },
    body,
  });
  console.log(`[submit-indexnow] submitted ${urlList.length} URLs → HTTP ${res.status}`);
} catch (err) {
  bail(`request failed (${err.message}) — skipping`);
}
