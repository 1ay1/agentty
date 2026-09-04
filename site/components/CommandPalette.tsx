"use client";

import { useEffect, useMemo, useRef, useState } from "react";
import { useRouter } from "next/navigation";
import { docsNav, topNav, site } from "@/lib/site";

type Item = { title: string; href: string; section: string; external?: boolean };

const index: Item[] = [
  { title: "Home", href: "/", section: "Pages" },
  ...topNav.map((n) => ({ ...n, section: "Pages" })),
  ...docsNav.flatMap((s) => s.items.map((i) => ({ ...i, section: s.title }))),
  { title: "Acknowledgements", href: "/acknowledgements", section: "Pages" },
  { title: "License", href: "/license", section: "Pages" },
  { title: "Security", href: "/security", section: "Pages" },
  { title: "Contributing", href: "/contributing", section: "Pages" },
  { title: "Code of Conduct", href: "/code-of-conduct", section: "Pages" },
  { title: "GitHub repository ↗", href: site.github, section: "Links", external: true },
  { title: "Latest release ↗", href: site.releasesLatest, section: "Links", external: true },
];

// tiny fuzzy: subsequence match + contiguous bonus
function score(q: string, t: string): number {
  if (!q) return 1;
  const s = t.toLowerCase();
  let qi = 0;
  let sc = 0;
  let streak = 0;
  for (let i = 0; i < s.length && qi < q.length; i++) {
    if (s[i] === q[qi]) {
      qi++;
      streak++;
      sc += streak;
    } else {
      streak = 0;
    }
  }
  return qi === q.length ? sc : 0;
}

export function CommandPalette({ startOpen = false }: { startOpen?: boolean }) {
  const [open, setOpen] = useState(startOpen);
  const [q, setQ] = useState("");
  const [sel, setSel] = useState(0);
  const router = useRouter();
  const inputRef = useRef<HTMLInputElement>(null);

  const results = useMemo(() => {
    const query = q.trim().toLowerCase();
    if (!query) return index.slice(0, 9);
    return index
      .map((it) => ({ it, sc: score(query, it.title) }))
      .filter((r) => r.sc > 0)
      .sort((a, b) => b.sc - a.sc)
      .slice(0, 10)
      .map((r) => r.it);
  }, [q]);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === "k") {
        e.preventDefault();
        setOpen((o) => !o);
      }
      if (e.key === "Escape") setOpen(false);
    };
    // The nav trigger + mobile "Search" dispatch this event; listening for it
    // here (not just ⌘K) is what makes click-to-open work every time — the lazy
    // wrapper only arms once, so after the first close it can't re-open us.
    const onOpen = () => setOpen(true);
    window.addEventListener("keydown", onKey);
    window.addEventListener("agentty:open-palette", onOpen);
    return () => {
      window.removeEventListener("keydown", onKey);
      window.removeEventListener("agentty:open-palette", onOpen);
    };
  }, []);

  useEffect(() => {
    if (open) setTimeout(() => inputRef.current?.focus(), 10);
    else {
      setQ("");
      setSel(0);
    }
  }, [open]);

  useEffect(() => setSel(0), [q]);

  // Keep the highlighted result visible when arrowing past the fold.
  const resultsRef = useRef<HTMLDivElement>(null);
  useEffect(() => {
    const el = resultsRef.current?.querySelector<HTMLElement>(".cmdk-item.sel");
    el?.scrollIntoView({ block: "nearest" });
  }, [sel]);

  const go = (it: Item) => {
    setOpen(false);
    if (it.external) window.open(it.href, "_blank", "noopener,noreferrer");
    else router.push(it.href);
  };

  if (!open) return null;

  return (
    <div className="cmdk-overlay" onClick={() => setOpen(false)}>
      <div
        className="cmdk"
        role="dialog"
        aria-modal="true"
        aria-label="Jump to a page or doc"
        onClick={(e) => e.stopPropagation()}
      >
        <div className="cmdk-top">
          <span className="cmdk-edge">╭</span>
          <span className="cmdk-fill" />
          <span className="cmdk-cap"> JUMP TO </span>
          <span className="cmdk-fill" />
          <span className="cmdk-edge">╮</span>
        </div>
        <div className="cmdk-inputrow">
          <span className="cmdk-prompt">❯</span>
          <input
            ref={inputRef}
            className="cmdk-input"
            aria-label="Search pages and docs"
            placeholder="jump to a page or doc…"
            value={q}
            onChange={(e) => setQ(e.target.value)}
            onKeyDown={(e) => {
              if (e.key === "ArrowDown") {
                e.preventDefault();
                setSel((s) => Math.min(s + 1, results.length - 1));
              }
              if (e.key === "ArrowUp") {
                e.preventDefault();
                setSel((s) => Math.max(s - 1, 0));
              }
              if (e.key === "Enter" && results[sel]) go(results[sel]);
            }}
          />
        </div>

        <div className="cmdk-results" ref={resultsRef} role="listbox" aria-label="Results">
          {results.length === 0 && (
            <div className="cmdk-empty">no match for “{q}”</div>
          )}
          {results.map((r, i) => (
            <button
              key={r.href + r.title}
              className={`cmdk-item ${i === sel ? "sel" : ""}`}
              role="option"
              aria-selected={i === sel}
              onMouseEnter={() => setSel(i)}
              onClick={() => go(r)}
            >
              <span className="cmdk-glyph">{i === sel ? "▌" : " "}</span>
              <span className="cmdk-title">{r.title}</span>
              <span className="cmdk-section">{r.section}</span>
            </button>
          ))}
        </div>

        <div className="cmdk-foot">
          <span>
            <kbd>↑</kbd>
            <kbd>↓</kbd> move
          </span>
          <span>
            <kbd>↵</kbd> open
          </span>
          <span>
            <kbd>esc</kbd> close
          </span>
        </div>
        <div className="cmdk-bot">
          <span className="cmdk-edge">╰</span>
          <span className="cmdk-fill" />
          <span className="cmdk-edge">╯</span>
        </div>
      </div>
    </div>
  );
}
