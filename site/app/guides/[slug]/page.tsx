import type { Metadata } from "next";
import Link from "next/link";
import { notFound } from "next/navigation";
import { getGuidePages, getGuidePage } from "@/lib/seo-pages";
import { site } from "@/lib/site";

export function generateStaticParams() {
  return getGuidePages().map((p) => ({ slug: p.slug }));
}

export async function generateMetadata({
  params,
}: {
  params: Promise<{ slug: string }>;
}): Promise<Metadata> {
  const { slug } = await params;
  const page = getGuidePage(slug);
  if (!page) return { title: "Not found" };
  return {
    title: page.title,
    description: page.description,
    keywords: [
      page.competitor,
      "terminal coding agent",
      "AI coding agent",
      "agentty guide",
    ].filter(Boolean),
    alternates: { canonical: page.href },
    openGraph: {
      title: page.title,
      description: page.description,
      type: "article",
      url: `${site.url}${page.href}/`,
      siteName: "agentty",
      images: [`${site.url}/opengraph-image`],
    },
    twitter: {
      card: "summary_large_image",
      title: page.title,
      description: page.description,
      images: [`${site.url}/twitter-image`],
    },
  };
}

export default async function GuidePage({
  params,
}: {
  params: Promise<{ slug: string }>;
}) {
  const { slug } = await params;
  const page = getGuidePage(slug);
  if (!page) notFound();

  const jsonLd = {
    "@context": "https://schema.org",
    "@type": "TechArticle",
    headline: page.title,
    description: page.description,
    dateModified: page.updated,
    author: { "@type": "Organization", name: "agentty" },
    mainEntityOfPage: `${site.url}${page.href}/`,
  };

  return (
    <main className="docs-main seo-page">
      <script type="application/ld+json" dangerouslySetInnerHTML={{ __html: JSON.stringify(jsonLd) }} />
      <div className="wrap" style={{ maxWidth: 820, paddingTop: 40, paddingBottom: 72 }}>
        <p className="eyebrow">Guide</p>
        <h1>{page.title}</h1>
        {page.verdict && (
          <div className="note" style={{ marginTop: 8 }}>
            <strong>TL;DR:</strong> {page.verdict}
          </div>
        )}
        <article className="doc-body" dangerouslySetInnerHTML={{ __html: page.html }} />

        <div className="seo-cta">
          <p>Install agentty in one line — a single static binary.</p>
          <code>{site.installOneLiner}</code>
          <div className="seo-cta-actions">
            <Link className="btn btn-primary" href="/docs/installation">Get started</Link>
            <Link className="btn btn-ghost" href="/guides">All guides →</Link>
          </div>
        </div>

        <RelatedGuides slug={slug} />
      </div>
    </main>
  );
}

function RelatedGuides({ slug }: { slug: string }) {
  const others = getGuidePages().filter((p) => p.slug !== slug).slice(0, 6);
  if (others.length === 0) return null;
  return (
    <nav className="seo-related" aria-label="More guides">
      <p className="t">More guides</p>
      <ul>
        {others.map((p) => (
          <li key={p.slug}>
            <Link href={p.href}>{p.title}</Link>
          </li>
        ))}
      </ul>
    </nav>
  );
}
