# Packaging

agentty ships as a single fully-static binary, so every package below simply
installs the published GitHub release artifact for the target arch — no source
build. **Versioning is centralized**: the single source of truth is
`project(agentty VERSION X.Y.Z)` in `CMakeLists.txt`. `scripts/release.sh`
reads that line and rewrites the version (and pins per-arch checksums) into
every manifest at release time. Never hardcode a version in a manifest.

## Cutting a release (the ONE manual step)

```sh
scripts/cut-release.sh X.Y.Z          # POSIX / macOS / Linux / Git-Bash
scripts\cut-release.cmd X.Y.Z         # Windows cmd.exe
```

That's the whole ritual. The script bumps `project(agentty VERSION …)` in
`CMakeLists.txt`, promotes `CHANGELOG.md`'s `[Unreleased]` section to
`[X.Y.Z]`, commits `release: vX.Y.Z`, creates the annotated tag, and pushes
branch + tag. **The tag push is what fires everything downstream** —
`.github/workflows/release.yml` then builds every binary, every OS package,
and submits to winget/homebrew/scoop/AUR (plus attaches nix/snap/gentoo
manifests), all in the cloud with no further input.

Guards: refuses a downgrade or duplicate version, requires a clean tree, and
rejects a tag that already exists. Preview with `--dry-run` (writes nothing);
commit+tag without pushing with `--no-push`.

> `scripts/release.sh` is a different tool — it *builds* release artifacts
> locally on the host it runs on (used by CI and for local reproduction). You
> don't run it by hand to ship; `cut-release` + CI does that for you.

## Install matrix

Linux

| Distro / manager   | Command                     | Manifest                        |
|--------------------|-----------------------------|---------------------------------|
| Ubuntu / Debian    | `apt-get install agentty`   | `deb/` (`.deb` via `build.sh`)  |
| Arch / Manjaro     | `pacman -S agentty`         | `arch/PKGBUILD` (AUR)           |
| Fedora             | `dnf install agentty`       | `rpm/agentty.spec.in`           |
| CentOS / RHEL      | `yum install agentty`       | `rpm/agentty.spec.in`           |
| openSUSE           | `zypper install agentty`    | `rpm/agentty.spec.in` (same rpm)|
| Alpine             | `apk add agentty`           | `alpine/APKBUILD`               |
| Snap               | `snap install agentty`      | `snap/snapcraft.yaml.in`        |
| Nix                | `nix-env -iA agentty`       | `nix/default.nix`               |
| Gentoo             | `emerge agentty`            | `gentoo/agentty-9999.ebuild`    |

macOS / Windows

| Platform | Command                | Manifest                    |
|----------|------------------------|-----------------------------|
| macOS    | `brew install agentty` | `homebrew/agentty.rb`       |
| Windows  | `scoop install agentty`| `scoop/agentty.json`        |
| Windows  | `winget install agentty`| `winget/*.yaml`            |
| Windows  | `.msi` installer       | `windows/agentty.wxs`       |

Universal (no package manager):

```sh
curl -fsSL https://raw.githubusercontent.com/1ay1/agentty/master/install.sh | sh
```

## How the version flows

```
CMakeLists.txt  project(agentty VERSION X.Y.Z)
        │
        ▼  scripts/release.sh reads VERSION, builds binaries, computes SHA256SUMS
        │
        ├─ deb/rpm/arch  → built directly with $VERSION
        ├─ alpine  APKBUILD      (@VERSION@ → $VERSION, sha512 pinned)
        ├─ nix     default.nix   (@VERSION@ + sha256 pinned)
        ├─ snap    snapcraft.yaml (@VERSION@ → $VERSION)
        ├─ gentoo  agentty-$VERSION.ebuild (version lives in filename / PV)
        ├─ homebrew agentty.rb   (version + all 4 sha256 pinned)
        └─ scoop   agentty.json  (version + win sha256 pinned)
```

Generated, version-pinned manifests land in `dist/packaging/` for publishing
to their respective repositories (AUR, alpine aports, nixpkgs, snapcraft,
a Gentoo overlay, the Homebrew tap, the scoop bucket).

## Automated publishing (CI)

`.github/workflows/release.yml` runs on every `vX.Y.Z` tag push. Besides
building + uploading all binaries and OS packages, it opens/pushes the
downstream package updates automatically — each step is **gated on a secret**
and skips silently when that secret is absent, so the release never fails just
because a channel isn't configured yet.

| Job / channel        | What it does                                   | Secret needed   |
|----------------------|------------------------------------------------|-----------------|
| `publish-winget`     | PR to microsoft/winget-pkgs                    | `WINGET_TOKEN`  |
| `publish-homebrew`   | push formula to 1ay1/homebrew-tap              | `TAP_TOKEN`     |
| `publish-scoop`      | push manifest to 1ay1/scoop-bucket             | `SCOOP_TOKEN`   |
| `publish-aur`        | push agentty-bin PKGBUILD + .SRCINFO to AUR    | `AUR_SSH_KEY`   |
| `package-alpine`     | build `.apk`, attach to the release            | *(none)*        |
| `publish-manifests`  | pin + attach nix/snap/gentoo manifests         | *(none)*        |

Secrets:

- **`WINGET_TOKEN` / `TAP_TOKEN` / `SCOOP_TOKEN`** — GitHub PAT (classic,
  `public_repo` / `repo` scope) that can push to the respective repo.
- **`AUR_SSH_KEY`** — an SSH private key whose public half is registered on
  the AUR account that owns `agentty-bin`.

Every job derives the version from the CMake `project(agentty VERSION …)` line
(via the `prepare` job's `version` output) and pins checksums from the release
`SHA256SUMS` — so tagging is the *only* manual step. `nixpkgs`, `snapcraft`,
and a Gentoo overlay aren't auto-PR'd (they need human review / store login);
their pinned manifests are attached to the release for a one-command submit.
