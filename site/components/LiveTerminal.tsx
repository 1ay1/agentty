"use client";

import { useEffect, useRef, useState } from "react";

/**
 * LiveTerminal — the REAL agentty binary, streamed into the browser.
 *
 * Connects to the live backend WebSocket (deploy/live/server.mjs), which
 * spawns agentty in a PTY pointed at a mock Ollama model. We render the raw
 * PTY byte stream with xterm.js (loaded from CDN so the static site needs no
 * bundler changes) and forward keystrokes + resizes back.
 *
 * If the backend is unreachable, `onUnavailable` fires and the caller shows
 * the scripted fallback terminal instead — the demo never looks broken.
 */

declare global {
  interface Window {
    Terminal?: any;
    FitAddon?: any;
  }
}

const XTERM_JS = "https://cdn.jsdelivr.net/npm/@xterm/xterm@5.5.0/lib/xterm.js";
const XTERM_CSS = "https://cdn.jsdelivr.net/npm/@xterm/xterm@5.5.0/css/xterm.css";
const FIT_JS = "https://cdn.jsdelivr.net/npm/@xterm/addon-fit@0.10.0/lib/addon-fit.js";

function loadScript(src: string): Promise<void> {
  return new Promise((resolve, reject) => {
    if (document.querySelector(`script[src="${src}"]`)) return resolve();
    const s = document.createElement("script");
    s.src = src; s.async = true;
    s.onload = () => resolve();
    s.onerror = () => reject(new Error("load failed: " + src));
    document.head.appendChild(s);
  });
}
function loadCss(href: string) {
  if (document.querySelector(`link[href="${href}"]`)) return;
  const l = document.createElement("link");
  l.rel = "stylesheet"; l.href = href;
  document.head.appendChild(l);
}

function wsUrl(): string {
  const proto = location.protocol === "https:" ? "wss:" : "ws:";
  // same-origin path proxied by nginx to the live backend
  return `${proto}//${location.host}/live/ws`;
}

export function LiveTerminal({ onUnavailable }: { onUnavailable: () => void }) {
  const hostRef = useRef<HTMLDivElement>(null);
  const [status, setStatus] = useState<"connecting" | "live" | "ended">("connecting");
  const [endMsg, setEndMsg] = useState("");

  useEffect(() => {
    let disposed = false;
    let term: any, fit: any, ws: WebSocket | null = null, ro: ResizeObserver | null = null;

    (async () => {
      try {
        loadCss(XTERM_CSS);
        await loadScript(XTERM_JS);
        await loadScript(FIT_JS);
      } catch {
        if (!disposed) onUnavailable();
        return;
      }
      if (disposed || !hostRef.current || !window.Terminal) { onUnavailable(); return; }

      term = new window.Terminal({
        cursorBlink: true,
        fontFamily: "var(--font-mono), 'JetBrains Mono', ui-monospace, monospace",
        fontSize: 13.5,
        lineHeight: 1.15,
        allowProposedApi: true,
        theme: {
          background: "#08060d", foreground: "#f2ecfb", cursor: "#22d3ee",
          selectionBackground: "rgba(217, 70, 239,0.3)",
          black: "#010409", red: "#e06c75", green: "#98c379", yellow: "#e5c07b",
          blue: "#61afef", magenta: "#c586c0", cyan: "#56b6c2", white: "#d7dae0",
          brightBlack: "#656d76", brightRed: "#e06c75", brightGreen: "#7ee787",
          brightYellow: "#f0d98c", brightBlue: "#61afef", brightMagenta: "#d97cd9",
          brightCyan: "#56d4e0", brightWhite: "#e6edf3",
        },
      });
      const FitAddon = window.FitAddon?.FitAddon || window.FitAddon;
      fit = new FitAddon();
      term.loadAddon(fit);
      term.open(hostRef.current);
      try { fit.fit(); } catch {}

      // connect
      let connected = false;
      const url = wsUrl();
      try { ws = new WebSocket(url); } catch { onUnavailable(); return; }

      const failTimer = setTimeout(() => { if (!connected) { try { ws?.close(); } catch {}; onUnavailable(); } }, 4000);

      ws.binaryType = "arraybuffer";
      ws.onopen = () => {
        connected = true; clearTimeout(failTimer);
        if (disposed) { try { ws?.close(); } catch {}; return; }
        setStatus("live");
        sendResize();
        term.focus();
      };
      ws.onmessage = (ev) => {
        const data = typeof ev.data === "string" ? ev.data : new Uint8Array(ev.data);
        // control frames arrive as JSON strings prefixed by the server
        if (typeof data === "string" && data.startsWith("{") && data.includes('"t"')) {
          try {
            const m = JSON.parse(data);
            if (m.t === "end") { setStatus("ended"); setEndMsg(m.m || "session ended"); return; }
            if (m.t === "err") { setStatus("ended"); setEndMsg(m.m || "error"); return; }
          } catch {}
        }
        term.write(data);
      };
      ws.onerror = () => { if (!connected) { clearTimeout(failTimer); onUnavailable(); } };
      ws.onclose = () => { if (!connected) { clearTimeout(failTimer); onUnavailable(); } else if (status !== "ended") { setStatus("ended"); setEndMsg("disconnected"); } };

      term.onData((d: string) => { if (ws && ws.readyState === WebSocket.OPEN) ws.send(d); });

      function sendResize() {
        try {
          fit.fit();
          const dims = { t: "resize", cols: term.cols, rows: term.rows };
          if (ws && ws.readyState === WebSocket.OPEN) ws.send("\x00json:" + JSON.stringify(dims));
        } catch {}
      }
      ro = new ResizeObserver(() => sendResize());
      if (hostRef.current) ro.observe(hostRef.current);
      window.addEventListener("resize", sendResize);
    })();

    return () => {
      disposed = true;
      try { ro?.disconnect(); } catch {}
      try { ws?.close(); } catch {}
      try { term?.dispose(); } catch {}
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  return (
    <div className="live-root">
      <div className="live-topbar">
        <div className="live-dots"><span /><span /><span /></div>
        <div className="live-title">
          agentty — live demo
          <span className={`live-badge ${status}`}>
            {status === "connecting" ? "connecting…" : status === "live" ? "● LIVE · mock model" : "session ended"}
          </span>
        </div>
        <a className="live-exit" href="/">← back to site</a>
      </div>
      <div className="live-term" ref={hostRef} />
      {status === "ended" && (
        <div className="live-ended">
          <p>{endMsg || "The demo session ended."}</p>
          <button onClick={() => location.reload()}>↻ Start a new session</button>
          <p className="live-fine">This ran the real agentty binary against a mock model. Install it for the real thing:</p>
          <code>curl -fsSL https://agentty.org/install.sh | sh</code>
        </div>
      )}
      <div className="live-hint">
        Real agentty TUI · mock model · sandboxed throwaway workspace ·{" "}
        <kbd>^C</kbd> quit · <kbd>^K</kbd> palette · type to chat
      </div>
    </div>
  );
}
