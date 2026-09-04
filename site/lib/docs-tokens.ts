// Build-time token values for the docs {{...}} substitutions (see lib/docs.ts).
// Sourced from the same live release/stats accessors the rest of the site uses,
// so a doc's install commands and size claims always match the deployed binary.

import { release, versionLabel } from "./release";
import { stats } from "./stats";
import { site } from "./site";

export const docsTokens: Record<string, string> = {
  version: release.version,
  versionLabel,
  sizeMB: stats.sizeMB,
  coldStartMs: stats.coldStart,
  installOneLiner: site.installOneLiner,
  installOneLinerWindows: site.installOneLinerWindows,
  github: site.github,
  releasesLatest: site.releasesLatest,
};
