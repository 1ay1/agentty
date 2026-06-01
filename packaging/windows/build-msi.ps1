#requires -Version 5.1
<#
.SYNOPSIS
  Build (and optionally code-sign) the agentty Windows MSI installer.

.DESCRIPTION
  1. Optionally signs agentty.exe.
  2. Builds agentty-windows-<arch>.msi from packaging\windows\agentty.wxs
     using WiX v4 (the `wix` dotnet tool).
  3. Optionally signs the MSI.

  Signing uses **Azure Trusted Signing** via signtool + the Trusted Signing
  dlib, which keeps the private key in Microsoft's FIPS-140-2 HSM (required by
  the CA/Browser Forum since 2023 — you cannot sign with a local .pfx anymore
  for a publicly-trusted cert). With an EV-validated Trusted Signing account,
  SmartScreen trusts the installer immediately, with no "unknown publisher"
  warning.

  If the signing inputs are not provided, the script still produces a valid
  (unsigned) MSI — useful for local testing and PRs.

.PARAMETER Version
  Product version, e.g. 0.1.0 (from the CMake project version).

.PARAMETER Exe
  Path to the agentty.exe to package.

.PARAMETER Arch
  x64 (default) or arm64.

.PARAMETER Sign
  Switch — attempt code signing. Requires the AZURE_* / TRUSTED_SIGNING_*
  environment variables (see below). No-op + warning if they're missing.

.NOTES
  Signing env vars (set as GitHub Actions secrets):
    AZURE_TENANT_ID, AZURE_CLIENT_ID, AZURE_CLIENT_SECRET   (service principal)
    TRUSTED_SIGNING_ENDPOINT   e.g. https://wus2.codesigning.azure.net
    TRUSTED_SIGNING_ACCOUNT    your Trusted Signing account name
    TRUSTED_SIGNING_PROFILE    your certificate profile name
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$Version,
    [Parameter(Mandatory)] [string]$Exe,
    [ValidateSet("x64", "arm64")] [string]$Arch = "x64",
    [switch]$Sign
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Resolve-Path (Join-Path $here "..\..")

function Info($m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Ok($m)   { Write-Host "OK  $m"  -ForegroundColor Green }
function Warn($m) { Write-Host "!!  $m"  -ForegroundColor Yellow }

# arch → asset suffix used in the release (x64 -> x86_64, arm64 -> aarch64)
$assetArch = if ($Arch -eq "arm64") { "aarch64" } else { "x86_64" }
$msi  = "agentty-windows-$assetArch.msi"
$ico  = Join-Path $here "agentty.ico"
$lic  = Join-Path $repo "LICENSE"
$licRtf = Join-Path $here "license.rtf"

# WiX wants RTF for the license screen — wrap the plain MIT text once.
if (-not (Test-Path $licRtf)) {
    Info "wrapping LICENSE into RTF for the installer UI"
    $text = (Get-Content $lic -Raw) -replace "`r?`n", '\par '
    $rtf  = "{\rtf1\ansi\deff0{\fonttbl{\f0 Consolas;}}\fs18 $text}"
    Set-Content -Path $licRtf -Value $rtf -Encoding ASCII
}

# ---- ensure WiX v4 is available ----------------------------------------------
if (-not (Get-Command wix -ErrorAction SilentlyContinue)) {
    Info "installing WiX v4 dotnet tool"
    dotnet tool install --global wix | Out-Null
    $env:Path = "$env:Path;$env:USERPROFILE\.dotnet\tools"
}
# UI extension provides WixUI_InstallDir
wix extension add -g WixToolset.UI.wixext | Out-Null

# ---- (optional) sign the exe BEFORE packaging --------------------------------
function Invoke-TrustedSign($file) {
    if (-not $Sign) { return }
    if (-not $env:TRUSTED_SIGNING_ENDPOINT) {
        Warn "signing requested but TRUSTED_SIGNING_* env not set — leaving $file unsigned"
        return
    }
    Info "code-signing $file via Azure Trusted Signing"

    # Trusted Signing dlib + metadata describe the cert profile to signtool.
    $dlibDir = Join-Path $env:TEMP "tsdlib"
    if (-not (Test-Path "$dlibDir\bin\x64\Azure.CodeSigning.Dlib.dll")) {
        New-Item -ItemType Directory -Force -Path $dlibDir | Out-Null
        Info "fetching Microsoft.Trusted.Signing.Client"
        nuget install Microsoft.Trusted.Signing.Client -Version 1.0.60 `
            -OutputDirectory $dlibDir -ExcludeVersion | Out-Null
    }
    $dlib = Get-ChildItem -Recurse $dlibDir -Filter "Azure.CodeSigning.Dlib.dll" |
            Select-Object -First 1
    $meta = Join-Path $env:TEMP "metadata.json"
    @{
        Endpoint               = $env:TRUSTED_SIGNING_ENDPOINT
        CodeSigningAccountName = $env:TRUSTED_SIGNING_ACCOUNT
        CertificateProfileName = $env:TRUSTED_SIGNING_PROFILE
    } | ConvertTo-Json | Set-Content -Path $meta -Encoding ASCII

    $signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" |
                Sort-Object FullName | Select-Object -Last 1
    & $signtool.FullName sign `
        /v /fd SHA256 /tr "http://timestamp.acs.microsoft.com" /td SHA256 `
        /dlib $dlib.FullName /dmdf $meta `
        $file
    if ($LASTEXITCODE -ne 0) { throw "signtool failed on $file" }
    Ok "signed $file"
}

Invoke-TrustedSign $Exe

# ---- build the MSI -----------------------------------------------------------
Info "building $msi (WiX v4, $Arch)"
wix build (Join-Path $here "agentty.wxs") `
    -arch $Arch `
    -d Version=$Version `
    -d ExePath=$Exe `
    -d IconPath=$ico `
    -d LicensePath=$licRtf `
    -ext WixToolset.UI.wixext `
    -o $msi
if ($LASTEXITCODE -ne 0) { throw "wix build failed" }
Ok "built $msi"

# ---- sign the MSI ------------------------------------------------------------
Invoke-TrustedSign $msi

Ok "done: $msi"
