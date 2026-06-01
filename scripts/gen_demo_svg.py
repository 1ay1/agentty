#!/usr/bin/env python3
"""Generate an animated terminal SVG for the README that mirrors the
agentty.org homepage TUI animation: the user turn lands, the assistant
header appears, tool events stream into the Actions panel one at a time
(spinner -> done check), then the assistant prose types out and the
status bar settles. Pure CSS keyframes inside the SVG -> GitHub renders
it inline via <img>. No JS, no recording.

Run:  python3 scripts/gen_demo_svg.py > assets/demo.svg

Colors / glyphs mirror src/runtime/view/* + maya widgets (palette.hpp,
agent_timeline.hpp). small_caps = uppercase + space-separated.
"""

import sys

# ── palette (xterm-ish mapping of agentty's named ANSI colors) ──
C = {
    "bg":     "#07080b",
    "barbg":  "#0d0f14",
    "border": "#20242e",
    "mag":    "#c586c0",  # role_brand (magenta)
    "bmag":   "#e29be0",  # role_brand_alt (bright_magenta, Opus)
    "cyan":   "#4ec9d8",  # bright_cyan (code_path / inspect / execute)
    "bcyan":  "#6fe0ee",
    "green":  "#98c379",
    "bgreen": "#7ee787",  # status_ok check
    "yellow": "#e5c07b",
    "red":    "#e06c75",
    "white":  "#aab1bd",  # ANSI 7 (mid gray)
    "bwhite": "#f2f4f8",  # ANSI 15 (prose)
    "dim":    "#5c6370",  # bright_black (chrome)
    "title":  "#6a7280",
}

W = 760          # svg width
H = 540          # svg height
PAD_X = 22
LINE = 19        # line height (px)
FS = 13          # font size
CH = 7.82        # approx char advance at FS=13 in the mono font

# Animation schedule (seconds). One full loop.
T_USER   = 0.6      # user message visible
T_ASST   = 1.1      # assistant header
T_PANEL  = 1.4      # actions panel frame
EVENTS_START = 1.7
EV_RUN  = 0.7       # spinner-spin duration per event
EV_GAP  = 0.55      # gap before next event done->next start
PROSE_CPS = 26      # prose chars/sec
HOLD = 1.8           # hold at end before loop
DUR_TOTAL = 0.0     # filled in below


