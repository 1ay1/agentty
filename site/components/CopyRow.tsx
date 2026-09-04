"use client";

import { useEffect, useRef, useState } from "react";
import { track } from "@/lib/analytics";

export function CopyRow({ cmd, typed = false }: { cmd: string; typed?: boolean }) {
  const [copied, setCopied] = useState(false);
  const [flash, setFlash] = useState(false);
  const [shown, setShown] = useState(typed ? "" : cmd);
  const [caret, setCaret] = useState(typed);
  const [marquee, setMarquee] = useState(false);
  const rootRef = useRef<HTMLDivElement>(null);
  const codeRef = useRef<HTMLElement>(null);
  const ran = useRef(false);

  // Marquee the command back-and-forth ONLY when it's wider than its box (i.e.
  // truncated on narrow screens), so the whole line stays readable without a
  // manual scroll. Measured after the text settles + on resize; disabled under
  // reduced-motion.
  useEffect(() => {
    const measure = () => {
      const el = codeRef.current;
      if (!el || caret) return; // wait until typing is done
      const inner = el.querySelector<HTMLElement>(".copyrow-cmd");
      if (!inner) return;
      const reduce = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
      // temporarily neutralise any transform so we measure true content width
      const overflow = inner.scrollWidth - el.clientWidth;
      if (reduce || overflow <= 2) {
        setMarquee(false);
        el.style.removeProperty("--marquee-shift");
      } else {
        // shift far enough to reveal the hidden tail, plus a little breathing room
        el.style.setProperty("--marquee-shift", `-${overflow + 12}px`);
        setMarquee(true);
      }
    };
    measure();
    window.addEventListener("resize", measure);
    return () => window.removeEventListener("resize", measure);
  }, [shown, caret]);

  // type the command out, once, when it scrolls into view
  useEffect(() => {
    if (!typed) return;
    const el = rootRef.current;
    if (!el) return;
    const reduce = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
    if (reduce) {
      setShown(cmd);
      setCaret(false);
      return;
    }
    const io = new IntersectionObserver(
      (entries) => {
        entries.forEach((e) => {
          if (e.isIntersecting && !ran.current) {
            ran.current = true;
            let i = 0;
            const step = () => {
              i++;
              setShown(cmd.slice(0, i));
              if (i < cmd.length) {
                window.setTimeout(step, 26 + Math.random() * 28);
              } else {
                window.setTimeout(() => setCaret(false), 700);
              }
            };
            window.setTimeout(step, 250);
            io.unobserve(e.target);
          }
        });
      },
      { threshold: 0.5 },
    );
    io.observe(el);
    // Safety net: never leave the command blank if the typing observer doesn't
    // fire (deferred mount, IO edge cases) — settle to the full command.
    const fallback = window.setTimeout(() => {
      if (!ran.current) {
        ran.current = true;
        io.disconnect();
        setShown(cmd);
        setCaret(false);
      }
    }, 3500);
    return () => {
      io.disconnect();
      window.clearTimeout(fallback);
    };
  }, [cmd, typed]);

  const copy = () => {
    navigator.clipboard?.writeText(cmd).catch(() => {});
    setCopied(true);
    setFlash(true);
    setTimeout(() => setFlash(false), 520);
    setTimeout(() => setCopied(false), 1400);
    track("copy-install", { cmd });
  };

  return (
    <div className={`copyrow ${flash ? "flash" : ""} ${marquee ? "has-marquee" : ""}`} ref={rootRef}>
      <span className="prompt">$</span>
      <code ref={codeRef} className={marquee ? "marquee" : ""}>
        <span className="copyrow-cmd">
          {shown}
          {caret && <span className="copyrow-caret" />}
        </span>
      </code>
      <button
        className="copybtn"
        onClick={copy}
        aria-label={copied ? "Command copied to clipboard" : `Copy command: ${cmd}`}
      >
        <span aria-hidden="true">{copied ? "copied ✓" : "copy"}</span>
      </button>
      <span className="sr-only" role="status" aria-live="polite">
        {copied ? "Copied to clipboard" : ""}
      </span>
    </div>
  );
}
