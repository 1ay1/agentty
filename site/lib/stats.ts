// Public, stable accessor for the auto-measured binary stats. Pages import
// from HERE, never from the generated file directly — so if the generated
// file is ever missing or partial, we still have sane fallbacks and a single
// import surface.
//
// lib/stats.generated.ts is produced by scripts/measure-stats.mjs on every
// deploy (see deploy.sh) from the real agentty binary. It is committed so the
// build works on a fresh checkout without the binary present.

import { measuredStats } from "./stats.generated";

// Hardcoded fallbacks — only used if a field is somehow absent from the
// generated file. Kept in sync with the last known-good measurement.
const fallback = {
  binarySizeMB: "13.2 MB",
  version: "0.2.4",
  staticLinked: true,
  coldStartLabel: "~2 ms",
  versionMs: 2.29,
  helpMs: 2.22,
} as const;

export const stats = {
  /** e.g. "13.2 MB" */
  sizeMB: measuredStats?.binarySizeMB ?? fallback.binarySizeMB,
  /** exact bytes, e.g. 9849816 */
  sizeBytes: measuredStats?.binarySizeBytes ?? 0,
  /** e.g. "0.2.4" */
  version: measuredStats?.version || fallback.version,
  /** statically linked? */
  static: measuredStats?.staticLinked ?? fallback.staticLinked,
  /** headline cold-start label, e.g. "~2 ms" */
  coldStart: measuredStats?.coldStartLabel ?? fallback.coldStartLabel,
  /** numeric --version cold start in ms */
  versionMs: measuredStats?.versionMs ?? fallback.versionMs,
  /** numeric --help cold start in ms */
  helpMs: measuredStats?.helpMs ?? fallback.helpMs,
  /** ISO timestamp of the measurement */
  measuredAt: measuredStats?.measuredAt ?? "",
} as const;