def esc(s):
    return (s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


def tspan(text, color, bold=False, italic=False, dx=None):
    attrs = f'fill="{color}"'
    if bold:
        attrs += ' font-weight="700"'
    if italic:
        attrs += ' font-style="italic"'
    if dx is not None:
        attrs += f' dx="{dx}"'
    return f'<tspan {attrs}>{esc(text)}</tspan>'


# ── content: tool events (mirrors homepage EVENTS) ──
EVENTS = [
    dict(name="Read",  detail="src/auth/handler.cpp  ·  214 lines",
         cat="cyan", elapsed="142ms", el="dim", glyph="╭─"),
    dict(name="Grep",  detail="TokenCache  ·  3 matches",
         cat="cyan", elapsed=" 89ms", el="dim", glyph="├─"),
    dict(name="Edit",  detail="src/auth/handler.cpp  (+18 −9)",
         cat="mag", elapsed="  6ms", el="green", glyph="├─",
         body=[("dim", "@@ resolve(id) @@"),
               ("red", "- return fetch_remote(id);"),
               ("green", "+ if (auto v = cache.lookup(id)) return *v;")]),
    dict(name="Bash",  detail="cmake --build build -j",
         cat="cyan", elapsed="  3.6s", el="yellow", glyph="╰─",
         body=[("dim", "[100%] Built target agentty")]),
]

PROSE = ("Auth handler now resolves through TokenCache::lookup, "
         "falling back to a network refresh only on a miss. Build is green.")


def build():
    out = []
    rows = []           # list of (appear_time, svg_text_row)
    # compute per-event timing
    ev_done = []        # done time per event
    t = EVENTS_START
    ev_appear = []
    for i, ev in enumerate(EVENTS):
        ev_appear.append(t)
        t += EV_RUN
        ev_done.append(t)
        t += EV_GAP
    panel_settle = ev_done[-1]
    prose_start = panel_settle + 0.3
    prose_end = prose_start + len(PROSE) / PROSE_CPS
    loop = prose_end + HOLD

    def fade(at, dur=0.22):
        # opacity 0 until `at`, fade in, hold, reset at loop end. Returned
        # as a nested <animate> (no xlink:href) so it lives INSIDE the
        # target <text> — the most widely-supported SMIL form.
        a = at / loop
        a2 = (at + dur) / loop
        return (f'<animate attributeName="opacity" '
                f'values="0;0;1;1;0" keyTimes="0;{a:.4f};{a2:.4f};0.999;1" '
                f'dur="{loop:.3f}s" repeatCount="indefinite" '
                f'calcMode="linear"/>')

    anims = []
    y = [40]  # mutable current y

    def add(elem_id, x, spans, appear):
        row = (f'<text id="{elem_id}" x="{x}" y="{y[0]}" opacity="0">'
               + "".join(spans) + fade(appear) + '</text>')
        rows.append(row)
        y[0] += LINE

    # title bar dots + title
    static = []
    static.append(f'<rect width="{W}" height="{H}" rx="12" fill="{C["bg"]}" '
                  f'stroke="{C["border"]}"/>')
    static.append(f'<rect width="{W}" height="34" rx="12" fill="{C["barbg"]}"/>')
    static.append(f'<rect y="22" width="{W}" height="12" fill="{C["barbg"]}"/>')
    static.append(f'<line x1="0" y1="34" x2="{W}" y2="34" stroke="{C["border"]}"/>')
    for i, col in enumerate(["#ff5f56", "#ffbd2e", "#27c93f"]):
        static.append(f'<circle cx="{18+i*18}" cy="17" r="5.5" fill="{col}"/>')
    static.append(f'<text x="{18+3*18+8}" y="21" fill="{C["title"]}" '
                  f'font-size="11.5">agentty — ~/projects/app</text>')

    y[0] = 60
    X = PAD_X
    XI = PAD_X + 14   # indented inside rail

    # user turn
    add("u_head", XI, [tspan("❯ ", C["mag"]), tspan("You", C["mag"], bold=True)], T_USER)
    add("u_msg", XI, [tspan("refactor the auth handler to use the new token cache", C["bwhite"])], T_USER + 0.05)
    y[0] += 8

    # assistant header
    add("a_head", XI, [tspan("✦ ", C["bmag"]), tspan("Opus 4.7", C["bmag"], bold=True),
                       tspan("    12:34  ·  4.2s  ·  turn 3", C["dim"])], T_ASST)
    y[0] += 4

    # panel top
    title = " A C T I O N S  ·  4/4 "
    add("p_top", XI, [tspan("╭─" + title, C["dim"], bold=True),
                      tspan("───────────────────  4.2s ─╮", C["dim"])], T_PANEL)
    # stats
    add("p_stat", XI, [tspan("│ ", C["dim"]),
                       tspan("I N S P E C T", C["cyan"], bold=True), tspan(" 2", C["white"]),
                       tspan("  ·  ", C["dim"]),
                       tspan("M U T A T E", C["mag"], bold=True), tspan(" 1", C["white"]),
                       tspan("  ·  ", C["dim"]),
                       tspan("E X E C U T E", C["cyan"], bold=True), tspan(" 1", C["white"])], T_PANEL + 0.05)
    add("p_blank", XI, [tspan("│", C["dim"])], T_PANEL + 0.05)

    # events
    for i, ev in enumerate(EVENTS):
        cat = C[ev["cat"]]
        eid = f"ev{i}"
        spans = [tspan("│ ", C["dim"]),
                 tspan(ev["glyph"] + " ", cat),
                 tspan("✓ ", C["bgreen"], bold=True),
                 tspan(ev["name"], cat, bold=True),
                 tspan("  " + ev["detail"], cat, italic=True),
                 tspan("   " + ev["elapsed"], C[ev["el"]])]
        add(eid, XI, spans, ev_appear[i])
        for j, (bc, bt) in enumerate(ev.get("body", [])):
            add(f"{eid}b{j}", XI, [tspan("│    │  ", C["dim"]), tspan(bt, C[bc])], ev_done[i])
        if i < len(EVENTS) - 1:
            add(f"{eid}c", XI, [tspan("│    │", C["dim"])], ev_done[i])

    # footer
    add("p_foot", XI, [tspan("│   ", C["dim"]),
                       tspan("✓ D O N E", C["bgreen"], bold=True),
                       tspan("   4 actions   4.2s", C["white"])], panel_settle)
    add("p_bot", XI, [tspan("╰" + "─" * 56 + "╯", C["dim"])], panel_settle + 0.05)
    y[0] += 6

    # prose (typed) — we animate width via a clip rect; simpler: reveal full line at prose_end-ish
    add("a_prose1", XI, [tspan("Auth handler now resolves through ", C["bwhite"]),
                         tspan("TokenCache::lookup", C["bcyan"]),
                         tspan(", falling back to a network", C["bwhite"])], prose_start + 1.2)
    add("a_prose2", XI, [tspan("refresh only on a miss. Build is green.", C["bwhite"])], prose_start + 2.2)

    # status bar (fixed at bottom)
    sb_y = H - 18
    static.append(f'<line x1="{PAD_X}" y1="{sb_y-14}" x2="{W-PAD_X}" y2="{sb_y-14}" '
                  f'stroke="{C["dim"]}" opacity="0.5"/>')
    sb = (f'<text x="{PAD_X}" y="{sb_y}" font-size="12">'
          + tspan("▎", C["cyan"]) + tspan(" refactor auth", C["white"])
          + tspan("   ·   ", C["dim"]) + tspan("▌ ● ", C["dim"])
          + tspan("Ready", C["dim"], bold=True)
          + '</text>')
    sb_r = (f'<text x="{W-PAD_X}" y="{sb_y}" font-size="12" text-anchor="end">'
            + tspan("⚡ 0.0 t/s ", C["yellow"])
            + tspan("▁▁▂▁▃▂▁▁▂▁▂▃▂▁  ·  ", C["dim"])
            + tspan("● Opus 4.7", C["bmag"]) + tspan(" · ", C["dim"])
            + tspan("████", C["green"]) + tspan("░░░░░░ 38%", C["dim"])
            + '</text>')
    static.append(f'<line x1="{PAD_X}" y1="{sb_y+8}" x2="{W-PAD_X}" y2="{sb_y+8}" '
                  f'stroke="{C["dim"]}" opacity="0.5"/>')

    style = (f'<style>text{{font-family:"JetBrains Mono","DejaVu Sans Mono",'
             f'ui-monospace,monospace;font-size:{FS}px;'
             f'white-space:pre;}}</style>')

    svg = []
    svg.append(f'<svg xmlns="http://www.w3.org/2000/svg" '
               f'width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
               f'font-family="monospace">')
    svg.append(style)
    svg.extend(static)
    # rail lines for the turn (magenta user, bright-magenta assistant)
    svg.append(f'<rect x="{PAD_X}" y="48" width="2" height="20" fill="{C["mag"]}"/>')
    svg.append(f'<rect x="{PAD_X}" y="92" width="2" height="{sb_y-130}" fill="{C["bmag"]}"/>')
    svg.extend(rows)
    svg.append(sb)
    svg.append(sb_r)
    svg.append('</svg>')
    return "\n".join(svg)


if __name__ == "__main__":
    sys.stdout.write(build())
