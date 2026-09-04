"use client";

import { useEffect, useRef } from "react";

/**
 * A geeky, GPU-cheap hero backdrop:
 *   • a faint dot/grid lattice (CSS)
 *   • a canvas "digital rain" of monospace glyphs falling in the brand
 *     palette — denser and brighter near the head of each column, fading
 *     to nothing in the tail, so it reads as code streaming, not noise.
 *
 * Respects prefers-reduced-motion (renders one static frame, no RAF loop).
 * Pointer-events: none — it never eats clicks. z-index sits behind content.
 */

// glyphs that read as "source": operators, hex, braille, box-drawing, katakana
const GLYPHS =
  "01{}[]()<>/=+-*&|!?;:.#$@_λ→∴⟨⟩∇∂∑01アイウエオカキクケコ▓▒░╱╲┃━┏┓┗┛".split("");

const COLORS = ["#d946ef", "#22d3ee", "#e879f9", "#7a6e91"];

export function HeroBackground() {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;

    const reduced =
      typeof window !== "undefined" &&
      window.matchMedia &&
      window.matchMedia("(prefers-reduced-motion: reduce)").matches;

    const FONT = 15;
    let cols = 0;
    let drops: number[] = [];
    let speeds: number[] = [];
    let dpr = 1;
    // resolve the self-hosted mono family so the canvas matches the page
    const mono =
      getComputedStyle(document.documentElement)
        .getPropertyValue("--font-mono")
        .trim() || "monospace";
    // Cache the trail-fade color once per resize/theme instead of calling
    // getComputedStyle every frame (that read forces a style/layout reflow).
    let fade = "rgba(8, 9, 12, 0.10)";
    function readFade() {
      fade =
        getComputedStyle(document.documentElement)
          .getPropertyValue("--matrix-fade")
          .trim() || "rgba(8, 9, 12, 0.10)";
    }
    readFade();

    function resize() {
      if (!canvas || !ctx) return;
      dpr = Math.min(window.devicePixelRatio || 1, 2);
      const w = canvas.clientWidth;
      const h = canvas.clientHeight;
      canvas.width = Math.floor(w * dpr);
      canvas.height = Math.floor(h * dpr);
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      cols = Math.ceil(w / FONT);
      drops = new Array(cols).fill(0).map(() => Math.random() * -40);
      speeds = new Array(cols).fill(0).map(() => 0.25 + Math.random() * 0.55);
      ctx.font = `${FONT}px ${mono}, monospace`;
    }

    function frame() {
      if (!canvas || !ctx) return;
      const w = canvas.clientWidth;
      const h = canvas.clientHeight;

      // translucent wipe → motion trails (the classic rain fade).
      // `fade` is cached (see readFade) so this loop never reads layout.
      ctx.fillStyle = fade;
      ctx.fillRect(0, 0, w, h);

      for (let i = 0; i < cols; i++) {
        const x = i * FONT;
        const y = drops[i] * FONT;
        const g = GLYPHS[(Math.random() * GLYPHS.length) | 0];

        // head glyph: bright; below it a short colored tail handled by trail fade
        const lead = Math.random() < 0.04;
        ctx.fillStyle = lead ? "#f2ecfb" : COLORS[(i + (drops[i] | 0)) % COLORS.length];
        ctx.globalAlpha = lead ? 0.9 : 0.42;
        ctx.fillText(g, x, y);
        ctx.globalAlpha = 1;

        if (y > h && Math.random() > 0.975) drops[i] = Math.random() * -20;
        drops[i] += speeds[i];
      }
    }

    resize();
    window.addEventListener("resize", resize);
    // re-read theme-dependent colors when the user toggles light/dark
    const themeObs = new MutationObserver(readFade);
    themeObs.observe(document.documentElement, { attributes: true, attributeFilter: ["data-theme"] });

    let raf = 0;
    let last = 0;
    const FRAME_MS = 1000 / 30; // cap at 30fps — plenty for rain, halves CPU
    let visible = true; // in viewport
    let active = !document.hidden; // tab focused

    // pause the loop when the hero scrolls out of view
    const io = new IntersectionObserver(
      ([e]) => {
        visible = e.isIntersecting;
      },
      { threshold: 0 },
    );
    if (canvas) io.observe(canvas);

    const onVisibility = () => {
      active = !document.hidden;
    };
    document.addEventListener("visibilitychange", onVisibility);

    if (reduced) {
      // single static pass — no RAF loop
      for (let p = 0; p < 60; p++) frame();
    } else {
      const loop = (now: number) => {
        raf = requestAnimationFrame(loop);
        if (!visible || !active) return;
        if (now - last < FRAME_MS) return;
        last = now;
        frame();
      };
      raf = requestAnimationFrame(loop);
    }

    return () => {
      window.removeEventListener("resize", resize);
      document.removeEventListener("visibilitychange", onVisibility);
      themeObs.disconnect();
      io.disconnect();
      if (raf) cancelAnimationFrame(raf);
    };
  }, []);

  return (
    <div className="hero-bg" aria-hidden>
      <canvas ref={canvasRef} className="hero-bg-canvas" />
      <div className="hero-bg-grid" />
      <div className="hero-bg-glow" />
      <div className="hero-bg-scan" />
    </div>
  );
}
