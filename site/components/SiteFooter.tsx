import Link from "next/link";
import { site } from "@/lib/site";

export function SiteFooter() {
  const year = new Date().getFullYear();
  return (
    <footer className="foot">
      <div className="wrap">
        <div className="foot-grid">
          <div>
            <Link className="brand" href="/" aria-label="agentty home">
              <span className="brand-mark" aria-hidden="true">▌</span>
              <span className="brand-name">agentty</span>
            </Link>
            <p className="foot-blurb">
              A blazing-fast coding agent in your terminal. A native C++26 agent —
              one static binary, sandboxed by default, bring any model.
            </p>
          </div>
          <div>
            <h2 className="foot-h">Docs</h2>
            <Link href="/docs">Introduction</Link>
            <Link href="/docs/installation">Installation</Link>
            <Link href="/docs/quick-start">Quick Start</Link>
            <Link href="/docs/keybindings">Keybindings</Link>
            <Link href="/docs/airgap">SSH Air-gap</Link>
          </div>
          <div>
            <h2 className="foot-h">Project</h2>
            <a href={site.github} target="_blank" rel="noopener noreferrer">GitHub</a>
            <a href={site.releasesLatest} target="_blank" rel="noopener noreferrer">Releases</a>
            <Link href="/changelog">Changelog</Link>
            <Link href="/contributing">Contributing</Link>
            <Link href="/security">Security</Link>
          </div>
          <div>
            <h2 className="foot-h">Compare</h2>
            <Link href="/alternatives/claude-code-alternative">Claude Code alternative</Link>
            <Link href="/alternatives/aider-alternative">Aider alternative</Link>
            <Link href="/alternatives/cursor-alternative">Cursor alternative</Link>
            <Link href="/compare">All comparisons</Link>
            <Link href="/guides">Guides</Link>
          </div>
          <div>
            <h2 className="foot-h">Community</h2>
            <a href={site.discord} target="_blank" rel="noopener noreferrer">Discord</a>
            <Link href="/community">Get Involved</Link>
            <a href={site.issues} target="_blank" rel="noopener noreferrer">Report a Bug</a>
            <Link href="/acknowledgements">Acknowledgements</Link>
            <Link href="/code-of-conduct">Code of Conduct</Link>
            <Link href="/license">License</Link>
          </div>
        </div>
        <div className="foot-bottom">
          <span>© {year} agentty contributors · {site.license} Licensed</span>
          <span>Built with C++26 · Bring your own model</span>
        </div>
      </div>
    </footer>
  );
}
