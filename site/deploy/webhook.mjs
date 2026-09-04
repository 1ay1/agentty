#!/usr/bin/env node
// Minimal, zero-dependency GitHub webhook listener for agentty.org auto-deploy.
// Listens on 127.0.0.1:$PORT (nginx proxies /_deploy-hook to it). On a verified
// `push` event to master of either 1ay1/agentty or 1ay1/agentty.org, it kicks
// autodeploy.sh. HMAC-SHA256 verified against $WEBHOOK_SECRET — unsigned or
// mismatched requests are rejected.
//
// Env:
//   WEBHOOK_SECRET  (required) shared secret configured on the GitHub webhook
//   PORT            (default 9099)
//   AUTODEPLOY      (default /home/ayush/projects/agentpp-site/deploy/autodeploy.sh)

import { createServer } from "node:http";
import { createHmac, timingSafeEqual } from "node:crypto";
import { spawn } from "node:child_process";

const PORT = Number(process.env.PORT || 9099);
const SECRET = process.env.WEBHOOK_SECRET || "";
const AUTODEPLOY =
  process.env.AUTODEPLOY ||
  "/home/ayush/projects/agentty/site/deploy/autodeploy.sh";

const WATCHED = new Set(["1ay1/agentty"]);

function log(...a) {
  console.log(`[${new Date().toISOString()}]`, ...a);
}

if (!SECRET) {
  log("FATAL: WEBHOOK_SECRET not set");
  process.exit(1);
}

// Debounce: coalesce a burst of near-simultaneous pushes into one deploy.
let pending = null;
function triggerDeploy(reason) {
  if (pending) {
    log(`deploy already queued — ${reason} folded in`);
    return;
  }
  pending = setTimeout(() => {
    pending = null;
    log(`launching autodeploy (${reason})`);
    const child = spawn("bash", [AUTODEPLOY], {
      env: { ...process.env, FORCE: "1" },
      detached: true,
      stdio: "ignore",
    });
    child.on("error", (e) => log("autodeploy spawn error:", e.message));
    child.unref();
  }, 3000);
}

function verify(sigHeader, body) {
  if (!sigHeader) return false;
  const expected = "sha256=" + createHmac("sha256", SECRET).update(body).digest("hex");
  const a = Buffer.from(sigHeader);
  const b = Buffer.from(expected);
  return a.length === b.length && timingSafeEqual(a, b);
}

const server = createServer((req, res) => {
  if (req.method === "GET" && req.url === "/health") {
    res.writeHead(200, { "content-type": "text/plain" });
    return res.end("ok\n");
  }
  if (req.method !== "POST") {
    res.writeHead(405);
    return res.end("method not allowed\n");
  }

  const chunks = [];
  let size = 0;
  req.on("data", (c) => {
    size += c.length;
    if (size > 5_000_000) req.destroy(); // 5 MB guard
    chunks.push(c);
  });
  req.on("end", () => {
    const body = Buffer.concat(chunks);
    const sig = req.headers["x-hub-signature-256"];
    if (!verify(sig, body)) {
      log("rejected: bad signature");
      res.writeHead(401);
      return res.end("bad signature\n");
    }
    const event = req.headers["x-github-event"];
    let payload = {};
    try {
      payload = JSON.parse(body.toString("utf8"));
    } catch {
      res.writeHead(400);
      return res.end("bad json\n");
    }

    if (event === "ping") {
      log("ping from", payload.repository?.full_name);
      res.writeHead(200);
      return res.end("pong\n");
    }

    if (event === "push") {
      const repo = payload.repository?.full_name;
      const ref = payload.ref;
      if (!WATCHED.has(repo)) {
        log(`ignored push from unwatched repo ${repo}`);
        res.writeHead(202);
        return res.end("ignored\n");
      }
      if (ref !== "refs/heads/master") {
        log(`ignored push to ${ref} on ${repo}`);
        res.writeHead(202);
        return res.end("ignored (not master)\n");
      }
      // For the agentty repo, only redeploy when docs/website changed — a code
      // push shouldn't rebuild the site. (The site refetches version/sizes on
      // the release-published path anyway.)
      // Unified repo: rebuild when a push touches site CONTENT (docs/website/,
      // which includes docs/website/content/) or the site RUNTIME (site/) or
      // the CHANGELOG. Anything else (C++ source, tests) doesn't affect the site.
      if (repo === "1ay1/agentty") {
        const files = new Set(
          (payload.commits || []).flatMap((c) => [
            ...(c.added || []),
            ...(c.modified || []),
            ...(c.removed || []),
          ]),
        );
        const touchedSite = [...files].some(
          (f) =>
            f.startsWith("docs/website/") ||
            f.startsWith("site/") ||
            f === "CHANGELOG.md",
        );
        if (!touchedSite) {
          log(`agentty push ${payload.after?.slice(0, 8)} did not touch docs/website, site/ or CHANGELOG — ignored`);
          res.writeHead(202);
          return res.end("ignored (no site change)\n");
        }
      }
      log(`accepted push ${repo}@${payload.after?.slice(0, 8)} → deploy`);
      triggerDeploy(`push ${repo}`);
      res.writeHead(202);
      return res.end("deploy queued\n");
    }

    if (event === "release" && payload.action === "published") {
      log(`release published on ${payload.repository?.full_name} → deploy (refresh version/sizes)`);
      triggerDeploy("release published");
      res.writeHead(202);
      return res.end("deploy queued\n");
    }

    log(`ignored event: ${event}`);
    res.writeHead(202);
    res.end("ignored\n");
  });
});

server.listen(PORT, "127.0.0.1", () => log(`webhook listener on 127.0.0.1:${PORT}`));
