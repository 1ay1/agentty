// Stable accessor for the latest GitHub release data. Pages import from HERE.
// lib/release.generated.ts is produced by scripts/fetch-release.mjs on each
// deploy and committed, so the build works offline / on a fresh checkout.

import { latestRelease } from "./release.generated";

const fallback = {
  version: "0.2.8",
  tag: "v0.2.8",
  htmlUrl: "https://github.com/1ay1/agentty/releases/latest",
} as const;

export const release = {
  /** "0.2.8" */
  version: latestRelease?.version || fallback.version,
  /** "v0.2.8" */
  tag: latestRelease?.tag || fallback.tag,
  /** release page on GitHub */
  url: latestRelease?.htmlUrl || fallback.htmlUrl,
  /** ISO published date */
  publishedAt: latestRelease?.publishedAt || "",
  /** size of the headline (Linux x86_64) standalone binary, e.g. "13.2 MB" */
  headlineSizeMB: latestRelease?.headlineSizeMB || null,
  /** direct download for the headline binary */
  headlineUrl: latestRelease?.headlineUrl || null,
  /** total download count across all assets in the latest release */
  downloads: latestRelease?.totalDownloads ?? 0,
  /** per-platform standalone binaries with sizes + checksums */
  platforms: latestRelease?.platforms ?? [],
} as const;

/** "0.2.4" → "v0.2.4" for display */
export const versionLabel = release.tag || `v${release.version}`;
