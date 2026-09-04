"use client";

import { release } from "@/lib/release";

// The per-platform download table on /docs/installation. Rendered from the live
// GitHub release data (lib/release.generated.ts), so names, sizes, and SHA-256
// prefixes always match the deployed release. Injected into the installation
// markdown via the `:::release-table` directive.
export function ReleaseTable() {
  const platforms = release.platforms as ReadonlyArray<(typeof release.platforms)[number]>;
  if (platforms.length === 0) return null;
  return (
    <div className="tablewrap" style={{ marginBottom: 24 }}>
      <table>
        <thead>
          <tr>
            <th>Platform</th>
            <th>Binary</th>
            <th>Size</th>
            <th>SHA-256</th>
          </tr>
        </thead>
        <tbody>
          {platforms.map((p) => (
            <tr key={p.key}>
              <td>{p.label}</td>
              <td className="mono">
                <a href={p.url} target="_blank" rel="noopener noreferrer">
                  <code>{p.name}</code>
                </a>
              </td>
              <td className="mono">
                <span className="win">{p.sizeMB}</span>
              </td>
              <td className="mono" style={{ fontSize: 11 }}>
                {p.sha256 ? <code>{p.sha256.slice(0, 16)}…</code> : "—"}
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}
