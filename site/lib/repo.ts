// Stable accessor for the agentty GitHub repo's social stats. Pages import
// from HERE, never from the generated file directly — so a fresh checkout
// (or a rate-limited deploy) still has sane fallbacks and one import surface.
//
// lib/repo.generated.ts is produced by scripts/fetch-repo.mjs on each deploy
// and committed, so the build works offline / on a fresh checkout.

import { repoStats } from "./repo.generated";

// Hardcoded fallbacks — only used if the generated file is missing a field.
// Kept in sync with the last known-good fetch.
const fallback = {
  stars: 19,
  forks: 1,
  openIssues: 0,
  htmlUrl: "https://github.com/1ay1/agentty",
} as const;

export const repo = {
  /** stargazer count, e.g. 19 */
  stars: repoStats?.stars ?? fallback.stars,
  /** fork count */
  forks: repoStats?.forks ?? fallback.forks,
  /** open issues */
  openIssues: repoStats?.openIssues ?? fallback.openIssues,
  /** watchers / subscribers */
  watchers: repoStats?.watchers ?? 0,
  /** repo page on GitHub */
  url: repoStats?.htmlUrl || fallback.htmlUrl,
  /** ISO timestamp of the last push */
  pushedAt: repoStats?.pushedAt ?? "",
  /** ISO timestamp of the fetch */
  fetchedAt: repoStats?.fetchedAt ?? "",
} as const;

/** Compact star label, e.g. "19" or "1.2k". */
export const starLabel =
  repo.stars >= 1000 ? `${(repo.stars / 1000).toFixed(1)}k` : `${repo.stars}`;
