"use client";

import { useEffect, useRef, useState } from "react";

type Heading = { id: string; text: string; level: number };

export function TableOfContents() {
  const [headings, setHeadings] = useState<Heading[]>([]);
  const [active, setActive] = useState<string>("");
  const activeRef = useRef<string>("");

  useEffect(() => {
    const nodes = Array.from(
      document.querySelectorAll<HTMLElement>(".docs-main h2[id], .docs-main h3[id]")
    );
    setHeadings(
      nodes.map((n) => ({
        id: n.id,
        text: n.textContent ?? "",
        level: n.tagName === "H3" ? 3 : 2,
      }))
    );
    if (nodes.length === 0) return;

    // Offset the "reading line" just below the sticky nav so a heading counts
    // as active once its top crosses that line.
    const navH =
      parseInt(
        getComputedStyle(document.documentElement).getPropertyValue("--nav-h")
      ) || 64;
    const readLine = navH + 24;

    const setActiveId = (id: string) => {
      if (activeRef.current !== id) {
        activeRef.current = id;
        setActive(id);
      }
    };

    // Scroll-position based spy: the active heading is the LAST one whose top is
    // above the reading line. This is robust at the top (falls back to the first
    // heading) and at the bottom (explicitly pins the last heading once the page
    // is scrolled to its end) — the two edge cases a pure IntersectionObserver
    // band gets wrong.
    let ticking = false;
    const compute = () => {
      ticking = false;

      const scrollY = window.scrollY;
      const viewportH = window.innerHeight;
      const docH = document.documentElement.scrollHeight;

      // At (or within 2px of) the bottom, always highlight the last heading —
      // otherwise trailing headings that can't reach the reading line never
      // activate.
      if (scrollY + viewportH >= docH - 2) {
        setActiveId(nodes[nodes.length - 1].id);
        return;
      }

      let current = nodes[0].id;
      let found = false;
      for (const n of nodes) {
        if (n.getBoundingClientRect().top <= readLine) {
          current = n.id;
          found = true;
        } else {
          break;
        }
      }
      // Before the first heading crosses the line, keep the first item active
      // (so there's never a "nothing selected" flash at the top of the page).
      if (!found) current = nodes[0].id;
      setActiveId(current);
    };

    const onScroll = () => {
      if (!ticking) {
        ticking = true;
        requestAnimationFrame(compute);
      }
    };

    compute();
    window.addEventListener("scroll", onScroll, { passive: true });
    window.addEventListener("resize", onScroll, { passive: true });
    return () => {
      window.removeEventListener("scroll", onScroll);
      window.removeEventListener("resize", onScroll);
    };
  }, []);

  if (headings.length < 2) return <aside className="docs-toc" aria-hidden />;

  const onClick = (id: string) => {
    // Sync immediately on click so the highlight doesn't lag the smooth scroll.
    setActive(id);
    activeRef.current = id;
  };

  return (
    <aside className="docs-toc" aria-label="On this page">
      <p className="t">On this page</p>
      {headings.map((h) => (
        <a
          key={h.id}
          href={`#${h.id}`}
          onClick={() => onClick(h.id)}
          aria-current={active === h.id ? "true" : undefined}
          className={`${active === h.id ? "active" : ""} ${h.level === 3 ? "sub" : ""}`}
        >
          {h.text}
        </a>
      ))}
    </aside>
  );
}
