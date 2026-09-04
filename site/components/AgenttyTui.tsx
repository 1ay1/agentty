"use client";

import { useEffect, useRef, useState } from "react";
import "./agentty-tui.css";

/**
 * A faithful, *animated* replica of the agentty TUI — plays a live session
 * the way the real app renders one: the user turn lands, the assistant
 * header appears, tool events stream into the Actions panel one by one
 * (Pending/Running spinner → Done ✓ with elapsed), the title `n/total`
 * and stats counts tick up live, then the assistant prose types out and
 * the status bar settles from Streaming → Ready. Then it loops.
 *
 * Every glyph / color / layout is lifted from the real source:
 *   turn header  → src/runtime/view/thread/turn/turn.cpp + maya/widget/turn.hpp
 *   Actions panel→ maya/widget/agent_timeline.hpp + agent_timeline.cpp
 *   tool colors  → .../agent_timeline/tool_helpers.cpp
 *   status bar   → maya/widget/status_bar.hpp + status_bar/*
 *   palette      → include/agentty/runtime/view/palette.hpp
 *
 * Spinner frames are maya's exact braille set; status icons ✓ / ⠋ match.
 */

const SPINNER = ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"];

type Cat = "inspect" | "mutate" | "execute";

interface ToolEvent {
  name: string;
  detail: string;
  cat: Cat;
  elapsed: string;
  elapsedClass: string; // duration_color zone
  runMs: number; // how long the spinner spins before settling
  body?: { cls: string; text: string }[];
}

const EVENTS: ToolEvent[] = [
  {
    name: "Read",
    detail: "src/auth/handler.cpp  ·  214 lines",
    cat: "inspect",
    elapsed: "142ms",
    elapsedClass: "dim",
    runMs: 700,
  },
  {
    name: "Grep",
    detail: "TokenCache  ·  3 matches",
    cat: "inspect",
    elapsed: " 89ms",
    elapsedClass: "dim",
    runMs: 600,
  },
  {
    name: "Edit",
    detail: "src/auth/handler.cpp  (+18 −9)",
    cat: "mutate",
    elapsed: "  6ms",
    elapsedClass: "green",
    runMs: 900,
    body: [
      { cls: "dim", text: "@@ resolve(id) @@" },
      { cls: "red", text: "- return fetch_remote(id);" },
      { cls: "green", text: "+ if (auto v = cache.lookup(id)) return *v;" },
    ],
  },
  {
    name: "Bash",
    detail: "cmake --build build -j",
    cat: "execute",
    elapsed: "  3.6s",
    elapsedClass: "yellow",
    runMs: 1400,
    body: [{ cls: "dim", text: "[100%] Built target agentty" }],
  },
];

const PROSE =
  "Auth handler now resolves through TokenCache::lookup, falling back to a network refresh only on a miss. Build is green.";

const catColor: Record<Cat, string> = {
  inspect: "cyan",
  mutate: "mag",
  execute: "cyan",
};

// timeline scheduling — cumulative ms offsets
function schedule() {
  const steps: { at: number; running?: number; done?: number }[] = [];
  let t = 1100; // after user msg + assistant header appear
  EVENTS.forEach((ev, i) => {
    steps.push({ at: t, running: i }); // event i starts (Running)
    t += ev.runMs;
    steps.push({ at: t, done: i }); // event i settles (Done)
    t += 120;
  });
  return { steps, settleAt: t };
}

