"use client";

import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import "./dev-terminal.css";

/**
 * dev.agentty.org — a genuinely INTERACTIVE agentty terminal in the browser.
 *
 * You type real prompts. The composer, slash-command palette (`/`), the
 * agent timeline (tool calls streaming Pending → Running ⠋ → Done ✓), the
 * live token/sec status bar and the `n/total` title all behave the way the
 * real TUI does. Slash commands (`/help`, `/model`, `/clear`, `/new`, …) are
 * lifted from include/agentty/runtime/command_palette.hpp. Free-text prompts
 * drive a scripted-but-live session so the page feels like a real agent
 * without a backend.
 */

const SPINNER = ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"];

const BANNER = `   ▄▄▄   ▄▄ ▄▄▄▄▄ ▄▄  ▄▄ ▄▄▄▄▄ ▄▄▄▄▄ ▄▄  ▄▄
  ▄▀ ▀▄ ▄▀ ▀▄ █   ██▄▄██   █     █    ▀▄▄▀
  █▀▀▀█ █   █ █   ██▀▀██   █     █     ██
  ▀   ▀ ▀▄▄▀▀ ▀▄▄▀▀▀  ▀▀   ▀     ▀     ▀▀`;

// ── slash commands, lifted verbatim from command_palette.hpp ──
type Cat = "inspect" | "mutate" | "execute";
interface Cmd {
  name: string;
  label: string;
  desc: string;
  run: (t: Terminal) => void;
}

// ── one scrollback line ──
type Chunk = { t: string; cls?: string };
interface Line {
  id: number;
  kind: "raw" | "user" | "asst-head" | "prose" | "panel" | "blank" | "banner" | "sys";
  chunks?: Chunk[];
  text?: string;
  // panel-specific live state
  panel?: PanelState;
}

interface ToolEvent {
  name: string;
  detail: string;
  cat: Cat;
  runMs: number;
  body?: Chunk[];
  elapsed: string;
  elapsedClass: string;
}
interface PanelState {
  events: ToolEvent[];
  running: number; // index currently spinning (-1 none)
  done: number; // last finished index
  summary?: string;
  finished: boolean;
}

const CAT_CLASS: Record<Cat, string> = {
  inspect: "cyan",
  mutate: "byellow",
  execute: "bmag",
};

// ── a live agent scenario keyed off the user's prompt ──
interface Scenario {
  events: ToolEvent[];
  prose: Chunk[];
}

let uid = 1;
const nextId = () => uid++;

// public handle the slash commands + input use to drive the terminal
interface Terminal {
  print: (line: Omit<Line, "id">) => void;
  printChunks: (kind: Line["kind"], chunks: Chunk[]) => void;
  clear: () => void;
  setModel: (m: string) => void;
  setProfile: (p: string) => void;
  newThread: () => void;
  model: string;
}

/* ---------- scenario authoring ---------- */

function e(name: string, detail: string, cat: Cat, runMs: number, elapsed: string, ecls: string, body?: Chunk[]): ToolEvent {
  return { name, detail, cat, runMs, elapsed, elapsedClass: ecls, body };
}

