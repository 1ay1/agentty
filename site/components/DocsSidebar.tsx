"use client";

import Link from "next/link";
import { usePathname } from "next/navigation";
import { useState } from "react";
import { docsNav } from "@/lib/site";

export function DocsSidebar() {
  const path = usePathname();
  const [open, setOpen] = useState(false);
  const norm = (p: string) => (p.endsWith("/") ? p.slice(0, -1) : p) || "/";
  const current = norm(path);

  const currentTitle =
    docsNav.flatMap((s) => s.items).find((i) => norm(i.href) === current)?.title ??
    "Documentation";

  return (
    <>
      <button
        className="docs-side-toggle"
        onClick={() => setOpen((o) => !o)}
        aria-expanded={open}
      >
        <span>📚 {currentTitle}</span>
        <span>{open ? "▲" : "▼"}</span>
      </button>
      <aside className={`docs-side ${open ? "open" : ""}`} aria-label="Documentation">
        {docsNav.map((sec) => (
          <div className="sec" key={sec.title}>
            <p className="sec-title">{sec.title}</p>
            {sec.items.map((it) => (
              <Link
                key={it.href}
                href={it.href}
                onClick={() => setOpen(false)}
                className={current === norm(it.href) ? "active" : ""}
              >
                {it.title}
              </Link>
            ))}
          </div>
        ))}
      </aside>
    </>
  );
}
