// Loader for top-level content pages (security, contributing, code-of-conduct,
// license, community, acknowledgements) — synced from the agentty repo at
// docs/website/content/pages/*.md by scripts/sync-content.mjs. Rendering these
// from Markdown (instead of hand-written .tsx) means the agentty repo is the
// single source of truth for EVERY page; the site never needs a content push.

import { readFileSync, readdirSync, existsSync } from "node:fs";
import { join } from "node:path";
import { markdownToHtml } from "./markdown";

export type ContentPage = {
  slug: string;
  title: string;
  description: string;
  html: string;
};

function parseFrontmatter(raw: string): { data: Record<string, string>; body: string } {
  const m = raw.match(/^---\s*\n([\s\S]*?)\n---\s*\n?([\s\S]*)$/);
  if (!m) return { data: {}, body: raw };
  const data: Record<string, string> = {};
  for (const line of m[1].split("\n")) {
    const kv = line.match(/^([A-Za-z0-9_]+)\s*:\s*(.*)$/);
    if (!kv) continue;
    let v = kv[2].trim();
    if ((v.startsWith('"') && v.endsWith('"')) || (v.startsWith("'") && v.endsWith("'"))) {
      v = v.slice(1, -1);
    }
    data[kv[1]] = v;
  }
  return { data, body: m[2] };
}

const DIR = join(process.cwd(), "content", "pages");

let cache: ContentPage[] | null = null;

export function getContentPages(): ContentPage[] {
  if (cache) return cache;
  if (!existsSync(DIR)) return (cache = []);
  cache = readdirSync(DIR)
    .filter((f) => f.endsWith(".md"))
    .map((file) => {
      const raw = readFileSync(join(DIR, file), "utf8");
      const { data, body } = parseFrontmatter(raw);
      const slug = data.slug || file.replace(/\.md$/, "");
      // strip a leading H1 if present; the route renders <h1> from the title
      const bodyNoTitle = body.replace(/^\s*#\s+.*(?:\n|$)/, "");
      return {
        slug,
        title: data.title || slug,
        description: data.description || "",
        html: markdownToHtml(bodyNoTitle),
      };
    });
  return cache;
}

export function getContentPage(slug: string): ContentPage | undefined {
  return getContentPages().find((p) => p.slug === slug);
}

export function getContentPageSlugs(): string[] {
  return getContentPages().map((p) => p.slug);
}