export function AgenttyTui() {
  // count of events that have STARTED rendering, and how many are DONE
  const [running, setRunning] = useState(-1); // index currently/last running
  const [done, setDone] = useState(-1); // highest done index
  const [frame, setFrame] = useState(0);
  const [proseLen, setProseLen] = useState(0);
  const [phase, setPhase] = useState<"idle" | "stream" | "ready">("idle");
  const [userTyped, setUserTyped] = useState(false);
  const timers = useRef<ReturnType<typeof setTimeout>[]>([]);
  // Animation only runs once the TUI is on-screen AND the browser has gone
  // idle after first paint — so it never competes with hydration / first input
  // and stays paused while scrolled out of view. The initial static frame is
  // always rendered, so the hero looks complete immediately.
  const [started, setStarted] = useState(false);

  // ── interactive 3D tilt: the window leans toward the cursor and a glare
  //    sheen tracks the pointer, like a pane of smoked glass floating off
  //    the page. Pure CSS-var driven + rAF-throttled; no-ops on touch and
  //    when the user prefers reduced motion. ──
  const tiltRef = useRef<HTMLDivElement>(null);
  useEffect(() => {
    const el = tiltRef.current;
    if (!el) return;
    const fine = window.matchMedia("(pointer: fine)").matches;
    const reduce = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
    if (!fine || reduce) return;

    let raf = 0;
    let tx = 0, ty = 0, gx = 50, gy = 0, active = 0;
    const apply = () => {
      raf = 0;
      el.style.setProperty("--rx", `${ty.toFixed(2)}deg`);
      el.style.setProperty("--ry", `${tx.toFixed(2)}deg`);
      el.style.setProperty("--gx", `${gx.toFixed(1)}%`);
      el.style.setProperty("--gy", `${gy.toFixed(1)}%`);
      el.style.setProperty("--glare", active.toFixed(2));
    };
    const queue = () => { if (!raf) raf = requestAnimationFrame(apply); };

    const onMove = (e: PointerEvent) => {
      const r = el.getBoundingClientRect();
      const px = (e.clientX - r.left) / r.width;  // 0..1
      const py = (e.clientY - r.top) / r.height;  // 0..1
      const MAX = 7; // degrees of lean
      tx = (px - 0.5) * 2 * MAX;       // rotateY
      ty = -(py - 0.5) * 2 * MAX;      // rotateX
      gx = px * 100;
      gy = py * 100;
      active = 1;
      queue();
    };
    const onLeave = () => {
      tx = 0; ty = 0; gx = 50; gy = 0; active = 0;
      queue();
    };

    el.addEventListener("pointermove", onMove);
    el.addEventListener("pointerleave", onLeave);
    return () => {
      el.removeEventListener("pointermove", onMove);
      el.removeEventListener("pointerleave", onLeave);
      if (raf) cancelAnimationFrame(raf);
    };
  }, []);

  // arm the animation: visible in viewport + browser idle
  useEffect(() => {
    const el = tiltRef.current;
    if (!el) return;
    let idleHandle: number | null = null;
    const w = window as Window & {
      requestIdleCallback?: (cb: () => void, o?: { timeout: number }) => number;
    };
    const io = new IntersectionObserver(
      ([e]) => {
        if (!e.isIntersecting) return;
        io.disconnect();
        const go = () => setStarted(true);
        if (typeof w.requestIdleCallback === "function") {
          idleHandle = w.requestIdleCallback(go, { timeout: 1800 });
        } else {
          idleHandle = window.setTimeout(go, 900);
        }
      },
      { threshold: 0.25 },
    );
    io.observe(el);
    return () => {
      io.disconnect();
      if (idleHandle != null) window.clearTimeout(idleHandle);
    };
  }, []);

  // master loop — clears + reschedules everything each cycle
  useEffect(() => {
    if (!started) return;
    let cancelled = false;
    const clearAll = () => {
      timers.current.forEach(clearTimeout);
      timers.current = [];
    };

    const run = () => {
      if (cancelled) return;
      // reset
      setRunning(-1);
      setDone(-1);
      setProseLen(0);
      setPhase("idle");
      setUserTyped(false);

      const push = (ms: number, fn: () => void) =>
        timers.current.push(setTimeout(fn, ms));

      push(300, () => setUserTyped(true));
      push(700, () => setPhase("stream"));

      const { steps, settleAt } = schedule();
      steps.forEach((s) => {
        push(s.at, () => {
          if (s.running !== undefined) setRunning(s.running);
          if (s.done !== undefined) setDone(s.done);
        });
      });

      // type the prose out after the panel settles
      const proseStart = settleAt + 250;
      for (let i = 1; i <= PROSE.length; i++) {
        push(proseStart + i * 11, () => setProseLen(i));
      }
      const proseEnd = proseStart + PROSE.length * 11;
      push(proseEnd + 150, () => setPhase("ready"));

      // hold, then loop — but only while the tab is visible
      push(proseEnd + 4200, () => {
        clearAll();
        if (!document.hidden) run();
        else {
          const resume = () => {
            if (!document.hidden) {
              document.removeEventListener("visibilitychange", resume);
              run();
            }
          };
          document.addEventListener("visibilitychange", resume);
        }
      });
    };

    run();
    return () => {
      cancelled = true;
      clearAll();
    };
  }, [started]);

  // spinner ticker — only while anything is in flight
  const anyRunning = running >= 0 && done < EVENTS.length - 1;
  useEffect(() => {
    if (!anyRunning && phase !== "stream") return;
    const id = setInterval(() => setFrame((f) => (f + 1) % 10), 90);
    return () => clearInterval(id);
  }, [anyRunning, phase]);

  const total = EVENTS.length;
  const doneCount = done + 1;
  const allDone = doneCount === total;
  const visibleCount = Math.max(running + 1, doneCount); // events shown so far
  const showPanel = running >= 0;

  // live stats counts (only count categories of events seen so far)
  const seen = EVENTS.slice(0, visibleCount);
  const inspect = seen.filter((e) => e.cat === "inspect").length;
  const mutate = seen.filter((e) => e.cat === "mutate").length;
  const execute = seen.filter((e) => e.cat === "execute").length;

  // prose split: render code-ref span (TokenCache::lookup) in bright_cyan
  const prose = PROSE.slice(0, proseLen);

  return (
    <div className="ttui-stage" ref={tiltRef}>
    <div className="ttui" role="img" aria-label="agentty terminal interface showing a live coding session">
      <span className="ttui-glare" aria-hidden />
      <div className="ttui-bar">
        <span className="ttui-dot r" />
        <span className="ttui-dot y" />
        <span className="ttui-dot g" />
        <span className="ttui-title">agentty — ~/projects/app</span>
      </div>

      <div className="ttui-body">
        <div className="ttui-scroll">
        {/* ── USER TURN ── */}
        <div className="ttui-turn rail-mag">
          <div className="row ttui-head">
            <span className="mag">❯</span>
            <span> </span>
            <span className="mag b">You</span>
            <span className="ttui-meta dim">12:34</span>
          </div>
          <div className="row ttui-blank" />
          <div className="row">
            <span className="bwhite">refactor the auth handler to use the new token cache</span>
            {!userTyped && <span className="term-cursor"> </span>}
          </div>
        </div>

        {/* ── ASSISTANT TURN (appears after user message) ── */}
        {userTyped && (
          <div className="ttui-turn rail-bmag ttui-fade">
            <div className="row ttui-head">
              <span className="bmag">✦</span>
              <span> </span>
              <span className="bmag b">Opus 4.5</span>
              <span className="ttui-meta dim">
                {phase === "ready" ? "12:34  ·  4.2s  ·  turn 3" : "12:34  ·  turn 3"}
              </span>
            </div>
            <div className="row ttui-blank" />

            {/* Actions panel */}
            {showPanel && (
              <div className="ttui-panel ttui-fade">
                <div className="ttui-panel-top">
                  <span className="dim">╭─</span>
                  <span className="dim b ttui-cap">{` A C T I O N S  ·  ${doneCount}/${total} `}</span>
                  <span className="dim ttui-fill" />
                  <span className="dim b">
                    {allDone ? " 4.2s " : ` ${EVENTS[running]?.name ?? ""} `}
                  </span>
                  <span className="dim">─╮</span>
                </div>

                {/* stats row */}
                <div className="ttui-panel-line">
                  <span className="ttui-edge dim">│</span>
                  <span className="ttui-pad">
                    {inspect > 0 && (
                      <>
                        <span className="cyan b">I N S P E C T</span>
                        <span className="white"> {inspect}</span>
                      </>
                    )}
                    {mutate > 0 && (
                      <>
                        {inspect > 0 && <span className="dim">{"  ·  "}</span>}
                        <span className="mag b">M U T A T E</span>
                        <span className="white"> {mutate}</span>
                      </>
                    )}
                    {execute > 0 && (
                      <>
                        {(inspect > 0 || mutate > 0) && <span className="dim">{"  ·  "}</span>}
                        <span className="cyan b">E X E C U T E</span>
                        <span className="white"> {execute}</span>
                      </>
                    )}
                  </span>
                  <span className="ttui-edge dim">│</span>
                </div>
                <div className="ttui-panel-line">
                  <span className="ttui-edge dim">│</span>
                  <span className="ttui-pad" />
                  <span className="ttui-edge dim">│</span>
                </div>

                {/* events */}
                {EVENTS.slice(0, visibleCount).map((ev, i) => {
                  const isDone = i <= done;
                  const isLast = i === total - 1;
                  const glyph = total === 1 ? "──" : i === 0 ? "╭─" : isLast ? "╰─" : "├─";
                  const c = catColor[ev.cat];
                  const showBody = isDone && ev.body;
                  // connector after this event uses NEXT event status
                  const nextDone = i + 1 <= done;
                  return (
                    <div key={i} className="ttui-evgroup ttui-fade">
                      <PanelRow>
                        <span className={`${c} dim`}>{glyph}</span>
                        <span> </span>
                        {isDone ? (
                          <span className="bgreen b">✓</span>
                        ) : (
                          <span className="bcyan b">{SPINNER[frame]}</span>
                        )}
                        <span>{"  "}</span>
                        <span className={`${c} ${isDone ? "dim" : ""} b`}>{ev.name}</span>
                        <span>{"  "}</span>
                        <span className={`${c} ${isDone ? "dim" : ""} i`}>
                          {isDone ? ev.detail : "running…"}
                        </span>
                        {isDone && (
                          <span className={`ttui-elapsed ${ev.elapsedClass}`}>{ev.elapsed}</span>
                        )}
                      </PanelRow>
                      {showBody &&
                        ev.body!.map((b, j) => (
                          <PanelRow key={j}>
                            <span className={`${c} dim`}>{"   │  "}</span>
                            <span className={b.cls}>{b.text}</span>
                          </PanelRow>
                        ))}
                      {!isLast && (
                        <PanelRow>
                          <span className={nextDone ? "dim" : "blue"}>{"   │"}</span>
                        </PanelRow>
                      )}
                    </div>
                  );
                })}

                {/* footer — appears once all events done */}
                {allDone && (
                  <>
                    <PanelRow>
                      <span className="ttui-pad" />
                    </PanelRow>
                    <PanelRow>
                      <span>{"   "}</span>
                      <span className="bgreen b">{"✓ "}</span>
                      <span className="bgreen b">D O N E</span>
                      <span className="white">{"   4 actions   4.2s"}</span>
                    </PanelRow>
                  </>
                )}

                <div className="ttui-panel-bot">
                  <span className="dim">╰</span>
                  <span className="dim ttui-fill-bot" />
                  <span className="dim">╯</span>
                </div>
              </div>
            )}

            {/* assistant prose — types out after panel settles */}
            {proseLen > 0 && (
              <>
                <div className="row ttui-blank" />
                <div className="row ttui-prose">
                  <ProseText text={prose} />
                  {phase !== "ready" && <span className="term-cursor"> </span>}
                </div>
              </>
            )}
          </div>
        )}

        {/* ── COMPOSER ── */}
        </div>
        <div className="ttui-chrome">
        <div className="ttui-composer">
          <div className="ttui-comp-top">
            <span className="dim">╭</span>
            <span className="dim ttui-fill-bot" />
            <span className="dim">╮</span>
          </div>
          <div className="ttui-comp-mid">
            <span className="dim">│ </span>
            <span className="bmag b">❯ </span>
            <span className="dim">type a message…</span>
            <span className="ttui-comp-right" />
            <span className="dim"> │</span>
          </div>
          <div className="ttui-comp-bot">
            <span className="dim">╰</span>
            <span className="dim ttui-fill-bot" />
            <span className="dim">╯</span>
          </div>
        </div>

        {/* ── STATUS BAR ── */}
        <div className={`ttui-accent ${phase === "stream" ? "bcyan" : "dim"}`} aria-hidden />
        <div className="ttui-status">
          <div className="ttui-status-left">
            <span> </span>
            <span className="cyan">▎</span>
            <span className="white"> refactor auth</span>
            <span className="dim">{"   ·   "}</span>
            {phase === "stream" ? (
              <>
                <span className="bcyan b">▌</span>
                <span> </span>
                <span className="bcyan b">{SPINNER[frame]}</span>
                <span className="bcyan b"> {allDone && proseLen > 0 ? "Streaming" : EVENTS[running]?.name ?? "Streaming"}</span>
              </>
            ) : (
              <>
                <span className="dim">▌</span>
                <span> </span>
                <span className="dim">●</span>
                <span className="dim b"> Ready</span>
              </>
            )}
          </div>
          <div className="ttui-status-right">
            <span className="yellow">⚡ </span>
            <span className="cyan">{phase === "stream" ? "78.3" : "  0.0"}</span>
            <span className="cyan"> t/s </span>
            <span className={phase === "stream" ? "cyan" : "dim"}>
              {phase === "stream" ? "▂▃▅▆▇█▇▆▅▃▄▆▇▅▃▂" : "▁▁▂▁▃▂▁▁▂▁▂▃▂▁▁▁"}
            </span>
            <span className="dim">{"   ·   "}</span>
            <span className="bmag">● </span>
            <span className="bmag">Opus 4.5</span>
            <span className="dim">{" · "}</span>
            <span className="green">████</span>
            <span className="dim">░░░░░░ 38%</span>
            <span> </span>
          </div>
        </div>
        <div className={`ttui-accent ${phase === "stream" ? "bcyan" : "dim"}`} aria-hidden />
        </div>
      </div>
    </div>
    </div>
  );
}

/* prose with the TokenCache::lookup identifier highlighted in bright_cyan
   once it's been typed past — mirrors maya markdown inline-code coloring. */
function ProseText({ text }: { text: string }) {
  const ref = "TokenCache::lookup";
  const idx = text.indexOf(ref);
  if (idx === -1) return <span className="bwhite">{text}</span>;
  const end = idx + ref.length;
  const shownRef = text.slice(idx, end);
  return (
    <>
      <span className="bwhite">{text.slice(0, idx)}</span>
      <span className="bcyan">{shownRef}</span>
      <span className="bwhite">{text.slice(end)}</span>
    </>
  );
}

/* A single content row inside the Actions panel: left border, padded
   content, right border — mirrors maya's bordered card with padding(0,1). */
function PanelRow({ children }: { children: React.ReactNode }) {
  return (
    <div className="ttui-panel-line">
      <span className="ttui-edge dim">│</span>
      <span className="ttui-pad">{children}</span>
      <span className="ttui-edge dim">│</span>
    </div>
  );
}
