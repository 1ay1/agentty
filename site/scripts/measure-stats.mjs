#!/usr/bin/env node
// Measure the REAL agentty binary and emit lib/stats.generated.ts so the site
// can never drift from the shipped binary. Run automatically by deploy.sh
// (and safe to run by hand: `node scripts/measure-stats.mjs`).
//
// What it captures:
//   • exact binary size on disk (bytes → MB, 1 decimal)
//   • whether it is statically linked
//   • version string from `--version`
//   • median cold-start for `--version` and `--help` (ms)
//
// If the binary can't be found or run, it leaves any existing generated file
// untouched and exits 0 — a deploy must never fail just because the local box
// doesn't have the binary installed.

import { execFileSync, spawnSync } from "node:child_process";
import { statSync, existsSync, writeFileSync, readFileSync, chmodSync } from "node:fs";
import { homedir, tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));
const OUT = join(__dirname, "..", "lib", "stats.generated.ts");

// ── locate the binary ────────────────────────────────────────────────────
async function findBinary() {
  // A real agentty binary is ~18MB; anything under this is a wrapper script or
  // stub that would produce garbage stats (0MB / v0.1.0). Reject those.
  const isReal = (p) => {
    try { return existsSync(p) && statSync(p).size > 1_000_000; } catch { return false; }
  };
  // explicit override wins (still sanity-checked)
  if (process.env.AGENTTY_BIN && isReal(process.env.AGENTTY_BIN)) {
    return process.env.AGENTTY_BIN;
  }
  // PATH lookup (avoid shell:true; scan PATH entries directly)
  for (const dir of (process.env.PATH || "").split(":")) {
    if (!dir) continue;
    const p = join(dir, "agentty");
    if (isReal(p)) return p;
  }
  // common install locations
  for (const p of [
    join(homedir(), ".local", "bin", "agentty"),
    join(homedir(), "bin", "agentty"),
    "/usr/local/bin/agentty",
    "/usr/bin/agentty",
  ]) {
    if (isReal(p)) return p;
  }
  // NOTHING real installed → auto-download the latest release binary so the
  // deploy never depends on a hand-installed binary. This is the last piece that
  // makes the stats fully hands-off: the site measures whatever's on Releases.
  return await downloadLatestBinary();
}

// Download the current platform's release binary to a temp path, chmod +x, and
// return it. Linux x86_64 only (the box the site deploys on); returns null on
// any failure so the deploy falls back to committed stats.
async function downloadLatestBinary() {
  try {
    const asset =
      process.platform === "linux" && process.arch === "x64"
        ? "agentty-linux-x86_64"
        : process.platform === "linux" && process.arch === "arm64"
          ? "agentty-linux-aarch64"
          : null;
    if (!asset) return null;
    const url = `https://github.com/1ay1/agentty/releases/latest/download/${asset}`;
    const dest = join(tmpdir(), "agentty-measure-bin");
    console.log(`[measure-stats] no local binary — downloading ${asset} from releases/latest`);
    const res = await fetch(url, { redirect: "follow" });
    if (!res.ok) throw new Error(`download ${res.status}`);
    const buf = Buffer.from(await res.arrayBuffer());
    if (buf.length < 1_000_000) throw new Error(`suspiciously small (${buf.length}b)`);
    writeFileSync(dest, buf);
    chmodSync(dest, 0o755);
    return dest;
  } catch (err) {
    console.warn(`[measure-stats] auto-download failed (${err.message})`);
    return null;
  }
}

// ── helpers ──────────────────────────────────────────────────────────────
function median(nums) {
  const s = [...nums].sort((a, b) => a - b);
  const mid = Math.floor(s.length / 2);
  return s.length % 2 ? s[mid] : (s[mid - 1] + s[mid]) / 2;
}

