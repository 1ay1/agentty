"use client";

import Link from "next/link";
import { usePathname } from "next/navigation";
import { useEffect, useRef, useState } from "react";
import { site, topNav } from "@/lib/site";
import { versionLabel } from "@/lib/release";
import { ThemeToggle } from "@/components/ThemeToggle";
import { track } from "@/lib/analytics";

const MENU_ID = "mobile-menu";

export function SiteNav() {
  const [open, setOpen] = useState(false);
  const pathname = usePathname();
  const toggleRef = useRef<HTMLButtonElement>(null);
  const menuRef = useRef<HTMLDivElement>(null);

  const close = () => setOpen(false);

  const openPalette = () => {
    window.dispatchEvent(new Event("agentty:open-palette"));
  };

  // Auto-close on route change so the drawer never lingers over a new page.
  useEffect(() => {
    setOpen(false);
  }, [pathname]);

  // While open: lock body scroll, close on Escape, close on outside-click,
  // and return focus to the toggle when it closes. This is the standard
  // disclosure-menu contract every user expects on a phone.
  useEffect(() => {
    if (!open) return;

    const prevOverflow = document.body.style.overflow;
    document.body.style.overflow = "hidden";

    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") {
        setOpen(false);
        toggleRef.current?.focus();
      }
    };
    const onPointer = (e: PointerEvent) => {
      const t = e.target as Node;
      if (menuRef.current?.contains(t) || toggleRef.current?.contains(t)) return;
      setOpen(false);
    };

    document.addEventListener("keydown", onKey);
    // pointerdown (not click) so a tap outside dismisses immediately,
    // before it can activate anything underneath.
    document.addEventListener("pointerdown", onPointer);

    return () => {
      document.body.style.overflow = prevOverflow;
      document.removeEventListener("keydown", onKey);
      document.removeEventListener("pointerdown", onPointer);
    };
  }, [open]);

  return (
    <header className="nav" id="top">
      <div className="wrap nav-inner">
        <Link className="brand" href="/" aria-label="agentty home" onClick={close}>
          <span className="brand-mark" aria-hidden="true">▌</span>
          <span className="brand-name">agentty</span>
          <span className="brand-ver" aria-label={`version ${versionLabel}`}>{versionLabel}</span>
        </Link>
        <nav className="nav-links" aria-label="Primary">
          {topNav.map((n) => (
            <Link
              key={n.href}
              href={n.href}
              aria-current={pathname === n.href ? "page" : undefined}
            >
              {n.title}
            </Link>
          ))}
        </nav>
        <span className="nav-spacer" />
        <div className="nav-cta">
          <button
            className="cmdk-trigger"
            aria-label="Open command palette"
            onClick={openPalette}
          >
            <span className="cmdk-trigger-ico">⌘</span>
            <span className="cmdk-trigger-label">Jump to…</span>
            <kbd className="cmdk-trigger-kbd">⌘K</kbd>
          </button>
          <ThemeToggle />
          <a
            className="ghbtn discordbtn"
            href={site.discord}
            target="_blank"
            rel="noopener noreferrer"
            aria-label="Join the agentty Discord"
            title="Join the Discord"
            onClick={() => track("discord-click", { from: "nav" })}
          >
            <svg viewBox="0 0 16 16" width="16" height="16" aria-hidden="true">
              <path
                fill="currentColor"
                d="M13.545 2.907a13.2 13.2 0 0 0-3.257-1.011.05.05 0 0 0-.052.025c-.141.25-.297.577-.406.833a12.2 12.2 0 0 0-3.658 0 8 8 0 0 0-.412-.833.05.05 0 0 0-.052-.025c-1.125.194-2.22.534-3.257 1.011a.04.04 0 0 0-.021.018C.356 6.024-.213 9.047.066 12.032q.003.022.021.037a13.3 13.3 0 0 0 3.995 2.02.05.05 0 0 0 .056-.019q.463-.63.818-1.329a.05.05 0 0 0-.01-.059l-.018-.011a9 9 0 0 1-1.248-.595.05.05 0 0 1-.02-.066l.015-.019q.127-.095.248-.195a.05.05 0 0 1 .051-.007c2.619 1.196 5.454 1.196 8.041 0a.05.05 0 0 1 .053.007q.121.1.248.195a.05.05 0 0 1-.004.085 8 8 0 0 1-1.249.594.05.05 0 0 0-.03.03.05.05 0 0 0 .003.041q.36.698.817 1.329a.05.05 0 0 0 .056.019 13.2 13.2 0 0 0 4.001-2.02.05.05 0 0 0 .021-.037c.334-3.451-.559-6.449-2.366-9.106a.03.03 0 0 0-.02-.019m-8.198 7.307c-.789 0-1.438-.724-1.438-1.612s.637-1.613 1.438-1.613c.807 0 1.45.73 1.438 1.613 0 .888-.637 1.612-1.438 1.612m5.316 0c-.788 0-1.438-.724-1.438-1.612s.637-1.613 1.438-1.613c.807 0 1.451.73 1.438 1.613 0 .888-.631 1.612-1.438 1.612"
              />
            </svg>
            <span>Discord</span>
          </a>
          <a
            className="ghbtn"
            href={site.github}
            target="_blank"
            rel="noopener noreferrer"
            onClick={() => track("github-click", { from: "nav" })}
          >
            <svg viewBox="0 0 16 16" width="16" height="16" aria-hidden="true">
              <path
                fill="currentColor"
                d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.013 8.013 0 0016 8c0-4.42-3.58-8-8-8z"
              />
            </svg>
            <span>Star on GitHub</span>
          </a>
        </div>
        <button
          ref={toggleRef}
          className="nav-toggle"
          aria-label={open ? "Close menu" : "Open menu"}
          aria-expanded={open}
          aria-controls={MENU_ID}
          onClick={() => setOpen((o) => !o)}
        >
          {open ? "✕" : "☰"}
        </button>
      </div>

      {open && (
        <div className="mobile-menu" id={MENU_ID} ref={menuRef}>
          <nav className="mobile-menu-links" aria-label="Mobile">
            {topNav.map((n) => (
              <Link
                key={n.href}
                href={n.href}
                aria-current={pathname === n.href ? "page" : undefined}
                onClick={close}
              >
                {n.title}
              </Link>
            ))}
            <a href={site.discord} target="_blank" rel="noopener noreferrer" onClick={close}>
              Discord ↗
            </a>
            <a href={site.github} target="_blank" rel="noopener noreferrer" onClick={close}>
              GitHub ↗
            </a>
          </nav>

          {/* Utilities are hidden from the desktop nav on phones — surface them
              here so search + theme are always reachable, not just on desktop. */}
          <div className="mobile-menu-utils">
            <button
              type="button"
              className="mobile-util"
              onClick={() => {
                close();
                openPalette();
              }}
            >
              <span className="mobile-util-ico" aria-hidden="true">⌘</span>
              Search
            </button>
            <div className="mobile-util mobile-util-theme">
              <span>Theme</span>
              <ThemeToggle />
            </div>
          </div>
        </div>
      )}
    </header>
  );
}
