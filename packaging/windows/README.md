# Windows installer (MSI) — build & code-signing

`agentty.wxs` is a [WiX v4](https://wixtoolset.org/) source that produces a
proper Windows installer (`agentty-windows-x86_64.msi`) which:

- installs `agentty.exe` to `%ProgramFiles%\agentty\`
- adds that folder to the **system PATH** (so `agentty` works in any shell)
- creates a **Start Menu** shortcut
- registers a normal **Add/Remove Programs** entry with a working uninstall
- carries a fixed `UpgradeCode`, so installing a newer MSI cleanly replaces the old

CI builds (and, when secrets are present, **code-signs**) this on every tagged
release — see the `build-windows` job in `.github/workflows/release.yml`.

## Build locally (Windows)

```powershell
dotnet tool install --global wix
.\packaging\windows\build-msi.ps1 -Version 0.1.0 -Exe agentty-windows-x86_64.exe -Arch x64
```

That emits `agentty-windows-x86_64.msi` (unsigned). Add `-Sign` to sign it (see below).

## Code signing — getting rid of the SmartScreen warning

An unsigned installer triggers Windows SmartScreen ("unknown publisher"). To
ship a **trusted, signed** installer you need a publicly-trusted code-signing
certificate. Since 2023 the private key must live on FIPS-140-2 hardware/HSM,
so signing in CI is done through a cloud signing service. We use
**[Azure Trusted Signing](https://learn.microsoft.com/azure/trusted-signing/)**
(~\$10/month; EV-validated profiles are trusted by SmartScreen immediately).

### One-time setup

1. Create a **Trusted Signing account** + a **certificate profile** in Azure.
2. Create an **app registration (service principal)** and grant it the
   *Trusted Signing Certificate Profile Signer* role on the account.
3. Add these as **GitHub Actions repository secrets**:

   | Secret | Value |
   |--------|-------|
   | `AZURE_TENANT_ID` | the service-principal tenant id |
   | `AZURE_CLIENT_ID` | the service-principal app id |
   | `AZURE_CLIENT_SECRET` | the service-principal secret |
   | `TRUSTED_SIGNING_ENDPOINT` | e.g. `https://wus2.codesigning.azure.net` |
   | `TRUSTED_SIGNING_ACCOUNT` | your Trusted Signing account name |
   | `TRUSTED_SIGNING_PROFILE` | your certificate profile name |

With those secrets present, the release workflow signs **both** `agentty.exe`
and the `.msi`. Without them, it still publishes a valid **unsigned** MSI.

> Alternatives to Azure Trusted Signing: DigiCert KeyLocker, SSL.com eSigner —
> any service that exposes a signtool dlib works; adjust `build-msi.ps1`
> accordingly.

## Files

| File | Purpose |
|------|---------|
| `agentty.wxs` | WiX v4 installer definition |
| `agentty.ico` | Multi-resolution app icon (16–256px) |
| `build-msi.ps1` | Builds + optionally signs the MSI |
| `license.rtf` | Generated from `LICENSE` for the installer UI (gitignored) |
