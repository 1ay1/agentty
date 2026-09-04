// Build-time loader for SEO landing pages: comparison ("agentty vs X") and
// alternative ("open-source X alternative") pages. Content lives in
// content/compare/*.md and content/alternatives/*.md as markdown with a small
// frontmatter block — identical authoring model to the blog, so a new page is
// just a file drop that auto-deploys and auto-enters the sitemap.
//
// Frontmatter keys:
//   title        H1 + <title>
//   description  meta description / OG
//   competitor   the thing being compared/alternative'd (e.g. "Claude Code")
//   verdict       one-line honest takeaway (renders in the summary card)
//   updated      ISO date, shown + used for sitemap lastModified
//   kind         "compare" | "alternative"  (set by the loader, not the file)

import { readFileSync, readdirSync, existsSync } from "node:fs";
import { join } from "node:path";
import { markdownToHtml } from "./markdown";

export type SeoKind = "compare" | "alternative" | "guide";

export type SeoPage = {
  slug: string;
  kind: SeoKind;
  href: string;
  title: string;
  description: string;
  competitor: string;
  verdict: string;
  updated: string;
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

function loadDir(kind: SeoKind): SeoPage[] {
  const dirName =
    kind === "compare" ? "compare" : kind === "guide" ? "guides" : "alternatives";
  const dir = join(process.cwd(), "content", dirName);
  if (!existsSync(dir)) return [];
  const base =
    kind === "compare" ? "/compare" : kind === "guide" ? "/guides" : "/alternatives";
  return readdirSync(dir)
    .filter((f) => f.endsWith(".md"))
    .map((file) => {
      const slug = file.replace(/\.md$/, "");
      const raw = readFileSync(join(dir, file), "utf8");
      const { data, body } = parseFrontmatter(raw);
      const bodyNoTitle = body.replace(/^\s*#\s+.*(?:\n|$)/, "");
      return {
        slug,
        kind,
        href: `${base}/${slug}`,
        title: data.title || slug,
        description: data.description || "",
        competitor: data.competitor || "",
        verdict: data.verdict || "",
        updated: data.updated || new Date().toISOString().slice(0, 10),
        html: markdownToHtml(bodyNoTitle),
      };
    })
    .sort((a, b) => (a.title < b.title ? -1 : 1));
}

let compareCache: SeoPage[] | null = null;
let altCache: SeoPage[] | null = null;
let guideCache: SeoPage[] | null = null;

export function getComparePages(): SeoPage[] {
  return (compareCache ??= loadDir("compare"));
}
export function getAlternativePages(): SeoPage[] {
  return (altCache ??= loadDir("alternative"));
}
export function getGuidePages(): SeoPage[] {
  return (guideCache ??= loadDir("guide"));
}
export function getAllSeoPages(): SeoPage[] {
  return [...getComparePages(), ...getAlternativePages(), ...getGuidePages()];
}

export function getComparePage(slug: string): SeoPage | undefined {
  return getComparePages().find((p) => p.slug === slug);
}
export function getAlternativePage(slug: string): SeoPage | undefined {
  return getAlternativePages().find((p) => p.slug === slug);
}
export function getGuidePage(slug: string): SeoPage | undefined {
  return getGuidePages().find((p) => p.slug === slug);
}
