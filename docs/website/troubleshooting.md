---
title: Troubleshooting
description: Common issues and how to resolve them.
nav_section: Help
nav_order: 10
slug: troubleshooting
---

The usual suspects, and how to get unstuck.

## `h2 socket hangup (server accepted request; not replayed automatically)`

The connection died **mid-answer**, after the provider had already started
streaming. agentty deliberately does not retry at that point: the turn is
semantically committed, and replaying it could duplicate the assistant message
or re-run tool calls.

This is almost always the network, not agentty and not the model. Corporate
proxies, VPNs and CDN edges routinely kill idle HTTP/2 connections at 30-60
seconds; agentty's connection pool holds them for 90, so it can hand you a
socket the network already closed.

Find out which:

```bash
grep http.stream_hangup ~/.agentty/logs/agentty.log
```

```
W net  http.stream_hangup: host=api.anthropic.com:443 reused=1 idle_ms=61402 status=200 …
```

- **`idle_ms` clusters near a round number** (30000, 60000) and `reused=1` —
  a network idle timeout. Set `AGENTTY_POOL_IDLE_TTL` below it:

  ```bash
  AGENTTY_POOL_IDLE_TTL=45 agentty      # evict before the network does
  AGENTTY_POOL_IDLE_TTL=0  agentty      # no pooling at all (safest, slower)
  ```

- **`idle_ms` is small or random** — ordinary packet loss or a flaky link;
  the pool is not the cause.

## agentty seems stuck after Esc

Fixed in current builds — a cancelled worker thread could null out a new turn's cancel token. Update to the latest release. If you still see it, restart the process and file a bug with your `git rev-parse HEAD` (or release version).

## Certificate / TLS verification errors

You're likely behind a TLS-terminating proxy. Install the proxy's CA into the system trust store — see [Corporate Proxies](/docs/proxies). As a last resort, `AGENTTY_INSECURE=1` skips verification (not for shared use).

## Auth not picked up

Check which source agentty will use:

```bash
agentty status
```

Remember the override order: `--key` > `ANTHROPIC_API_KEY` > `CLAUDE_CODE_OAUTH_TOKEN` > on-disk credentials. An env var will shadow the credentials file.

## Air-gap connection fails

- Confirm OpenSSH ≥ 7.6 on *both* ends.
- Make sure agentty is on the remote PATH, or pass `--remote-agentty PATH`.
- Run `--setup` once so credentials are copied to the remote.

## A local model (llama.cpp / vLLM) won't respond

agentty now diagnoses this instead of looping. The usual causes:

- **The spec is missing `/v1`.** Most local servers only serve under `/v1`. A bare `localhost:8080` gets the `/v1` prefix automatically, but a partial path like `localhost:8080/api` is honoured verbatim and will 404. Use the picker (`^P` → Custom host) so the derived path shows in the connect toast, or check with `AGENTTY_LOG=wire=trace`.
- **The model id doesn't match the server.** `llama-server` serves exactly what its `/v1/models` reports (often the GGUF filename). If a recalled id isn't listed, agentty refuses with *“model X isn't served by this host — pick one (^/)”* — open `^/` and pick a listed model.
- **A chat-template rejection.** Some models (gemma, gpt-oss) reject agentty's system prompt/tools with a streamed error. agentty surfaces the server's actual message; run `AGENTTY_LOG=wire=trace` to see the exact request and rejection.
- **Slow, not stuck.** A big model can process the prompt silently for minutes; the phase chip reads *“processing…”* and the timeout is 10 minutes for local endpoints. That's expected — `Esc` cancels if you don't want to wait.

See [Providers › Custom hosts](/docs/providers#custom-hosts) for the full model.

## Turning on logs for a bug report

```bash
AGENTTY_LOG=debug AGENTTY_LOG_FILE=/tmp/agentty.log agentty
# reproduce, then attach /tmp/agentty.log
```

For a provider/wire problem add `AGENTTY_LOG=wire=trace` to capture the raw request/response bytes. If agentty crashed, the stderr output already includes a backtrace and the last ~256 events (the flight recorder). Full details: **[Logging & diagnostics](/docs/logging)**.

## Ctrl+V pastes text instead of my image

Your clipboard lives on the machine your **terminal** runs on, so over SSH
agentty has to ask the terminal for it. Only **kitty** can send image bytes
back (OSC 5522); every other terminal answers a text-only dialect (OSC 52),
which is why the paste arrives as prose.

On kitty this is almost always one setting — its `clipboard_control` defaults
to write-only, so reads are refused:

```conf
# ~/.config/kitty/kitty.conf   (on the machine kitty runs on)
clipboard_control write-clipboard write-primary read-clipboard read-primary
```

Restart kitty fully. Inside tmux also run `tmux set -g allow-passthrough on`
(off by default), or the request never reaches kitty.

On any other terminal, attach by path (`@shot.png`) or set
`AGENTTY_CLIPBOARD_CMD` to ferry the clipboard over SSH. Full setup, including
the Linux `wl-clipboard` / `xclip` requirement and a troubleshooting table:
[Clipboard & Images](/docs/clipboard).

## Garbled rendering

Some terminals lag on DEC 2026 synchronized output. File a bug with your `$TERM`, the terminal emulator name, and a screenshot.

:::note
Found something not covered here? [Open an issue](https://github.com/1ay1/agentty/issues) with `$TERM`, your emulator, the version, and a screenshot or paste of the relevant block.
:::
