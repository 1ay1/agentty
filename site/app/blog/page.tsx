import type { Metadata } from "next";
import Link from "next/link";
import { getAllPosts, formatDate } from "@/lib/blog";
import { site } from "@/lib/site";

export const metadata: Metadata = {
  title: "Blog",
  description: `Release notes, performance deep dives, and design notes from the ${site.name} project — the native C++26 claude-code alternative.`,
  keywords: [
    "agentty blog",
    "terminal coding agent",
    "claude code alternative",
    "c++ coding agent",
    "AI agent architecture",
  ],
  alternates: {
    canonical: "/blog",
    types: { "application/rss+xml": `${site.url}/blog/feed.xml` },
  },
  openGraph: {
    type: "website",
    title: "agentty blog",
    description: `Release notes, performance deep dives, and design notes from the ${site.name} project.`,
    url: `${site.url}/blog/`,
    siteName: "agentty",
  },
};

export default function BlogIndex() {
  const posts = getAllPosts();
  const [featured, ...rest] = posts;

  const jsonLd = {
    "@context": "https://schema.org",
    "@graph": [
      {
        "@type": "Blog",
        "@id": `${site.url}/blog/#blog`,
        name: "agentty blog",
        description: `Release notes, performance deep dives, and design notes from the ${site.name} project.`,
        url: `${site.url}/blog/`,
        inLanguage: "en",
        publisher: { "@type": "Organization", name: "agentty", url: site.url },
      },
      {
        "@type": "ItemList",
        itemListElement: posts.map((p, idx) => ({
          "@type": "ListItem",
          position: idx + 1,
          url: `${site.url}/blog/${p.slug}/`,
          name: p.title,
        })),
      },
    ],
  };

  return (
    <section className="block blog-index">
      <script
        type="application/ld+json"
        dangerouslySetInnerHTML={{ __html: JSON.stringify(jsonLd) }}
      />
      <div className="wrap" style={{ maxWidth: 980 }}>
        <p className="eyebrow">Blog</p>
        <h1 className="section-title" style={{ marginBottom: 10 }}>
          Notes from building {site.name}.
        </h1>
        <p className="section-sub" style={{ marginBottom: 40 }}>
          Release notes, performance deep dives, and design notes — written the same way the
          code is: measured, specific, no fluff.
        </p>

        {posts.length === 0 ? (
          <p className="blog-empty">No posts yet. Check back soon.</p>
        ) : (
          <>
            {featured && (
              <Link href={`/blog/${featured.slug}`} className="blog-featured" data-reveal>
                <div className="blog-featured-glow" aria-hidden />
                <div className="blog-featured-body">
                  <span className="blog-featured-badge">Latest</span>
                  <div className="blog-card-meta">
                    <time dateTime={featured.date}>{formatDate(featured.date)}</time>
                    <span className="dot">·</span>
                    <span>{featured.readingMinutes} min read</span>
                  </div>
                  <h2 className="blog-featured-title">{featured.title}</h2>
                  <p className="blog-featured-excerpt">{featured.excerpt}</p>
                  <div className="blog-featured-foot">
                    {featured.tags.length > 0 && (
                      <div className="blog-tags">
                        {featured.tags.map((t) => (
                          <span key={t} className="blog-tag">
                            {t}
                          </span>
                        ))}
                      </div>
                    )}
                    <span className="blog-card-more">Read post →</span>
                  </div>
                </div>
              </Link>
            )}

            {rest.length > 0 && (
              <div className="blog-grid">
                {rest.map((p) => (
                  <Link key={p.slug} href={`/blog/${p.slug}`} className="blog-card" data-reveal>
                    <div className="blog-card-meta">
                      <time dateTime={p.date}>{formatDate(p.date)}</time>
                      <span className="dot">·</span>
                      <span>{p.readingMinutes} min</span>
                    </div>
                    <h2 className="blog-card-title">{p.title}</h2>
                    <p className="blog-card-excerpt">{p.excerpt}</p>
                    <div className="blog-card-bottom">
                      {p.tags.length > 0 && (
                        <div className="blog-tags">
                          {p.tags.slice(0, 2).map((t) => (
                            <span key={t} className="blog-tag">
                              {t}
                            </span>
                          ))}
                        </div>
                      )}
                      <span className="blog-card-more">→</span>
                    </div>
                  </Link>
                ))}
              </div>
            )}
          </>
        )}
      </div>
    </section>
  );
}