function pickScenario(prompt: string): Scenario {
  const p = prompt.toLowerCase();

  if (/test|spec|jest|pytest/.test(p)) {
    return {
      events: [
        e("grep", "\\bTODO\\b · **/*.ts", "inspect", 420, "0.4s", "green"),
        e("read", "src/token_cache.ts:1-80", "inspect", 380, "0.4s", "green"),
        e("write", "src/token_cache.test.ts (+64)", "mutate", 620, "0.6s", "yellow"),
        e("bash", "npm test -- token_cache", "execute", 1650, "1.7s", "red", [
          { t: "PASS  src/token_cache.test.ts", cls: "bgreen" },
          { t: "Tests: 7 passed, 7 total", cls: "green" },
        ]),
      ],
      prose: [
        { t: "Added " },
        { t: "src/token_cache.test.ts", cls: "bcyan" },
        { t: " covering hit/miss, TTL expiry, and eviction under pressure. All " },
        { t: "7 tests pass", cls: "bgreen" },
        { t: " — the LRU eviction had an off-by-one on the cold path, now fixed." },
      ],
    };
  }

  if (/bug|fix|crash|error|null|segfault|leak/.test(p)) {
    return {
      events: [
        e("grep", "panic|unwrap · src/**", "inspect", 460, "0.5s", "green"),
        e("read", "src/session/stream.rs:120-180", "inspect", 400, "0.4s", "green"),
        e("edit", "src/session/stream.rs (−3 +5)", "mutate", 700, "0.7s", "yellow", [
          { t: "- let tok = buf.next().unwrap();", cls: "red" },
          { t: "+ let Some(tok) = buf.next() else { break };", cls: "green" },
        ]),
        e("bash", "cargo build --release", "execute", 2100, "2.1s", "red", [
          { t: "   Compiling agentty v0.9.1", cls: "dim" },
          { t: "    Finished release [optimized]", cls: "green" },
        ]),
      ],
      prose: [
        { t: "Root cause: the stream reader called " },
        { t: "unwrap()", cls: "bcyan" },
        { t: " on an empty buffer at EOF, panicking mid-turn. Replaced it with a " },
        { t: "let-else", cls: "bcyan" },
        { t: " that breaks cleanly. Release build is green." },
      ],
    };
  }

  if (/perf|fast|slow|optimi|speed|benchmark/.test(p)) {
    return {
      events: [
        e("read", "src/render/diff.cpp:1-140", "inspect", 380, "0.4s", "green"),
        e("grep", "std::string · render/*.cpp", "inspect", 340, "0.3s", "green"),
        e("edit", "src/render/diff.cpp (−8 +6)", "mutate", 640, "0.6s", "yellow", [
          { t: "reuse a thread-local scratch buffer instead of per-cell alloc", cls: "dim" },
        ]),
        e("bash", "./bench --filter render", "execute", 1900, "1.9s", "red", [
          { t: "render/full   1.42ms → 0.61ms   (2.3× faster)", cls: "bgreen" },
        ]),
      ],
      prose: [
        { t: "The hot path allocated a fresh " },
        { t: "std::string", cls: "bcyan" },
        { t: " per cell. Hoisting a thread-local scratch buffer cut full-frame render " },
        { t: "2.3×", cls: "bgreen" },
        { t: " (1.42ms → 0.61ms)." },
      ],
    };
  }

  if (/explain|how|what|why|understand|architecture/.test(p)) {
    return {
      events: [
        e("repo_map", "focus: retrieval pipeline", "inspect", 520, "0.5s", "green"),
        e("read", "src/rag/advanced.cpp:1-90", "inspect", 440, "0.4s", "green"),
        e("read", "include/agentty/rag/graph.hpp", "inspect", 360, "0.4s", "green"),
      ],
      prose: [
        { t: "The retrieval pipeline is GraphRAG: a memoised doc graph (markdown links + tf-idf entity co-occurrence) feeds " },
        { t: "PageRank", cls: "bcyan" },
        { t: " and label-propagation communities. Candidates are tiered outbound < backlink < entity-neighbour < community-hub, then reranked with overlapping-window dedup before hitting the context." },
      ],
    };
  }

  // default: a small feature build
  return {
    events: [
      e("repo_map", "focus: " + prompt.slice(0, 24).trim(), "inspect", 480, "0.5s", "green"),
      e("read", "src/main.cpp:1-60", "inspect", 360, "0.4s", "green"),
      e("edit", "src/main.cpp (−1 +12)", "mutate", 680, "0.7s", "yellow"),
      e("bash", "cmake --build build -j", "execute", 2200, "2.2s", "red", [
        { t: "[100%] Built target agentty", cls: "green" },
      ]),
    ],
    prose: [
      { t: "Done — wired that up in " },
      { t: "src/main.cpp", cls: "bcyan" },
      { t: " and the build is green. Want me to add a test or open a review of the diff?" },
    ],
  };
}

/* ---------- component ---------- */

