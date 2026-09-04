"use client";

import { useEffect, useState } from "react";
import dynamic from "next/dynamic";

// The digital-rain <canvas> is the heaviest decorative client component. The
// static CSS layers (grid, glow, scanlines) render instantly via HeroBackdrop;
// the animated canvas is loaded only after the browser is idle, so it never
// blocks LCP or first input. Looks identical at first paint.
const HeroBackground = dynamic(
  () => import("./HeroBackground").then((m) => m.HeroBackground),
  { ssr: false },
);

export function HeroBackgroundLazy() {
  const [ready, setReady] = useState(false);

  useEffect(() => {
    const reduce = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
    if (reduce) return; // honor reduced-motion: never load the canvas
    const w = window as Window & {
      requestIdleCallback?: (cb: () => void, opts?: { timeout: number }) => number;
    };
    const start = () => setReady(true);
    if (typeof w.requestIdleCallback === "function") {
      w.requestIdleCallback(start, { timeout: 2500 });
    } else {
      const t = window.setTimeout(start, 1400);
      return () => window.clearTimeout(t);
    }
  }, []);

  // Static backdrop paints immediately; the animated canvas swaps in when idle.
  if (!ready) {
    return (
      <div className="hero-bg" aria-hidden>
        <div className="hero-bg-grid" />
        <div className="hero-bg-glow" />
        <div className="hero-bg-scan" />
      </div>
    );
  }
  return <HeroBackground />;
}
