"use client";

import { useEffect, useRef, useState } from "react";

/**
 * Odometer-style count-up that fires once the element scrolls into view.
 * `value` is a display string like "8.8 MB", "< 1 ms", "0", "C++26".
 * We animate the leading numeric part and keep the prefix/suffix verbatim;
 * non-numeric values (e.g. "C++26") just fade in.
 */
export function CountUp({ value }: { value: string }) {
  const ref = useRef<HTMLDivElement>(null);
  const [display, setDisplay] = useState(value);
  const started = useRef(false);

  useEffect(() => {
    const el = ref.current;
    if (!el) return;
    const reduce = window.matchMedia("(prefers-reduced-motion: reduce)").matches;

    // parse: optional prefix, a number, optional suffix
    const m = value.match(/^(\D*?)([\d.]+)(.*)$/s);
    // Only animate when the prefix is punctuation/symbol/space ("< ", "~", "$").
    // If it contains a LETTER (e.g. "C++26" → prefix "C++"), the string isn't a
    // metric — it's a label like a version, so fade it in verbatim instead of
    // counting "C++0 → C++26". Matches the component's documented contract.
    const prefixHasLetter = m ? /\p{L}/u.test(m[1]) : false;
    if (reduce || !m || prefixHasLetter) {
      setDisplay(value);
      return;
    }
    const [, prefix, numStr, suffix] = m;
    const target = parseFloat(numStr);
    const decimals = (numStr.split(".")[1] ?? "").length;
    setDisplay(`${prefix}0${decimals ? "." + "0".repeat(decimals) : ""}${suffix}`);

    const io = new IntersectionObserver(
      (entries) => {
        entries.forEach((e) => {
          if (e.isIntersecting && !started.current) {
            started.current = true;
            const dur = 1200;
            const t0 = performance.now();
            const tick = (now: number) => {
              const p = Math.min((now - t0) / dur, 1);
              // easeOutExpo
              const eased = p === 1 ? 1 : 1 - Math.pow(2, -10 * p);
              const v = (target * eased).toFixed(decimals);
              setDisplay(`${prefix}${v}${suffix}`);
              if (p < 1) requestAnimationFrame(tick);
              else setDisplay(value);
            };
            requestAnimationFrame(tick);
            io.unobserve(e.target);
          }
        });
      },
      { threshold: 0.4 },
    );
    io.observe(el);

    // Safety net: if the observer never fires (element rendered off the normal
    // scroll path, or IO unsupported edge cases), never leave it stuck at the
    // placeholder "0". This fires well after the IO would have, so the normal
    // count-up animation still wins when it's actually scrolled into view.
    const fallback = window.setTimeout(() => {
      if (!started.current) {
        started.current = true;
        io.disconnect();
        setDisplay(value);
      }
    }, 3500);

    return () => {
      io.disconnect();
      window.clearTimeout(fallback);
    };
  }, [value]);

  return (
    <div className="num" ref={ref}>
      {display}
    </div>
  );
}
