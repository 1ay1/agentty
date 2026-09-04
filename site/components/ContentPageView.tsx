import type { Metadata } from "next";
import { notFound } from "next/navigation";
import { getContentPage } from "@/lib/pages";
import { site } from "@/lib/site";

// Thin renderer for a repo-sourced content page. Every static page (security,
// contributing, …) is just this + a slug; the prose lives in the agentty repo
// at docs/website/content/pages/<slug>.md and is synced on deploy. No content
// is authored here — this file never needs to change.

export function renderContentMetadata(slug: string): Metadata {
  const page = getContentPage(slug);
  if (!page) return {};
  return {
    title: page.title,
    description: page.description,
    alternates: { canonical: `/${slug}` },
    openGraph: {
      title: page.title,
      description: page.description,
      url: `${site.url}/${slug}`,
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

export function ContentPageView({ slug }: { slug: string }) {
  const page = getContentPage(slug);
  if (!page) notFound();
  return (
    <div className="page">
      <h1>{page.title}</h1>
      {page.description && <p className="lead">{page.description}</p>}
      <div className="doc-body" dangerouslySetInnerHTML={{ __html: page.html }} />
    </div>
  );
}
