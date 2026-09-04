"use client";

import dynamic from "next/dynamic";
import { useState } from "react";
import { LiveTerminal } from "./LiveTerminal";
import "./live-terminal.css";

// Scripted fallback — the polished JS mock. Loaded lazily and only shown if
// the live backend is unreachable, so the demo never looks broken.
const DevTerminal = dynamic(() => import("./DevTerminal"), {
  ssr: false,
  loading: () => (
    <div
      style={{
        position: "fixed", inset: 0, background: "#07080b", color: "#5c6370",
        fontFamily: "var(--font-mono), ui-monospace, monospace",
        display: "flex", alignItems: "center", justifyContent: "center", fontSize: 13,
      }}
    >
      ⠋ booting agentty…
    </div>
  ),
});

/**
 * Prefer the REAL agentty binary streamed over WebSocket (LiveTerminal). If
 * that backend can't be reached — capacity, service down, local dev without
 * the live server — fall back to the scripted DevTerminal.
 */
export function DevTerminalMount() {
  const [live, setLive] = useState(true);
  return (
    <div className="devt-root">
      {live ? <LiveTerminal onUnavailable={() => setLive(false)} /> : <DevTerminal />}
    </div>
  );
}
