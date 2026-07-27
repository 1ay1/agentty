# Windows installer (MSI) — build & code-signing

`agentty.wxs` is a [WiX v4](https://wixtoolset.org/) source that produces a
proper Windows installer (`agentty-windows-x86_64.msi`) which:

- installs **per-user, with no admin rights / UAC prompt** — a double-click
  just works
- installs `agentty.exe` to `%LocalAppData%\Programs\agentty\`
- adds that folder to the **user PATH** (so `agentty` works in any shell)
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

An unsigned installer triggers Windows SmartScreen ("unknown publisher"). You
have three ways to deal with this — **you do not need Azure**:

### Option A (free, no signing): ship via winget / Scoop

Package managers download from the GitHub release and install with **no
SmartScreen warning at all** — the manager is trusted, not your binary. For a
CLI this is what most users prefer:

```powershell
winget install agentty        # or: scoop install agentty
```

The winget manifest lives in `packaging/winget/`; the `publish-winget` job in
the release workflow opens a PR to `microsoft/winget-pkgs` automatically once
you add a `WINGET_TOKEN` secret (a classic PAT with `public_repo` scope). Zero
cost, zero Azure.

### Option B (any CA, simplest paid path): a PFX certificate

Buy a code-signing certificate from **any** CA — SSL.com, Certum, DigiCert,
GlobalSign (~\$200–600/yr). Export it as a `.pfx`, base64-encode it, and add two
GitHub secrets:

| Secret | Value |
|--------|-------|
| `WINDOWS_CERT_BASE64` | `base64 -w0 cert.pfx` output |
| `WINDOWS_CERT_PASSWORD` | the PFX password |

`build-msi.ps1` signs both the `.exe` and `.msi` with it. An **OV** cert clears
SmartScreen after it builds download reputation; an **EV** cert clears it
immediately (but EV keys live on a hardware token and can't be exported — use
the vendor's cloud signer or Option C for those).

> Note: as of 2023, publicly-trusted keys must live on FIPS-140-2 hardware. A
> plain exported `.pfx` still works for **OV** certs from CAs that issue them in
> software; for EV, use a cloud signing service.

### Option C (cloud HSM): Azure Trusted Signing

If you'd rather not handle a token, Azure Trusted Signing (~\$10/mo) keeps the
key in Microsoft's HSM and clears SmartScreen immediately. Set
`TRUSTED_SIGNING_ENDPOINT`, `TRUSTED_SIGNING_ACCOUNT`, `TRUSTED_SIGNING_PROFILE`
(+ the `AZURE_*` service-principal secrets). `build-msi.ps1` auto-detects this
backend.

**With none of the above configured, CI still publishes a valid UNSIGNED MSI**
— which already gives PATH + Start Menu + clean uninstall, a big step up from a
raw `.exe`. Most users will install via winget/Scoop anyway and never see a
warning.

## Files

| File | Purpose |
|------|---------|
| `agentty.wxs` | WiX v4 installer definition |
| `agentty.ico` | Multi-resolution app icon (16–256px) |
| `build-msi.ps1` | Builds + optionally signs the MSI |
| `license.rtf` | Generated from `LICENSE` for the installer UI (gitignored) |