// median wall-clock (ms) to run `bin args`, over N runs after a warmup
function timeRun(bin, args, runs = 15) {
  // warm the page cache / fs first so we measure steady-state cold start
  for (let i = 0; i < 3; i++) {
    try { execFileSync(bin, args, { stdio: "ignore" }); } catch {}
  }
  const samples = [];
  for (let i = 0; i < runs; i++) {
    const t0 = process.hrtime.bigint();
    try {
      execFileSync(bin, args, { stdio: "ignore" });
    } catch {
      // --help / --version may exit non-zero on some builds; timing still valid
    }
    const t1 = process.hrtime.bigint();
    samples.push(Number(t1 - t0) / 1e6); // ns → ms
  }
  return median(samples);
}

function isStatic(bin) {
  const r = spawnSync("file", [bin], { encoding: "utf8" });
  if (r.status === 0 && /statically linked|not a dynamic executable/i.test(r.stdout)) return true;
  const ldd = spawnSync("ldd", [bin], { encoding: "utf8" });
  if (ldd.status !== 0 || /not a dynamic executable/i.test(ldd.stdout + ldd.stderr)) return true;
  return false;
}

// round a small ms value to a friendly label: 2.4 → "~2 ms", 0.8 → "<1 ms"
function msLabel(ms) {
  if (ms < 1) return "<1 ms";
  return `~${Math.round(ms)} ms`;
}

// ── run ──────────────────────────────────────────────────────────────────
const bin = await findBinary();
if (!bin) {
  console.warn("[measure-stats] agentty binary not found — keeping existing generated stats.");
  process.exit(0);
}

let sizeBytes = 0;
try {
  sizeBytes = statSync(bin).size;
} catch {
  console.warn("[measure-stats] could not stat binary — keeping existing stats.");
  process.exit(0);
}
const sizeMB = (sizeBytes / 1048576).toFixed(1);

let version = "";
try {
  const out = execFileSync(bin, ["--version"], { encoding: "utf8" });
  // `agentty --version` prints e.g. "agentty 0.7.0\nlog: /path/to/agentty.log".
  // Extract the semver token directly — NOT split().pop(), which grabbed the
  // trailing log path from the 2nd line and leaked it into the JSON-LD
  // softwareVersion field.
  const m = out.match(/\b(\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?)\b/);
  version = m ? m[1] : (out.split(/\r?\n/)[0].trim().split(/\s+/).pop() || "");
} catch {}

const verMs = timeRun(bin, ["--version"]);
const helpMs = timeRun(bin, ["--help"]);
const coldMs = Math.min(verMs, helpMs); // headline = the faster of the two
const staticLinked = isStatic(bin);

const stats = {
  binarySizeBytes: sizeBytes,
  binarySizeMB: `${sizeMB} MB`,
  version,
  staticLinked,
  coldStartMs: Number(coldMs.toFixed(2)),
  coldStartLabel: msLabel(coldMs),
  versionMs: Number(verMs.toFixed(2)),
  helpMs: Number(helpMs.toFixed(2)),
  measuredAt: new Date().toISOString(),
  source: bin,
};

const banner = `// AUTO-GENERATED by scripts/measure-stats.mjs — DO NOT EDIT BY HAND.
// Regenerated on every deploy from the real agentty binary, so the site can
// never drift from what actually ships. Last measured: ${stats.measuredAt}.
`;

const body = `export const measuredStats = ${JSON.stringify(stats, null, 2)} as const;

export type MeasuredStats = typeof measuredStats;
`;

writeFileSync(OUT, banner + "\n" + body);

console.log(
  `[measure-stats] ${bin}\n` +
    `  size      : ${stats.binarySizeMB} (${sizeBytes} bytes)\n` +
    `  static    : ${staticLinked}\n` +
    `  version   : ${version || "(unknown)"}\n` +
    `  --version : ${stats.versionMs} ms\n` +
    `  --help    : ${stats.helpMs} ms\n` +
    `  headline  : ${stats.coldStartLabel}\n` +
    `  → wrote ${OUT}`,
);
