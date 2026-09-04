import type { Metadata } from "next";
import Link from "next/link";
import { getGuidePages } from "@/lib/seo-pages";

export const metadata: Metadata = {
  title: "agentty guides — run AI coding agents locally, with Ollama, Claude & more",
  description:
    "Practical guides for agentty: run a coding agent locally with Ollama, use Claude from the terminal, integrate providers, and more.",
  alternates: { canonical: "/guides" },
};

export default function GuidesIndex() {
  const pages = getGuidePages();
  return (
    <main className="docs-main seo-page">
      <div className="wrap" style={{ maxWidth: 820, paddingTop: 40, paddingBottom: 72 }}>
        <p className="eyebrow">Guides</p>
        <h1>agentty guides</h1>
        <p className="section-sub" style={{ marginBottom: 28 }}>
          Practical, copy-pasteable walkthroughs for getting the most out of agentty.
        </p>
        <ul className="seo-index">
          {pages.map((p) => (
            <li key={p.slug}>
              <Link href={p.href}>
                <span className="si-title">{p.title}</span>
                {p.description && <span className="si-sub">{p.description}</span>}
              </Link>
            </li>
          ))}
        </ul>
      </div>
    </main>
  );
}
