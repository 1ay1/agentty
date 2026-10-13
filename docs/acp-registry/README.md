# Submitting agentty to the ACP Registry

The [ACP Registry](https://agentclientprotocol.com/registry) distributes
ACP-compatible agents to every registry-aware client (Zed, JetBrains, …).
Registering agentty once makes it installable + auto-updated in all of them,
next to Claude Code, Codex, Copilot CLI, Gemini, and OpenCode.

This directory holds the ready-to-submit manifest.

## Files

| File | Purpose |
|------|---------|
| `agent.json` | The registry manifest (schema: `agent.schema.json` in the registry repo) |
| `icon.svg` | 16×16 agent icon |

## Prerequisites (already satisfied by agentty)

The registry is **curated to agents that support authentication** — CI verifies
the agent returns valid `authMethods` in the ACP `initialize` handshake.
agentty's ACP server advertises the `agentty-login` auth method when no
credentials are present (`src/acp/server.cpp` → `on_initialize`), so a
fresh install passes the check. (Once the user runs `agentty login`, the
handshake reports no auth methods — "ready".)

agentty ships raw per-platform binaries as GitHub release assets
(`agentty-<os>-<arch>`), which the registry's `binary` distribution consumes
directly (raw binaries are a supported archive form).

## Submit (one-time)

1. Fork <https://github.com/agentclientprotocol/registry>.
2. Create a folder `agents/agentty/` (agent id = `agentty`).
3. Copy this directory's `agent.json` and `icon.svg` into it.
4. Validate against the schema:
   ```sh
   # from the registry repo root, if it ships a validator:
   npm run validate   # or: check the PR CI output
   ```
5. Open a PR. CI verifies the schema **and** performs the ACP auth handshake
   against the pinned binary, so make sure the `v<version>` release assets in
   `agent.json` are published on GitHub first.

## Keeping it current

The registry runs an hourly cron that auto-bumps `version` from new GitHub
releases — so after the initial PR lands, **no further PRs are needed** for
version bumps. Just publish a new `v<x.y.z>` GitHub release with the same
asset names and the registry follows.

## When bumping the pinned version by hand

If you ever need to hand-edit (e.g. asset-name change), update in lockstep:
- `version` → the new semver (no leading `v`),
- every `distribution.binary.*.archive` URL → `.../releases/download/v<x.y.z>/<asset>`.

The asset base names are fixed by `src/util/update.cpp::platform_asset()`:

| registry key | agentty asset |
|--------------|---------------|
| `darwin-aarch64` | `agentty-macos-arm64` |
| `darwin-x86_64`  | `agentty-macos-x86_64` |
| `linux-aarch64`  | `agentty-linux-aarch64` |
| `linux-x86_64`   | `agentty-linux-x86_64` |
| `windows-x86_64` | `agentty-windows-x86_64.exe` |

(Keep the two in sync: the same assets power both self-update and the registry.)

## Verifying locally before you submit

```sh
# The agent must respond to an ACP initialize with authMethods when logged out.
AGENTTY_LOG=acp=trace agentty acp   # then send an `initialize` frame; frames land in ~/.agentty/logs/agentty.log
```

See `docs/acp-editor-integration.md` for the full ACP surface agentty exposes.
