"use client";

import { useEffect, useRef } from "react";
import { createRoot } from "react-dom/client";
import { ReleaseTable } from "./ReleaseTable";

// Renders pre-built docs HTML and progressively enhances it on the client:
//  1. every `<pre class="code">` gets a floating copy button (matching the
//     hand-written <Code> component's UX);
//  2. any `<div data-directive="release-table">` sentinel is hydrated into the
//     live per-platform download table.
// The server output is complete, styled HTML — enhancement is additive, so the
// page is fully readable before/without JS.
export function DocBody({ html }: { html: string }) {
  const ref = useRef<HTMLDivElement>(null);

  useEffect(() => {
    const root = ref.current;
    if (!root) return;
    const cleanups: (() => void)[] = [];

    // 1. copy buttons on code blocks
    root.querySelectorAll<HTMLPreElement>("pre.code").forEach((pre) => {
      if (pre.querySelector(".codeblock-copy")) return;
      const code = pre.querySelector("code");
      const btn = document.createElement("button");
      btn.className = "codeblock-copy floating";
      btn.setAttribute("aria-label", "Copy code");
      btn.textContent = "⧉";
      const onClick = () => {
        navigator.clipboard?.writeText(code?.textContent ?? "");
        btn.textContent = "✓";
        setTimeout(() => (btn.textContent = "⧉"), 1400);
      };
      btn.addEventListener("click", onClick);
      pre.appendChild(btn);
      cleanups.push(() => btn.removeEventListener("click", onClick));
    });

    // 2. directive sentinels → live components
    root.querySelectorAll<HTMLDivElement>('div[data-directive]').forEach((el) => {
      if (el.dataset.hydrated) return;
      el.dataset.hydrated = "1";
      const kind = el.getAttribute("data-directive");
      if (kind === "release-table") {
        const r = createRoot(el);
        r.render(<ReleaseTable />);
        cleanups.push(() => queueMicrotask(() => r.unmount()));
      }
    });

    return () => cleanups.forEach((c) => c());
  }, [html]);

  return <div ref={ref} dangerouslySetInnerHTML={{ __html: html }} />;
}
