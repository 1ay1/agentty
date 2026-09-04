import type { Metadata } from "next";
import Link from "next/link";
import { getAlternativePages } from "@/lib/seo-pages";

export const metadata: Metadata = {
  title: "Open-source coding agent alternatives — Claude Code, Aider, Cursor & more",
  description:
    "agentty is an open-source, native terminal coding agent — a fast, multi-provider alternative to Claude Code, Aider, Cursor, Codex CLI, and more.",
  alternates: { canonical: "/alternatives" },
};

export default function AlternativesIndex() {
  const pages = getAlternativePages();
  return (
    <main className="docs-main seo-page">
      <div className="wrap" style={{ maxWidth: 820, paddingTop: 40, paddingBottom: 72 }}>
        <p className="eyebrow">Alternatives</p>
        <h1>Open-source coding agent alternatives</h1>
        <p className="section-sub" style={{ marginBottom: 28 }}>
          Looking to switch? Here's how agentty compares as an alternative to the popular coding agents.
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
