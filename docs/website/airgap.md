---
title: SSH Air-gap
description: Run agentty on a host with no direct internet, relaying bytes over SSH.
nav_section: Advanced
nav_order: 10
slug: airgap
---

Run agentty on a box that can't reach the internet directly. Your laptop relays the bytes; TLS pins on the real upstreams, so the network in between can't MITM you.

## One command

From the laptop that *does* have internet:

```bash
agentty airgap --setup user@airgapped-host    # first time: also copies your credentials
agentty airgap user@airgapped-host            # every time after
```

## How it works

`ssh -R 1080` exposes a SOCKS5 proxy on the remote at `localhost:1080`; connections to it tunnel back over SSH and are dialed by your laptop. The remote agentty gets `AGENTTY_SOCKS_PROXY=localhost:1080` and routes every TCP destination through it — chat, OAuth refresh, `web_fetch`, `web_search`. One env var, no per-host enumeration.

## Bare-metal version

If you'd rather not use the wrapper:

```bash
ssh -t -R 1080 user@airgapped-host \
    'AGENTTY_SOCKS_PROXY=localhost:1080 agentty'
```

Requires OpenSSH ≥ 7.6 on both ends (October 2017 — every distro has it). `AGENTTY_AIRGAP_SSH` injects extra `ssh` flags; `--remote-agentty PATH` if it isn't on the remote PATH.

:::warn Trust model
Airgap doesn't trust the network between laptop and remote, but it *does* trust the remote with your tokens — `--setup` copies `credentials.json` over at mode `600`. A compromised remote can exfiltrate your Anthropic credentials independent of the tunnel. Use it only on hosts you'd already trust with the same secret.
:::

## Run it inside Zed (ACP over airgap)

The same tunnel drives agentty as an editor agent. `agentty airgap user@remote --acp [flags…]` prints a ready-to-paste Zed `agent_servers` block whose `command` is `ssh` itself — one process is the SOCKS tunnel, the remote `agentty acp`, and the JSON-RPC transport, all owned by Zed. Full walkthrough — roles, prerequisites, the one paste, and troubleshooting — lives in [Use agentty inside Zed → air-gapped remote](/docs/acp#airgap).
