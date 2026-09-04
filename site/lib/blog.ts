// Build-time blog loader. Reads Markdown files from content/blog/, parses a
// small YAML-ish frontmatter block, and exposes a sorted post list + per-slug
// lookup. Runs only at build time (static export), so Node fs is fine and zero
// runtime deps ship to the browser.

import { readFileSync, readdirSync, existsSync } from "node:fs";
import { join } from "node:path";
import { markdownToHtml } from "./markdown";

const BLOG_DIR = join(process.cwd(), "content", "blog");

export type PostMeta = {
  slug: string;
  title: string;
  date: string; // ISO yyyy-mm-dd
  excerpt: string;
  author: string;
  tags: string[];
  readingMinutes: number;
  wordCount: number;
};

export type FaqItem = { question: string; answer: string };

export type Post = PostMeta & { html: string; faq: FaqItem[] };

// Minimal frontmatter parser: a leading `---` block of `key: value` lines.
// Values may be quoted; `tags` accepts an inline [a, b] array or comma list.
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

function parseTags(v?: string): string[] {
  if (!v) return [];
  return v
    .replace(/^\[|\]$/g, "")
    .split(",")
    .map((t) => t.trim().replace(/^["']|["']$/g, ""))
    .filter(Boolean);
}

function readingTime(text: string): number {
  const words = text.trim().split(/\s+/).length;
  return Math.max(1, Math.round(words / 200));
}

// Pulls Q&A pairs out of a "## Frequently asked" section (bold question line
// followed by a plain-text answer paragraph) to feed FAQPage JSON-LD. Purely
// additive — posts without that heading just get an empty faq array.
function parseFaq(body: string): FaqItem[] {
  const section = body.match(/\n##\s+Frequently asked\s*\n([\s\S]*?)(?:\n##\s|$)/);
  if (!section) return [];
  const items: FaqItem[] = [];
  const re = /\*\*(.+?)\*\*\s*\n([\s\S]*?)(?=\n\*\*|$)/g;
  let m: RegExpExecArray | null;
  while ((m = re.exec(section[1]))) {
    const question = m[1].trim();
    const answer = m[2]
      .trim()
      .replace(/\[([^\]]+)\]\([^)]+\)/g, "$1")
      .replace(/[*`_]/g, "")
      .replace(/\s+/g, " ");
    if (question && answer) items.push({ question, answer });
  }
  return items;
}

function loadPost(filename: string): Post {
  const slug = filename.replace(/\.md$/, "");
  const raw = readFileSync(join(BLOG_DIR, filename), "utf8");
  const { data, body } = parseFrontmatter(raw);
  // The page header renders the title from frontmatter, so drop a leading H1 in
  // the body to avoid showing the headline twice.
  const bodyNoTitle = body.replace(/^\s*#\s+.*(?:\n|$)/, "");
  const html = markdownToHtml(bodyNoTitle);
  const excerpt =
    data.excerpt ||
    body
      .replace(/^#.*$/gm, "")
      .replace(/[#>*`_\-]/g, "")
      .trim()
      .slice(0, 160)
      .trim() + "…";
  return {
    slug,
    title: data.title || slug,
    date: data.date || "1970-01-01",
    excerpt,
    author: data.author || "agentty",
    tags: parseTags(data.tags),
    readingMinutes: readingTime(body),
    wordCount: body.trim().split(/\s+/).filter(Boolean).length,
    html,
    faq: parseFaq(body),
  };
}

let cache: Post[] | null = null;

export function getAllPosts(): Post[] {
  if (cache) return cache;
  if (!existsSync(BLOG_DIR)) return (cache = []);
  const files = readdirSync(BLOG_DIR).filter((f) => f.endsWith(".md"));
  cache = files
    .map(loadPost)
    .sort((a, b) => (a.date < b.date ? 1 : a.date > b.date ? -1 : 0));
  return cache;
}

export function getPost(slug: string): Post | undefined {
  return getAllPosts().find((p) => p.slug === slug);
}

// The next (older) post after the given slug, for the end-of-article CTA.
export function getAdjacent(slug: string): { next?: PostMeta; prev?: PostMeta } {
  const all = getAllPosts();
  const i = all.findIndex((p) => p.slug === slug);
  if (i === -1) return {};
  return { prev: all[i - 1], next: all[i + 1] };
}

export function formatDate(iso: string): string {
  const d = new Date(iso + "T00:00:00");
  return d.toLocaleDateString("en-US", { year: "numeric", month: "long", day: "numeric" });
}
