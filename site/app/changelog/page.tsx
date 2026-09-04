import type { Metadata } from "next";
import { readFileSync, existsSync } from "node:fs";
import { join } from "node:path";
import { markdownToHtml } from "@/lib/markdown";
import { site } from "@/lib/site";

// The changelog is rendered straight from the agentty repo's CHANGELOG.md,
// synced into content/changelog.md on every deploy (scripts/sync-changelog.mjs).
// Zero hand-authoring — edit CHANGELOG.md in 1ay1/agentty and it appears here.

export const metadata: Metadata = {
  title: "Changelog",
  description:
    "Release history for agentty — every version's added, changed, and fixed, straight from the repo's CHANGELOG.md.",
  alternates: { canonical: "/changelog" },
  openGraph: {
    title: "agentty changelog",
    description: "Every agentty release: added, changed, fixed.",
    url: `${site.url}/changelog`,
    images: [`${site.url}/opengraph-image`],
  },
  twitter: {
    card: "summary_large_image",
    title: "agentty changelog",
    description: "Every agentty release: added, changed, fixed.",
    images: [`${site.url}/twitter-image`],
  },
};

function loadChangelog(): string {
  const path = join(process.cwd(), "content", "changelog.md");
  if (!existsSync(path)) return "";
  let md = readFileSync(path, "utf8");
  // Drop the top-level "# Changelog" heading + intro line; the page supplies its
  // own <h1>, so we render from the first version section down.
  md = md.replace(/^#\s+Changelog\s*\n/, "").replace(/^All notable changes[^\n]*\n/m, "");
  return md.trim();
}

export default function ChangelogPage() {
  const html = markdownToHtml(loadChangelog());
  return (
    <div className="page changelog-page">
      <h1>Changelog</h1>
      <p className="lead">
        Every agentty release — added, changed, and fixed. Sourced live from{" "}
        <a href={`${site.github}/blob/master/CHANGELOG.md`} target="_blank" rel="noopener noreferrer">
          CHANGELOG.md
        </a>{" "}
        in the repo.
      </p>
      <div className="doc-body" dangerouslySetInnerHTML={{ __html: html }} />
    </div>
  );
}
