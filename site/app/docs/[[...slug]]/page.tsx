import type { Metadata } from "next";
import { notFound } from "next/navigation";
import { Breadcrumb, EditThisPage } from "@/components/DocMeta";
import { DocNav } from "@/components/DocNav";
import { DocBody } from "@/components/DocBody";
import {
  getDoc,
  getDocSlugs,
  getAdjacentDocs,
  docSourcePath,
} from "@/lib/docs";
import { site } from "@/lib/site";

// One dynamic route renders every /docs page from the Markdown synced out of the
// agentty repo (docs/website/*.md → content/docs/*.md). Adding a doc is adding a
// file; no page component to touch.

export function generateStaticParams() {
  // "" (index) + every non-index slug. The optional catch-all matches /docs too.
  return [{ slug: [] as string[] }, ...getDocSlugs().map((slug) => ({ slug: [slug] }))];
}

type Params = { slug?: string[] };

function slugOf(params: Params): string {
  return params.slug?.join("/") ?? "";
}

export async function generateMetadata({
  params,
}: {
  params: Promise<Params>;
}): Promise<Metadata> {
  const doc = getDoc(slugOf(await params));
  if (!doc) return {};
  const canonical = doc.slug === "" ? "/docs" : `/docs/${doc.slug}`;
  return {
    title: doc.title,
    description: doc.description,
    alternates: { canonical },
    openGraph: { title: doc.title, description: doc.description, url: site.url + canonical },
  };
}

export default async function DocsPage({ params }: { params: Promise<Params> }) {
  const slug = slugOf(await params);
  const doc = getDoc(slug);
  if (!doc) notFound();

  const { prev, next } = getAdjacentDocs(doc.href);

  return (
    <>
      <Breadcrumb title={doc.title} />
      <h1>{doc.title}</h1>
      {doc.description && <p className="lead">{doc.description}</p>}

      <DocBody html={doc.html} />

      <DocNav prev={prev ? { title: prev.title, href: prev.href } : null}
              next={next ? { title: next.title, href: next.href } : null} />
      <EditThisPage path={docSourcePath(doc.slug)} repo="agentty" />
    </>
  );
}
