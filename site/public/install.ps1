# install.ps1 — agentty installer for Windows (PowerShell)
#
# Usage:
#   irm https://agentty.org/install.ps1 | iex
#   # with options:
#   & ([scriptblock]::Create((irm https://agentty.org/install.ps1))) -Prefix "$HOME\bin"
#
# Downloads the matching prebuilt agentty.exe from the latest GitHub release,
# verifies its SHA256 against the GitHub API, installs it, and adds the install
# dir to your user PATH. No build toolchain required.

[CmdletBinding()]
param(
    [string]$Version = "latest",
    [string]$Prefix  = "$env:LOCALAPPDATA\agentty",
    [switch]$NoVerify
)

$ErrorActionPreference = "Stop"
$Repo = "1ay1/agentty"

function Info($m) { Write-Host ":: $m" -ForegroundColor Blue }
function Ok($m)   { Write-Host "$([char]0x2713) $m" -ForegroundColor Green }
function Warn($m) { Write-Host "! $m" -ForegroundColor Yellow }
function Die($m)  { Write-Host "x install.ps1: $m" -ForegroundColor Red; exit 1 }

# --- detect arch -------------------------------------------------------------
$arch = $env:PROCESSOR_ARCHITECTURE
switch -Wildcard ($arch) {
    "AMD64" { $asset = "agentty-windows-x86_64.exe" }
    "x86"   { $asset = "agentty-windows-x86_64.exe" }  # 32-bit host can run the x64? fall through below
    "ARM64" { $asset = $null }                          # no prebuilt arm64 windows yet
    default { $asset = "agentty-windows-x86_64.exe" }
}
if ($arch -eq "ARM64") {
    Die "no prebuilt Windows ARM64 binary yet. Build from source: https://github.com/$Repo or run agentty via WSL using install.sh"
}

# --- resolve url -------------------------------------------------------------
if ($Version -eq "latest") {
    $base   = "https://github.com/$Repo/releases/latest/download"
    $apiUrl = "https://api.github.com/repos/$Repo/releases/latest"
} else {
    $base   = "https://github.com/$Repo/releases/download/$Version"
    $apiUrl = "https://api.github.com/repos/$Repo/releases/tags/$Version"
}

$bindir = $Prefix
New-Item -ItemType Directory -Force -Path $bindir | Out-Null
$dest = Join-Path $bindir "agentty.exe"
$tmp  = Join-Path ([System.IO.Path]::GetTempPath()) ([System.IO.Path]::GetRandomFileName() + ".exe")

# --- download ----------------------------------------------------------------
Info "downloading $asset"
try {
    Invoke-WebRequest -Uri "$base/$asset" -OutFile $tmp -UseBasicParsing
} catch {
    Die "download failed: $base/$asset  ($($_.Exception.Message))"
}
if (-not (Test-Path $tmp) -or (Get-Item $tmp).Length -eq 0) {
    Die "downloaded file is empty - asset '$asset' may not exist for $Version"
}

# --- verify ------------------------------------------------------------------
if ($NoVerify) {
    Warn "skipping SHA256 verification (-NoVerify)"
} else {
    try {
        $rel = Invoke-RestMethod -Uri $apiUrl -UseBasicParsing
        $a   = $rel.assets | Where-Object { $_.name -eq $asset } | Select-Object -First 1
        $expected = $null
        if ($a -and $a.digest) { $expected = ($a.digest -replace '^sha256:', '') }
    } catch { $expected = $null }

    $actual = (Get-FileHash -Algorithm SHA256 -Path $tmp).Hash.ToLower()
    if (-not $expected) {
        Warn "no published checksum for '$asset' - skipping verification"
    } elseif ($expected.ToLower() -eq $actual) {
        Ok "checksum verified"
    } else {
        Remove-Item $tmp -Force -ErrorAction SilentlyContinue
        Die "checksum mismatch for ${asset}: expected $expected, got $actual"
    }
}

# --- detect prior install ----------------------------------------------------
$prior = $null
if (Test-Path $dest) {
    try { $prior = (& $dest --version 2>$null | Select-String '^agentty ').ToString().Split(' ')[1] } catch {}
}

Move-Item -Force -Path $tmp -Destination $dest
$new = $null
try { $new = (& $dest --version 2>$null | Select-String '^agentty ').ToString().Split(' ')[1] } catch {}

if ($prior -and $new -and $prior -ne $new) {
    Ok "updated $dest  $prior -> $new"
} elseif ($prior -and $prior -eq $new) {
    Ok "reinstalled $dest ($new)"
} else {
    Ok "installed $dest$(if ($new) { " ($new)" })"
}

# --- PATH (user scope) -------------------------------------------------------
$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
if ($userPath -notlike "*$bindir*") {
    [Environment]::SetEnvironmentVariable("Path", "$userPath;$bindir", "User")
    $env:Path = "$env:Path;$bindir"
    Ok "added $bindir to your user PATH (restart your terminal to pick it up)"
}

# --- self-test ---------------------------------------------------------------
try {
    & $dest --version | Out-Null
    Ok "agentty is ready"
} catch {
    Warn "installed, but 'agentty --version' did not run cleanly. Open an issue: https://github.com/$Repo/issues"
}

Ok "run:  agentty"
