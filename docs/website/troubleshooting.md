---
title: Troubleshooting
description: Common issues and how to resolve them.
nav_section: Help
nav_order: 10
slug: troubleshooting
---

The usual suspects, and how to get unstuck.

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

## Garbled rendering

Some terminals lag on DEC 2026 synchronized output. File a bug with your `$TERM`, the terminal emulator name, and a screenshot.

:::note
Found something not covered here? [Open an issue](https://github.com/1ay1/agentty/issues) with `$TERM`, your emulator, the version, and a screenshot or paste of the relevant block.
:::
