import type { Metadata } from "next";
import Link from "next/link";
import { getComparePages } from "@/lib/seo-pages";

export const metadata: Metadata = {
  title: "agentty comparisons — how it stacks up against other coding agents",
  description:
    "Honest, side-by-side comparisons of agentty against Claude Code, Aider, and other terminal AI coding agents — speed, providers, sandboxing, and when to pick each.",
  alternates: { canonical: "/compare" },
};

export default function CompareIndex() {
  const pages = getComparePages();
  return (
    <main className="docs-main seo-page">
      <div className="wrap" style={{ maxWidth: 820, paddingTop: 40, paddingBottom: 72 }}>
        <p className="eyebrow">Comparisons</p>
        <h1>agentty vs other coding agents</h1>
        <p className="section-sub" style={{ marginBottom: 28 }}>
          Honest, side-by-side comparisons — including when to choose the other tool.
        </p>
        <ul className="seo-index">
          {pages.map((p) => (
            <li key={p.slug}>
              <Link href={p.href}>
                <span className="si-title">{p.title}</span>
                {p.verdict && <span className="si-sub">{p.verdict}</span>}
              </Link>
            </li>
          ))}
        </ul>
      </div>
    </main>
  );
}
