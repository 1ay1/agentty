"use client";

import "./agentty-logo.css";

/**
 * agentty's welcome-screen wordmark: ">AGENTTY" drawn in a 6×7 pixel-art
 * bitmap font, rendered through half-block cells (▀ ▄ █) in bright-magenta.
 *
 * Animation is driven entirely by CSS (composited transform + color), not by
 * a per-frame React re-render. The static glyph grid is built ONCE at module
 * scope and rendered once; each letter is its own column that CSS animates:
 *   • drop-in cascade — per-letter `animation-delay` (100ms stagger)
 *   • sine bob        — infinite per-letter transform, phase-shifted L→R
 *   • heartbeat       — color flash on the whole mark
 * This keeps the main thread idle (no rAF, no setState) → no long tasks.
 */

const FONT_W = 6;
const FONT_H = 7;
const SPACER = 1;
const TEXT = ">AGENTTY";

// 6×7 pixel-art font, '#' = lit. Transcribed exactly from the source.
const GLYPHS: Record<string, string[]> = {
  ">": [
    "      ",
    "#  #  ",
    "## ## ",
    " ## ##",
    "## ## ",
    "#  #  ",
    "      ",
  ],
  A: [
    "  ##  ",
    " #  # ",
    "#    #",
    "######",
    "#    #",
    "#    #",
    "#    #",
  ],
  G: [
    " #### ",
    "#    #",
    "#     ",
    "#  ###",
    "#    #",
    "#    #",
    " #### ",
  ],
  E: [
    "######",
    "#     ",
    "#     ",
    "##### ",
    "#     ",
    "#     ",
    "######",
  ],
  N: [
    "#    #",
    "##   #",
    "# #  #",
    "#  # #",
    "#   ##",
    "#    #",
    "#    #",
  ],
  T: [
    "######",
    "  ##  ",
    "  ##  ",
    "  ##  ",
    "  ##  ",
    "  ##  ",
    "  ##  ",
  ],
  Y: [
    "#    #",
    "#    #",
    " #  # ",
    "  ##  ",
    "  ##  ",
    "  ##  ",
    "  ##  ",
  ],
};

const PAD_TOP = 1;
const PAD_BOTTOM = 2;
const PH = FONT_H + PAD_TOP + PAD_BOTTOM;
const CH = Math.ceil(PH / 2); // cell rows (2 px per cell vertically)

type CellKind = " " | "▀" | "▄" | "█";

// Build each letter's settled half-block cell grid ONCE at module load.
// Result: per letter → CH rows × FONT_W cells.
function buildLetter(ch: string): CellKind[][] {
  const g = GLYPHS[ch];
  const rows2 = FONT_H + PAD_TOP + PAD_BOTTOM; // pixel rows
  const lit: number[] = new Array(rows2 * FONT_W).fill(0);
  if (g) {
    for (let row = 0; row < FONT_H; row++) {
      for (let col = 0; col < FONT_W; col++) {
        if (g[row][col] === "#") lit[(PAD_TOP + row) * FONT_W + col] = 1;
      }
    }
  }
  const cells: CellKind[][] = [];
  for (let cy = 0; cy < CH; cy++) {
    const line: CellKind[] = [];
    for (let x = 0; x < FONT_W; x++) {
      const top = lit[cy * 2 * FONT_W + x] === 1;
      const bot = lit[(cy * 2 + 1) * FONT_W + x] === 1;
      line.push(top && bot ? "█" : top ? "▀" : bot ? "▄" : " ");
    }
    cells.push(line);
  }
  return cells;
}

const LETTERS = TEXT.split("").map((ch) => buildLetter(ch));

export function AgenttyLogo() {
  return (
    <div className="agentty-logo lg" role="img" aria-label="agentty">
      {LETTERS.map((cells, li) => (
        <span
          className="logo-letter"
          key={li}
          style={
            {
              "--li": li,
              // Cascade drop: 180ms stagger per letter (TUI kStaggerMs 100ms,
              // slowed 1.8×). Runs once.
              "--delay": `${li * 180}ms`,
              // Bob phase: in the TUI each glyph starts its sine bob when its
              // drop ends (drop_end = li*180 + 900), and the L→R traveling wave
              // comes from the 0.7 rad/letter phase — as a time offset over the
              // 4000ms loop that is li * 0.7/(2π) * 4000 ≈ li*446ms EARLIER. Net
              // animation-delay = (li*180 + 900) - li*446 so the bob neither
              // fights the drop nor loses the wave.
              "--bob-delay": `${li * 180 + 900 - li * 446}ms`,
            } as React.CSSProperties
          }
        >
          {cells.map((line, cy) => (
            <span className="logo-row" key={cy}>
              {line.map((c, x) => (
                <span key={x} className={c === " " ? "px off" : "px on"}>
                  {c === " " ? "\u00a0" : c}
                </span>
              ))}
            </span>
          ))}
        </span>
      ))}
    </div>
  );
}
