import type { MetadataRoute } from "next";
import { site } from "@/lib/site";
import { docsNav } from "@/lib/site";
import { getAllPosts } from "@/lib/blog";
import { getComparePages, getAlternativePages, getGuidePages } from "@/lib/seo-pages";

export const dynamic = "force-static";

export default function sitemap(): MetadataRoute.Sitemap {
  const staticRoutes = [
    "/",
    "/blog",
    "/changelog",
    "/contributing",
    "/security",
    "/code-of-conduct",
    "/license",
    "/community",
    "/acknowledgements",
  ];
  const docRoutes = docsNav.flatMap((s) => s.items.map((i) => i.href));
  const posts = getAllPosts();

  const staticEntries: MetadataRoute.Sitemap = staticRoutes.map((path) => ({
    url: `${site.url}${path === "/" ? "" : path}/`,
    lastModified: new Date(),
    changeFrequency: "weekly",
    priority: path === "/" ? 1 : path === "/blog" ? 0.9 : 0.7,
  }));

  const docEntries: MetadataRoute.Sitemap = docRoutes.map((path) => ({
    url: `${site.url}${path}/`,
    lastModified: new Date(),
    changeFrequency: "weekly",
    priority: 0.7,
  }));

  // Blog posts: lastModified is the post's own date (stable, not "now"),
  // and they get the highest non-home priority — they're the freshest,
  // most link-worthy content.
  const blogEntries: MetadataRoute.Sitemap = posts.map((p) => ({
    url: `${site.url}/blog/${p.slug}/`,
    lastModified: new Date(`${p.date}T00:00:00Z`),
    changeFrequency: "monthly",
    priority: 0.8,
  }));

  // SEO landing pages (compare / alternatives / guides) — high intent, so
  // they get strong priority. Auto-included as soon as a markdown file lands.
  const seoPages = [...getComparePages(), ...getAlternativePages(), ...getGuidePages()];
  const seoEntries: MetadataRoute.Sitemap = seoPages.map((p) => ({
    url: `${site.url}${p.href}/`,
    lastModified: new Date(`${p.updated}T00:00:00Z`),
    changeFrequency: "monthly",
    priority: 0.85,
  }));

  // index/hub pages for the SEO clusters
  const hubEntries: MetadataRoute.Sitemap = ["/compare", "/alternatives", "/guides"].map(
    (path) => ({
      url: `${site.url}${path}/`,
      lastModified: new Date(),
      changeFrequency: "weekly" as const,
      priority: 0.7,
    }),
  );

  return [...staticEntries, ...docEntries, ...blogEntries, ...seoEntries, ...hubEntries];
}
