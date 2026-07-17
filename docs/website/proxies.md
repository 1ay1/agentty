---
title: Corporate Proxies
description: Make agentty work behind TLS-terminating forward proxies.
nav_section: Advanced
nav_order: 50
slug: proxies
---

SOCKS keeps TLS end-to-end, so cert verification works untouched. A forward proxy that re-encrypts with its own certificate is a different story.

## SOCKS proxies

Because SOCKS doesn't terminate TLS, agentty's certificate verification works exactly as it does on a direct connection. This is also why [air-gap mode](/docs/airgap) is safe over an untrusted network.

:::note
agentty routes through SOCKS via its own `AGENTTY_SOCKS_PROXY=host:port` variable — it does **not** read the ambient `HTTP_PROXY`/`HTTPS_PROXY`/`NO_PROXY` variables, so a stray shell export can't silently reroute your API traffic. Airgap mode sets it for you.
:::

```bash
AGENTTY_SOCKS_PROXY=127.0.0.1:1080 agentty
```

## TLS-terminating proxies (Zscaler, Bluecoat, mitmproxy)

If your network routes through a forward proxy that re-encrypts traffic with its own CA, install that CA into the system trust store — agentty picks up system roots at startup:

```bash
# Debian / Ubuntu
sudo cp corp-proxy-ca.crt /usr/local/share/ca-certificates/
sudo update-ca-certificates

# Fedora / RHEL
sudo cp corp-proxy-ca.crt /etc/pki/ca-trust/source/anchors/
sudo update-ca-trust
```

## Last resort

If you genuinely can't install the CA, you can disable peer verification:

```bash
AGENTTY_INSECURE=1 agentty
```

:::warn
`AGENTTY_INSECURE=1` skips peer verification entirely — anyone on the path can impersonate the API. Don't ship that to anyone you care about; use it only as a temporary local workaround.
:::