export default function DevTerminal() {
  const [lines, setLines] = useState<Line[]>([]);
  const [input, setInput] = useState("");
  const [focused, setFocused] = useState(true);
  const [model, setModel] = useState("Opus 4.5");
  const [profile, setProfile] = useState("Write");
  const [busy, setBusy] = useState(false);
  const [frame, setFrame] = useState(0);
  const [tps, setTps] = useState(0);
  const [ctxPct, setCtxPct] = useState(6);
  const [turn, setTurn] = useState(0);
  const [paletteIdx, setPaletteIdx] = useState(0);
  const [histIdx, setHistIdx] = useState(-1);

  const scrollRef = useRef<HTMLDivElement>(null);
  const inputRef = useRef<HTMLInputElement>(null);
  const timers = useRef<number[]>([]);
  const history = useRef<string[]>([]);
  const linesRef = useRef<Line[]>([]);
  linesRef.current = lines;

  const push = (ms: number, fn: () => void) => timers.current.push(window.setTimeout(fn, ms));

  const appendLines = useCallback((add: Omit<Line, "id">[]) => {
    setLines((prev) => [...prev, ...add.map((l) => ({ ...l, id: nextId() }))]);
  }, []);

  // ── boot banner ──
  useEffect(() => {
    appendLines([
      { kind: "banner", text: BANNER },
      { kind: "blank" },
      {
        kind: "sys",
        chunks: [
          { t: "  agentty ", cls: "bmag b" },
          { t: "v0.9.1", cls: "dim" },
          { t: "   ·   ", cls: "dim" },
          { t: "a blazing-fast coding agent in your terminal", cls: "white" },
        ],
      },
      {
        kind: "sys",
        chunks: [
          { t: "  Type a prompt and press ", cls: "dim" },
          { t: "Enter", cls: "cyan" },
          { t: ". Try ", cls: "dim" },
          { t: "/help", cls: "bmag" },
          { t: " for commands, or ", cls: "dim" },
          { t: "fix the null-deref in stream.rs", cls: "white i" },
          { t: ".", cls: "dim" },
        ],
      },
      { kind: "blank" },
    ]);
    // focus input
    const t = window.setTimeout(() => inputRef.current?.focus(), 200);
    return () => window.clearTimeout(t);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // ── spinner + live meters while busy ──
  useEffect(() => {
    if (!busy) {
      setTps(0);
      return;
    }
    const iv = window.setInterval(() => setFrame((f) => (f + 1) % SPINNER.length), 80);
    const tv = window.setInterval(() => setTps(60 + Math.random() * 40), 130);
    return () => {
      window.clearInterval(iv);
      window.clearInterval(tv);
    };
  }, [busy]);

  // ── autoscroll ──
  useEffect(() => {
    const el = scrollRef.current;
    if (el) el.scrollTop = el.scrollHeight;
  }, [lines, busy, frame]);

  useEffect(() => () => timers.current.forEach(clearTimeout), []);

  // ── terminal handle for slash commands ──
  const term: Terminal = useMemo(
    () => ({
      model,
      print: (l) => appendLines([l]),
      printChunks: (kind, chunks) => appendLines([{ kind, chunks }]),
      clear: () => setLines([]),
      setModel: (m) => setModel(m),
      setProfile: (p) => setProfile(p),
      newThread: () => {
        setLines([]);
        setTurn(0);
        setCtxPct(6);
      },
    }),
    [model, appendLines],
  );

  // ── slash command table (from command_palette.hpp) ──
  const COMMANDS: Cmd[] = useMemo(
    () => [
      {
        name: "/help",
        label: "Help",
        desc: "List commands and keybindings",
        run: (t) => {
          const rows: [string, string][] = [
            ["/model", "Switch the active model"],
            ["/provider", "Choose the LLM backend"],
            ["/profile", "Cycle Write → Ask → Minimal"],
            ["/new", "Start a fresh thread"],
            ["/threads", "Browse saved conversations"],
            ["/review", "Open the diff review pane"],
            ["/compact", "Replace history with a summary"],
            ["/rewind", "Restore files + chat to a checkpoint"],
            ["/plan", "View task progress"],
            ["/clear", "Clear the scrollback"],
            ["/login", "Sign in via OAuth or API key"],
            ["/quit", "Exit agentty"],
          ];
          t.printChunks("sys", [{ t: "  Commands", cls: "bmag b" }]);
          rows.forEach(([c, d]) =>
            t.printChunks("raw", [
              { t: "  " + c.padEnd(12), cls: "bmag" },
              { t: d, cls: "white" },
            ]),
          );
          t.print({ kind: "blank" });
        },
      },
      {
        name: "/model",
        label: "Open model picker",
        desc: "Switch the active model",
        run: (t) => {
          const models = ["Opus 4.5", "Sonnet 4.5", "GPT-5.1", "Llama 3.3 70B", "Kimi K2"];
          const cur = models.indexOf(t.model);
          const next = models[(cur + 1) % models.length];
          t.setModel(next);
          t.printChunks("sys", [
            { t: "  ● ", cls: "bmag" },
            { t: "Model → ", cls: "white" },
            { t: next, cls: "bmag b" },
          ]);
          t.print({ kind: "blank" });
        },
      },
      {
        name: "/provider",
        label: "Switch provider",
        desc: "Choose the LLM backend (Anthropic, OpenAI, …)",
        run: (t) => {
          t.printChunks("sys", [
            { t: "  Providers  ", cls: "bmag b" },
            { t: "Anthropic · OpenAI · Groq · OpenRouter · Together · Cerebras · Ollama", cls: "white" },
          ]);
          t.print({ kind: "blank" });
        },
      },
      {
        name: "/profile",
        label: "Cycle profile",
        desc: "Write → Ask → Minimal",
        run: (t) => {
          setProfile((p) => {
            const order = ["Write", "Ask", "Minimal"];
            const n = order[(order.indexOf(p) + 1) % order.length];
            t.printChunks("sys", [
              { t: "  ⚙ ", cls: "cyan" },
              { t: "Profile → ", cls: "white" },
              { t: n, cls: "bcyan b" },
            ]);
            t.print({ kind: "blank" });
            return n;
          });
        },
      },
      {
        name: "/new",
        label: "New thread",
        desc: "Start a fresh conversation",
        run: (t) => {
          t.newThread();
          t.printChunks("sys", [{ t: "  ✦ new thread", cls: "bmag" }]);
          t.print({ kind: "blank" });
        },
      },
      {
        name: "/threads",
        label: "Open threads",
        desc: "Browse saved conversations",
        run: (t) => {
          const rows = [
            ["refactor auth", "12 turns · 2h ago"],
            ["rag rerank window", "31 turns · yesterday"],
            ["ship v0.9.1", "8 turns · 3d ago"],
          ];
          t.printChunks("sys", [{ t: "  Threads", cls: "bmag b" }]);
          rows.forEach(([n, m], i) =>
            t.printChunks("raw", [
              { t: i === 0 ? "  ▎ " : "    ", cls: "cyan" },
              { t: n.padEnd(22), cls: i === 0 ? "bwhite" : "white" },
              { t: m, cls: "dim" },
            ]),
          );
          t.print({ kind: "blank" });
        },
      },
      {
        name: "/review",
        label: "Review changes",
        desc: "Open diff review pane",
        run: (t) => {
          t.printChunks("sys", [
            { t: "  ± ", cls: "yellow" },
            { t: "2 files changed", cls: "white" },
            { t: "  +17 ", cls: "green" },
            { t: "−4", cls: "red" },
          ]);
          t.printChunks("raw", [{ t: "    src/session/stream.rs   ", cls: "cyan" }, { t: "+5 −3", cls: "dim" }]);
          t.printChunks("raw", [{ t: "    src/token_cache.ts      ", cls: "cyan" }, { t: "+12 −1", cls: "dim" }]);
          t.print({ kind: "blank" });
        },
      },
      {
        name: "/compact",
        label: "Compact context",
        desc: "Replace history with a structured summary",
        run: (t) => {
          setCtxPct(9);
          t.printChunks("sys", [
            { t: "  ⟳ ", cls: "cyan" },
            { t: "context compacted", cls: "white" },
            { t: "  →  9% used", cls: "dim" },
          ]);
          t.print({ kind: "blank" });
        },
      },
      {
        name: "/rewind",
        label: "Rewind to checkpoint",
        desc: "Restore files + conversation to any earlier turn",
        run: (t) => {
          t.printChunks("sys", [{ t: "  ↺ rewound to turn 1", cls: "cyan" }]);
          t.print({ kind: "blank" });
        },
      },
      {
        name: "/plan",
        label: "Open plan",
        desc: "View task progress",
        run: (t) => {
          const items: [string, string][] = [
            ["✓", "reproduce the crash"],
            ["✓", "patch the EOF unwrap"],
            ["◐", "add a regression test"],
            ["○", "open a review"],
          ];
          t.printChunks("sys", [{ t: "  Plan", cls: "bmag b" }]);
          items.forEach(([g, d]) =>
            t.printChunks("raw", [
              { t: "  " + g + " ", cls: g === "✓" ? "bgreen" : g === "◐" ? "cyan" : "dim" },
              { t: d, cls: g === "○" ? "dim" : "white" },
            ]),
          );
          t.print({ kind: "blank" });
        },
      },
      {
        name: "/clear",
        label: "Clear",
        desc: "Clear the scrollback",
        run: (t) => t.clear(),
      },
      {
        name: "/login",
        label: "Login",
        desc: "Sign in via OAuth or API key",
        run: (t) => {
          t.printChunks("sys", [
            { t: "  ✓ ", cls: "bgreen" },
            { t: "Signed in as ", cls: "white" },
            { t: "Claude Max", cls: "bmag b" },
          ]);
          t.print({ kind: "blank" });
        },
      },
      {
        name: "/quit",
        label: "Quit",
        desc: "Exit agentty",
        run: (t) => {
          t.printChunks("sys", [{ t: "  bye 👋  (it's just a webpage — refresh to restart)", cls: "dim" }]);
          t.print({ kind: "blank" });
        },
      },
    ],
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [],
  );

  const showPalette = input.startsWith("/") && !input.includes(" ");
  const paletteMatches = useMemo(() => {
    if (!showPalette) return [];
    const q = input.slice(1).toLowerCase();
    return COMMANDS.filter((c) => c.name.slice(1).includes(q) || c.label.toLowerCase().includes(q));
  }, [showPalette, input, COMMANDS]);

  useEffect(() => setPaletteIdx(0), [input]);

  // ── run a live agent turn ──
  const runTurn = useCallback(
    (prompt: string) => {
      // Clear any still-pending timers from a prior turn and start the array
      // fresh, so timers.current can't accumulate thousands of stale IDs across
      // a long demo session (each turn schedules one per typed char).
      timers.current.forEach((t) => window.clearTimeout(t));
      timers.current = [];

      const s = pickScenario(prompt);
      const n = turn + 1;
      setTurn(n);
      setBusy(true);

      // user turn + assistant header
      appendLines([
        { kind: "blank" },
        { kind: "user", chunks: [{ t: "▎", cls: "bmag b" }, { t: " " + prompt, cls: "bwhite" }] },
        { kind: "asst-head", text: "agentty" },
      ]);

      // panel line, mutated in place as tools stream
      const panel: PanelState = { events: s.events, running: -1, done: -1, finished: false };
      const panelId = nextId();
      setLines((prev) => [...prev, { id: panelId, kind: "panel", panel: { ...panel } }]);

      const patchPanel = (patch: Partial<PanelState>) =>
        setLines((prev) =>
          prev.map((l) => (l.id === panelId ? { ...l, panel: { ...l.panel!, ...patch } } : l)),
        );

      // schedule tool events
      let at = 300;
      s.events.forEach((ev, i) => {
        push(at, () => patchPanel({ running: i, done: i - 1 }));
        at += ev.runMs;
        push(at, () => patchPanel({ done: i, running: i + 1 < s.events.length ? -1 : -1 }));
      });
      const settle = at + 200;
      push(settle, () =>
        patchPanel({
          running: -1,
          done: s.events.length - 1,
          finished: true,
          summary: `${s.events.length} actions   ${(at / 1000).toFixed(1)}s`,
        }),
      );

      // type the prose out
      const proseId = nextId();
      const full = s.prose;
      const totalChars = full.reduce((a, c) => a + c.t.length, 0);
      const proseStart = settle + 250;
      setCtxPct((c) => Math.min(c + 3, 42));

      for (let k = 1; k <= totalChars; k++) {
        push(proseStart + k * 9, () => {
          // build partial chunks up to k chars
          let rem = k;
          const partial: Chunk[] = [];
          for (const c of full) {
            if (rem <= 0) break;
            if (c.t.length <= rem) {
              partial.push(c);
              rem -= c.t.length;
            } else {
              partial.push({ t: c.t.slice(0, rem), cls: c.cls });
              rem = 0;
            }
          }
          setLines((prev) => {
            const exists = prev.some((l) => l.id === proseId);
            const row: Line = { id: proseId, kind: "prose", chunks: partial };
            if (exists) return prev.map((l) => (l.id === proseId ? row : l));
            return [...prev, { id: nextId(), kind: "blank" }, row];
          });
        });
      }
      const end = proseStart + totalChars * 9;
      push(end + 150, () => {
        appendLines([{ kind: "blank" }]);
        setBusy(false);
        inputRef.current?.focus();
      });
    },
    [turn, appendLines],
  );

  // ── submit ──
  const submit = useCallback(
    (value: string) => {
      const v = value.trim();
      if (!v) return;
      history.current.push(v);
      setHistIdx(-1);
      setInput("");

      if (v.startsWith("/")) {
        const name = v.split(/\s+/)[0];
        const cmd = COMMANDS.find((c) => c.name === name) ?? paletteMatches[0];
        // echo the command as a user turn
        appendLines([
          { kind: "blank" },
          { kind: "user", chunks: [{ t: "▎", cls: "bmag b" }, { t: " " + v, cls: "bwhite" }] },
        ]);
        if (cmd) cmd.run(term);
        else
          appendLines([
            { kind: "sys", chunks: [{ t: "  unknown command: ", cls: "red" }, { t: name, cls: "white" }] },
            { kind: "blank" },
          ]);
        return;
      }

      if (busy) return;
      runTurn(v);
    },
    [COMMANDS, paletteMatches, term, busy, runTurn, appendLines],
  );

  // ── keydown ──
  const onKeyDown = (ev: React.KeyboardEvent<HTMLInputElement>) => {
    if (showPalette && paletteMatches.length) {
      if (ev.key === "ArrowDown") {
        ev.preventDefault();
        setPaletteIdx((i) => (i + 1) % paletteMatches.length);
        return;
      }
      if (ev.key === "ArrowUp") {
        ev.preventDefault();
        setPaletteIdx((i) => (i - 1 + paletteMatches.length) % paletteMatches.length);
        return;
      }
      if (ev.key === "Tab") {
        ev.preventDefault();
        setInput(paletteMatches[paletteIdx].name + " ");
        return;
      }
      if (ev.key === "Enter") {
        ev.preventDefault();
        const chosen = paletteMatches[paletteIdx];
        setInput("");
        appendLines([
          { kind: "blank" },
          { kind: "user", chunks: [{ t: "▎", cls: "bmag b" }, { t: " " + chosen.name, cls: "bwhite" }] },
        ]);
        chosen.run(term);
        return;
      }
    }
    if (ev.key === "Enter") {
      ev.preventDefault();
      submit(input);
      return;
    }
    if (ev.key === "ArrowUp" && !showPalette) {
      ev.preventDefault();
      if (!history.current.length) return;
      const ni = histIdx < 0 ? history.current.length - 1 : Math.max(0, histIdx - 1);
      setHistIdx(ni);
      setInput(history.current[ni]);
      return;
    }
    if (ev.key === "ArrowDown" && !showPalette) {
      ev.preventDefault();
      if (histIdx < 0) return;
      const ni = histIdx + 1;
      if (ni >= history.current.length) {
        setHistIdx(-1);
        setInput("");
      } else {
        setHistIdx(ni);
        setInput(history.current[ni]);
      }
      return;
    }
    if (ev.key === "l" && ev.ctrlKey) {
      ev.preventDefault();
      setLines([]);
    }
  };

  return (
    <div className="devt-root" onClick={() => inputRef.current?.focus()}>
      {/* titlebar */}
      <div className="devt-titlebar">
        <div className="devt-dots">
          <span className="devt-dot r" />
          <span className="devt-dot y" />
          <span className="devt-dot g" />
        </div>
        <div className="devt-title">
          agentty <span className="devt-count">— {turn}/{turn} · {profile}</span>
        </div>
        <div className="devt-title-right">
          <a href="https://agentty.org" target="_blank" rel="noreferrer">
            agentty.org
          </a>
        </div>
      </div>

      {/* scrollback */}
      <div className="devt-scroll" ref={scrollRef}>
        {lines.map((l) => (
          <LineView key={l.id} line={l} frame={frame} />
        ))}
        {busy && (
          <div className="devt-line">
            <span className="bcyan b">{SPINNER[frame]}</span>
            <span className="dim"> thinking…</span>
          </div>
        )}
      </div>

      {/* slash palette */}
      {showPalette && paletteMatches.length > 0 && (
        <div className="devt-palette">
          {paletteMatches.map((c, i) => (
            <div
              key={c.name}
              className={`devt-pitem ${i === paletteIdx ? "sel" : ""}`}
              onMouseEnter={() => setPaletteIdx(i)}
              onMouseDown={(ev) => {
                ev.preventDefault();
                setInput("");
                appendLines([
                  { kind: "blank" },
                  { kind: "user", chunks: [{ t: "▎", cls: "bmag b" }, { t: " " + c.name, cls: "bwhite" }] },
                ]);
                c.run(term);
              }}
            >
              <span className="pcmd">{c.name}</span>
              <span className="pdesc">{c.desc}</span>
            </div>
          ))}
        </div>
      )}

      {/* composer */}
      <div className="devt-composer">
        <div className={`devt-comp-box ${focused ? "focus" : ""}`}>
          <span className="devt-prompt">❯&nbsp;</span>
          <div className="devt-input-wrap">
            <input
              ref={inputRef}
              className="devt-input"
              value={input}
              spellCheck={false}
              autoCapitalize="off"
              autoCorrect="off"
              onChange={(e) => setInput(e.target.value)}
              onKeyDown={onKeyDown}
              onFocus={() => setFocused(true)}
              onBlur={() => setFocused(false)}
              aria-label="agentty prompt"
            />
            <span className="devt-shadow">
              {input.length === 0 ? (
                <span className="devt-placeholder">
                  {busy ? "streaming… (input queued)" : "type a message, or / for commands"}
                </span>
              ) : (
                input
              )}
              <span className={`devt-caret ${focused ? "" : "hidden"}`} />
            </span>
          </div>
        </div>
      </div>

      {/* status bar */}
      <div className={`devt-accent ${busy ? "bcyan" : "dim"}`} aria-hidden />
      <div className="devt-status">
        <div className="devt-status-left">
          <span className="cyan">▎</span>
          <span className="white"> session</span>
          <span className="dim">{"   ·   "}</span>
          {busy ? (
            <>
              <span className="bcyan b">{SPINNER[frame]}</span>
              <span className="bcyan b"> Streaming</span>
            </>
          ) : (
            <>
              <span className="dim">●</span>
              <span className="dim b"> Ready</span>
            </>
          )}
        </div>
        <div className="devt-status-right">
          <span className="yellow">⚡ </span>
          <span className="cyan">{busy ? tps.toFixed(1) : "  0.0"}</span>
          <span className="cyan"> t/s </span>
          <span className={busy ? "cyan" : "dim"}>
            {busy ? "▂▃▅▆▇█▇▆▅▃▄▆▇▅▃▂" : "▁▁▂▁▃▂▁▁▂▁▂▃▂▁▁▁"}
          </span>
          <span className="dim">{"   ·   "}</span>
          <span className="bmag">● </span>
          <span className="bmag">{model}</span>
          <span className="dim">{" · "}</span>
          <span className="green">{"█".repeat(Math.max(1, Math.round(ctxPct / 10)))}</span>
          <span className="dim">{"░".repeat(10 - Math.max(1, Math.round(ctxPct / 10)))} {ctxPct}%</span>
        </div>
      </div>
    </div>
  );
}

/* ---------- line renderers ---------- */

function LineView({ line, frame }: { line: Line; frame: number }) {
  switch (line.kind) {
    case "blank":
      return <div className="devt-blank" />;
    case "banner":
      return <div className="devt-banner">{line.text}</div>;
    case "asst-head":
      return <div className="devt-asst-head">{line.text}</div>;
    case "panel":
      return <PanelView panel={line.panel!} frame={frame} />;
    default:
      return (
        <div className={`devt-line ${line.kind === "prose" ? "devt-prose" : ""}`}>
          {line.chunks?.map((c, i) => (
            <span key={i} className={c.cls}>
              {c.t}
            </span>
          ))}
          {line.kind === "prose" && <span className="term-cursor"> </span>}
        </div>
      );
  }
}

function PanelRow({ children }: { children: React.ReactNode }) {
  return (
    <div className="devt-prow">
      <span className="devt-edge">│</span>
      <span className="devt-pad">{children}</span>
      <span className="devt-edge">│</span>
    </div>
  );
}

function PanelView({ panel, frame }: { panel: PanelState; frame: number }) {
  const { events, running, done, finished, summary } = panel;
  return (
    <div className="devt-panel">
      <div className="devt-top">
        <span className="dim">╭─ </span>
        <span className="cyan b">Actions</span>
        <span className="dim"> </span>
        <span className="devt-fill" />
        <span className="dim">╮</span>
      </div>
      {events.map((ev, i) => {
        const isDone = i <= done;
        const isRunning = i === running;
        const pending = !isDone && !isRunning;
        const c = CAT_CLASS[ev.cat];
        const showBody = isDone && ev.body;
        const isLast = i === events.length - 1;
        return (
          <div key={i}>
            <PanelRow>
              <span> </span>
              {isDone ? (
                <span className="bgreen b">✓</span>
              ) : isRunning ? (
                <span className="bcyan b">{SPINNER[frame]}</span>
              ) : (
                <span className="dim">○</span>
              )}
              <span>{"  "}</span>
              <span className={`${c} ${isDone ? "dim" : ""} b`}>{ev.name}</span>
              <span>{"  "}</span>
              <span className={`${c} ${isDone ? "dim" : ""} i`}>
                {isRunning ? "running…" : pending ? "queued" : ev.detail}
              </span>
              {isDone && <span className={`devt-elapsed ${ev.elapsedClass}`}>{ev.elapsed}</span>}
            </PanelRow>
            {showBody &&
              ev.body!.map((b, j) => (
                <PanelRow key={j}>
                  <span className={`${c} dim`}>{"   │  "}</span>
                  <span className={b.cls}>{b.t}</span>
                </PanelRow>
              ))}
            {!isLast && (
              <PanelRow>
                <span className={i < done ? "dim" : "blue"}>{"   │"}</span>
              </PanelRow>
            )}
          </div>
        );
      })}
      {finished && summary && (
        <>
          <PanelRow>
            <span> </span>
          </PanelRow>
          <PanelRow>
            <span>{"   "}</span>
            <span className="bgreen b">{"✓ "}</span>
            <span className="bgreen b">DONE</span>
            <span className="white">{"   " + summary}</span>
          </PanelRow>
        </>
      )}
      <div className="devt-bot">
        <span className="dim">╰</span>
        <span className="devt-fill" />
        <span className="dim">╯</span>
      </div>
    </div>
  );
}
