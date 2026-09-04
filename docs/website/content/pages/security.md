---
title: Security Policy
description: How to report a security vulnerability in agentty.
slug: security
---

We take the security of agentty seriously — it handles your provider credentials and executes shell commands on your behalf.

## Reporting a vulnerability

Please **do not** open a public issue for security vulnerabilities. Instead, report privately through GitHub's [private security advisory](https://github.com/1ay1/agentty/security/advisories/new) form. Include a description, reproduction steps, affected versions, and impact.

## What to expect

- Acknowledgement of your report as soon as it's triaged.
- An assessment of severity and affected versions.
- A fix and coordinated disclosure once a patched release is available.
- Credit in the advisory, if you'd like it.

## Supported versions

agentty is pre-1.0; security fixes land on the latest release. Always update to the newest version (re-run the install one-liner) before reporting — the issue may already be fixed.

## Security model at a glance

- **Credentials** live at `~/.config/agentty/credentials.json`, mode `0600`, written atomically.
- **Shell calls** are sandboxed by default (`bwrap` / `sandbox-exec`) — see [Sandboxing](/docs/sandboxing).
- **Filesystem tools** are confined to the workspace — see [Workspace Boundary](/docs/workspace).
- **TLS** is pinned end-to-end, including through the air-gap SOCKS tunnel.
- **Air-gap mode** trusts the remote host with your tokens — review the [trust model](/docs/airgap) before using it.
