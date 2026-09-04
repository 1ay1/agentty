import { getAllPosts, formatDate } from "@/lib/blog";
import { site } from "@/lib/site";

export const dynamic = "force-static";

// Hand-rolled RSS 2.0 feed (zero deps, same philosophy as the rest of the
// site). Built once at export time. Linked from <head> via the blog layout's
// `alternates.types` so feed readers and search crawlers discover it.
const esc = (s: string) =>
  s
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&apos;");

export function GET() {
  const posts = getAllPosts();
  const updated = posts[0]
    ? new Date(`${posts[0].date}T00:00:00Z`).toUTCString()
    : new Date().toUTCString();

  const items = posts
    .map((p) => {
      const url = `${site.url}/blog/${p.slug}/`;
      const pub = new Date(`${p.date}T00:00:00Z`).toUTCString();
      const cats = p.tags.map((t) => `<category>${esc(t)}</category>`).join("");
      return `    <item>
      <title>${esc(p.title)}</title>
      <link>${url}</link>
      <guid isPermaLink="true">${url}</guid>
      <pubDate>${pub}</pubDate>
      <description>${esc(p.excerpt)}</description>
      <author>noreply@agentty.org (${esc(p.author)})</author>
      ${cats}
    </item>`;
    })
    .join("\n");

  const xml = `<?xml version="1.0" encoding="UTF-8"?>
<rss version="2.0" xmlns:atom="http://www.w3.org/2005/Atom">
  <channel>
    <title>agentty blog</title>
    <link>${site.url}/blog/</link>
    <atom:link href="${site.url}/blog/feed.xml" rel="self" type="application/rss+xml" />
    <description>Release notes, performance deep dives, and design notes from the agentty project — the native C++26 claude-code alternative.</description>
    <language>en-us</language>
    <lastBuildDate>${updated}</lastBuildDate>
    <generator>agentty.org</generator>
${items}
  </channel>
</rss>
`;

  return new Response(xml, {
    headers: {
      "Content-Type": "application/rss+xml; charset=utf-8",
      "Cache-Control": "public, max-age=3600",
    },
  });
}
