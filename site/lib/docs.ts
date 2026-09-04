// Build-time docs loader. Reads Markdown files from content/docs/ (synced from
// the agentty repo's docs/website/ by scripts/sync-docs.mjs), parses frontmatter,
// derives the sidebar nav from it, and renders each page to HTML. Mirrors
// lib/blog.ts — runs only at build time (static export), zero runtime deps.

import { readFileSync, readdirSync, existsSync } from "node:fs";
import { join } from "node:path";
import { markdownToHtml } from "./markdown";
import { docsTokens } from "./docs-tokens";

const DOCS_DIR = join(process.cwd(), "content", "docs");

// Canonical sidebar section order. A page's nav_section must be one of these;
// unknown sections are appended after, in first-seen order.
const SECTION_ORDER = [
  "Getting Started",
  "User Manual",
  "Tools",
  "Advanced",
  "Help",
] as const;

export type DocMeta = {
  slug: string; // URL segment; "" for the index (/docs)
  href: string; // "/docs" or "/docs/<slug>"
  title: string;
  description: string;
  navSection: string;
  navOrder: number;
};

export type Doc = DocMeta & { html: string; headings: { id: string; text: string }[] };

export type NavItem = { title: string; href: string };
export type NavSection = { title: string; items: NavItem[] };

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

// Substitute {{token}} placeholders with live release/stats values.
function applyTokens(md: string): string {
  return md.replace(/\{\{(\w+)\}\}/g, (whole, key: string) =>
    key in docsTokens ? docsTokens[key as keyof typeof docsTokens] : whole,
  );
}

// Extract H2 headings (with their slugified ids) for the on-page TOC, matching
// the id scheme markdownToHtml uses.
function extractHeadings(md: string): { id: string; text: string }[] {
  const out: { id: string; text: string }[] = [];
  for (const line of md.split("\n")) {
    const h = line.match(/^##\s+(.*)$/);
    if (!h) continue;
    const text = h[1].trim();
    const id = text
      .toLowerCase()
      .replace(/`/g, "")
      .replace(/\[\[([^\]]+)\]\]/g, "$1")
      .replace(/[^\w\s-]/g, "")
      .replace(/\s+/g, "-");
    out.push({ id, text: text.replace(/`/g, "").replace(/\[\[([^\]]+)\]\]/g, "$1") });
  }
  return out;
}

function loadDoc(filename: string): Doc {
  const raw = readFileSync(join(DOCS_DIR, filename), "utf8");
  const { data, body } = parseFrontmatter(raw);
  const slug = data.slug ?? filename.replace(/\.md$/, "");
  const isIndex = filename === "index.md" || slug === "";
  const withTokens = applyTokens(body);
  return {
    slug: isIndex ? "" : slug,
    href: isIndex ? "/docs" : `/docs/${slug}`,
    title: data.title || slug,
    description: data.description || "",
    navSection: data.nav_section || "Advanced",
    navOrder: Number(data.nav_order ?? 999),
    html: markdownToHtml(withTokens),
    headings: extractHeadings(withTokens),
  };
}

let cache: Doc[] | null = null;

export function getAllDocs(): Doc[] {
  if (cache) return cache;
  if (!existsSync(DOCS_DIR)) return (cache = []);
  const files = readdirSync(DOCS_DIR).filter((f) => f.endsWith(".md") && f !== "README.md");
  cache = files.map(loadDoc);
  return cache;
}

export function getDoc(slug: string): Doc | undefined {
  // slug is the joined [[...slug]] catch-all; "" (or undefined) → index.
  const s = slug ?? "";
  return getAllDocs().find((d) => d.slug === s);
}

// Every non-index slug, for generateStaticParams.
export function getDocSlugs(): string[] {
  return getAllDocs()
    .filter((d) => d.slug !== "")
    .map((d) => d.slug);
}

// Ordered, flattened list matching the sidebar — used for prev/next.
export function getDocsFlat(): DocMeta[] {
  return getDocsNav().flatMap((s) => s.items.map((it) => ({
    ...(getAllDocs().find((d) => d.href === it.href) as DocMeta),
  })));
}

// Build the sidebar sections from frontmatter. Sections follow SECTION_ORDER;
// items within a section sort by navOrder.
export function getDocsNav(): NavSection[] {
  const docs = getAllDocs();
  const bySection = new Map<string, Doc[]>();
  for (const d of docs) {
    if (!bySection.has(d.navSection)) bySection.set(d.navSection, []);
    bySection.get(d.navSection)!.push(d);
  }
  const sectionKeys = [
    ...SECTION_ORDER.filter((s) => bySection.has(s)),
    ...[...bySection.keys()].filter((s) => !SECTION_ORDER.includes(s as never)),
  ];
  return sectionKeys.map((title) => ({
    title,
    items: bySection
      .get(title)!
      .slice()
      .sort((a, b) => a.navOrder - b.navOrder)
      .map((d) => ({ title: d.title, href: d.href })),
  }));
}

export function getAdjacentDocs(href: string): { prev?: DocMeta; next?: DocMeta } {
  const flat = getDocsFlat();
  const i = flat.findIndex((d) => d.href === href);
  if (i === -1) return {};
  return { prev: flat[i - 1], next: flat[i + 1] };
}

// The source path in the agentty repo, for the "Edit this page" link.
export function docSourcePath(slug: string): string {
  return `docs/website/${slug === "" ? "index" : slug}.md`;
}
