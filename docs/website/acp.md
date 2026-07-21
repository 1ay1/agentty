---
title: Zed / ACP
description: Run agentty as an Agent Client Protocol agent inside Zed — streaming responses, inline diffs, native permission prompts, session reload, and a one-command air-gapped setup. Same engine as the TUI, driven over JSON-RPC on stdio.
nav_section: Advanced
nav_order: 20
slug: acp
---

agentty speaks the [Agent Client Protocol](https://agentclientprotocol.com) — the same protocol Zed uses to drive Claude Code and Gemini. Point Zed at the `agentty acp` subcommand and your terminal agent becomes a first-class agent panel inside the editor: streaming responses, inline diffs for every edit, and native permission prompts before any file write or shell command.

## Set up in Zed

Add this to Zed's `settings.json` (`zed: open settings`):

```json
{
  "agent_servers": {
    "agentty": {
      "command": "agentty",
      "args": ["acp"]
    }
  }
}
```

Then open the agent panel (`cmd-?` / `ctrl-?`), pick **agentty** from the agent list, and prompt. Auth is whatever `agentty login` already set up — the ACP process reads the same `~/.config/agentty/credentials.json`, so there's nothing extra to configure.

## Model & permission profile

Set the model per-subprocess in the `args`. In ACP mode `-m` is an *ephemeral* override — it does **not** clobber your TUI's saved model:

```json
{
  "agent_servers": {
    "agentty": {
      "command": "agentty",
      "args": ["acp", "-m", "claude-haiku-4-5", "--profile", "ask"]
    }
  }
}
```

`--profile` picks how eagerly Zed prompts you before a tool runs:

- **`ask`** (default) — prompt for `write` / `edit` / `bash` / network; read-only inspection (`read` / `grep` / `glob` / `list_dir`) runs without a dialog so the loop stays fluid.
- **`minimal`** — prompt for *everything* that touches the outside world, reads included. Tightest leash.
- **`write`** — fully autonomous: *every* tool (`write` / `edit` / `bash` / network included) runs without a prompt. Loosest leash — the same auto-approve-everything tier as the TUI's Write mode.

## What works over ACP

- **Streaming text** — the model's reply renders token-by-token in Zed's panel.
- **Tool calls** — every `read` / `edit` / `bash` / `grep` / … shows up as a Zed tool card with the right icon, the raw arguments, and live status (pending → running → done/failed).
- **Inline diffs** — `write` and `edit` emit ACP `diff` content, so Zed renders the file change inline and lets you review it in place.
- **Follow-along** — read/edit/write/grep tool calls carry the file path as an ACP `location`, so Zed can open and highlight the file the agent is touching in real time.
- **Permission prompts** — side-effecting tools (`bash`, `write`, `edit`, network) trigger Zed's native allow/reject dialog before they run; `--profile` tunes exactly which tools prompt.
- **Session modes** — the three permission tiers (`Ask` / `Write` / `Minimal`) surface as ACP session modes, so you can switch them live from Zed's mode picker (`session/set_mode`) without restarting the agent. The starting tier is the `--profile` flag.
- **Cancellation** — stop a turn from Zed and the in-flight stream tears down.
- **Full session lifecycle** — agentty advertises and implements the complete ACP v1 session surface: `session/new`, `session/load`, `session/resume`, `session/list`, `session/close`, `session/delete`, plus `logout`. Zed can enumerate past sessions, reopen any of them, and prune them — all backed by the on-disk thread store.
- **Session persistence + reload** — every session is written to agentty's on-disk thread store after each turn (the *same* format the TUI uses), so it survives a subprocess restart. Zed can `session/load` to resume a past conversation: agentty replays the full transcript (user + assistant messages and tool cards) as `session/update` notifications, then hands back control. Sessions started in Zed also show up in the standalone TUI's thread picker, and vice versa.
- **Workspace sandbox** — file tools stay inside the session's `cwd` (the folder you opened in Zed); `bash` is wrapped in bwrap/sandbox-exec exactly like the standalone TUI.

:::tip
The ACP agent is the *same* engine as the TUI — same provider, same tools, same wire-message shaping, same permission policy — just driven over JSON-RPC on stdio instead of a terminal. Any other ACP client (not just Zed) works the same way.
:::

## The protocol surface

`agentty acp` is a headless subcommand that speaks newline-delimited JSON-RPC 2.0 over stdio and implements the full ACP v1 agent surface: `initialize` (capability negotiation), `authenticate`, `session/new`, `session/load`, `session/resume`, `session/list`, `session/close`, `session/delete`, `session/set_mode`, `session/prompt` (drives a complete agent turn), and `session/cancel`. While a turn runs it streams `session/update` notifications — `agent_message_chunk` for model text, `tool_call` / `tool_call_update` for every tool — and calls back with `session/request_permission` before any side-effecting tool runs. There is no maya/UI dependency, so cold start is fast: ACP mode prewarms the TLS/DNS connection to Anthropic before serving, eliminating the first-prompt handshake latency.

:::tip
Run `agentty acp` by hand and it sits waiting for newline-delimited JSON-RPC on stdin (diagnostics go to stderr; stdout is the protocol channel). The repo ships two reference clients: `scripts/acp_smoke.py` drives a full initialize → prompt → tool → permission round-trip, and `scripts/acp_methods_test.py` exercises the rest of the v1 method surface (modes, list/resume/close/delete, logout) offline. The wire protocol itself lives in the `acp-cpp` submodule — agentty no longer hand-rolls JSON-RPC.
:::

## Loosen the workspace or sandbox

The same `--workspace` and `--sandbox` switches the TUI accepts apply in ACP mode (they run before the agent starts), so you can loosen or disable both from the Zed `args`:

```json
{
  "agent_servers": {
    "agentty": {
      "command": "agentty",
      "args": ["acp", "--workspace", "/", "--sandbox", "off"]
    }
  }
}
```

## agentty-in-Zed on an air-gapped remote

Yes — you can run agentty inside Zed against a server with **zero internet access**. Your laptop relays every byte; nothing runs on the remote besides the agent and your shell session. `agentty airgap <host> --acp` generates the whole Zed config for you, so there's no hand-assembled `ssh -N -R` tunnel or `env` block to maintain.

### The two machines

- **Laptop** — has internet, has your Anthropic OAuth/API key, runs Zed, runs `ssh`. This is the relay. *Everything below is done here.*
- **Remote** — the air-gapped box. No internet. Runs `agentty acp`, spawned by Zed over ssh.

### Prerequisites

1. `agentty` installed on **both** machines (`which agentty` on each prints a path).
2. Passwordless SSH from laptop to remote (`ssh user@remote echo ok` returns `ok` without prompting). If it prompts, run `ssh-copy-id user@remote` first.
3. You're logged in on the laptop (`agentty status` shows OAuth or API key). If not: `agentty login`.
4. Zed installed on the laptop.

### One-time setup (two commands, ~5 seconds)

From the laptop:

```bash
# 1. Copy your Anthropic credentials to the remote (once):
agentty airgap --setup user@remote

# 2. Print the Zed config block for this remote:
agentty airgap user@remote --acp -m claude-haiku-4-5 --profile ask
```

The second command **prints to stderr and exits** — it does not start anything. You'll see something like:

```text
agentty airgap --acp: add this to Zed's settings.json
  → /home/you/.config/zed/settings.json:

  "agent_servers": {
    "agentty (airgap)": {
      "command": "ssh",
      "args": ["-T", "-R", "1080", "-o", "ExitOnForwardFailure=yes", ...
              "user@remote",
              "AGENTTY_SOCKS_PROXY=localhost:1080 exec agentty acp -m claude-haiku-4-5 --profile ask"]
    }
  }
```

### Wire it into Zed (one paste)

1. Open Zed's settings: `cmd-,` (macOS) or `ctrl-,` (Linux).
2. Paste the printed `"agent_servers"` block into the JSON. If you already have an `agent_servers` object, merge the `"agentty (airgap)"` key into it.
3. Save.

### Use it

1. Open the agent panel in Zed: `cmd-?` / `ctrl-?`.
2. From the agent picker, pick **agentty (airgap)**.
3. Prompt.

That's it. Zed spawns `ssh` directly — a single process is the tunnel, the agent, and the JSON-RPC transport. Zed owns its lifecycle: close the agent panel and the ssh + remote `agentty acp` both die. No background `ssh -N`, no wrapper script, no daemon.

### What's happening under the hood

- Zed runs `ssh -R 1080 user@remote 'AGENTTY_SOCKS_PROXY=localhost:1080 exec agentty acp …'` directly.
- `-R 1080` exposes a SOCKS5 proxy on the remote's `localhost:1080`. The remote `agentty` routes every outbound connection (chat, OAuth refresh, `web_fetch`, `web_search`) through it. Those connections tunnel back over SSH and are dialed by your laptop.
- ACP JSON-RPC flows over ssh's stdio. No extra port, no extra process.
- TLS still negotiates end-to-end with the real upstream (api.anthropic.com etc.). The laptop sees encrypted bytes only — it can't MITM.

:::warn Trust model (read before --setup)
`--setup` copies your laptop's `~/.config/agentty/credentials.json` to the remote (chmod 600). That file contains your OAuth refresh token (or API key). A compromised remote can exfiltrate it independent of the tunnel. **agentty airgap protects the network between laptop and remote, not the remote itself** — treat the remote as a credential-bearing peer, not a sandboxed proxy. See [SSH Air-gap](/docs/airgap).
:::

### Troubleshooting

- **Zed shows the agent greyed out / "failed to start"** — check `~/.local/share/zed/logs/Zed.log` (Linux) or `~/Library/Logs/Zed/Zed.log` (macOS) for the ssh spawn line. Common causes: `ssh` not on Zed's PATH (rare), the remote prompts for a password (run `ssh-copy-id` first), or `agentty` isn't on the remote's PATH (pass `--remote-agentty /full/path/to/agentty` to the `airgap` command, then re-print the config).
- **"connection refused" or "could not resolve host" mid-turn** — the SOCKS tunnel dropped, usually a flaky network on the laptop. Close the agent panel in Zed and reopen — Zed respawns ssh.
- **Slow first response** — the first TLS handshake to api.anthropic.com tunnels through ssh, adding ~100 ms. Subsequent turns reuse the connection.
- **`agentty: command not found` in the ssh spawn log** — agentty isn't on the remote shell's non-interactive PATH (`ssh user@remote 'echo $PATH'` shows what Zed sees). Install it system-wide on the remote, or re-print the config with `--remote-agentty /full/path/to/agentty`.
- **The remote needs a non-default ssh port / key / jump host** — export `AGENTTY_AIRGAP_SSH="-p 2222 -i ~/.ssh/work -J bastion"` before running `agentty airgap … --acp`. Those flags get embedded into the printed config.

:::tip
Want to confirm the tunnel works before touching Zed? `agentty airgap user@remote` (no `--acp`) launches the agentty *TUI* running on the remote, in your local terminal. If that works, the ACP version will too — same tunnel, different transport.
:::
