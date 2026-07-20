#!/bin/sh
# scripts/release.sh — one-shot reproducible release builder.
#
# OS-aware: builds and uploads only what makes sense on the HOST it's run on.
# Run on each platform separately and the assets accrete on the same `--tag`
# release (gh release upload --clobber is idempotent; it never deletes
# unrelated assets).
#
#   Linux host   → dist/agentty-linux-x86_64                  fully-static musl
#                  dist/agentty-linux-aarch64                 fully-static musl
#                  dist/agentty_<ver>_amd64.deb               .deb (debian/ubuntu)
#                  dist/agentty_<ver>_arm64.deb               .deb
#                  dist/agentty-<ver>-1.x86_64.rpm            .rpm (fedora/rhel)
#                  dist/agentty-<ver>-1.aarch64.rpm           .rpm
#                  dist/agentty-bin-<ver>-1-x86_64.pkg.tar.zst   arch .pkg
#                  dist/agentty-bin-<ver>-1-aarch64.pkg.tar.zst  arch .pkg
#                  dist/agentty-<ver>.tar.gz                  source tarball (homebrew)
#                  dist/packaging/{agentty.rb,PKGBUILD,       manifests (brew, AUR,
#                                  APKBUILD,default.nix,        alpine, nix, snap,
#                                  snapcraft.yaml,              gentoo)
#                                  agentty-<ver>.ebuild}
#   macOS host   → dist/agentty-macos-x86_64                  native (Intel)
#                  dist/agentty-macos-arm64                   native (Apple Silicon)
#                  dist/agentty-<ver>.tar.gz                  source tarball
#   Windows host → dist/agentty-windows-x86_64.exe            MSVC + vcpkg static
#                  dist/packaging/agentty.json                scoop manifest
#
#   All hosts    → dist/SHA256SUMS                            sha256 of every
#                                                             asset built this run
#
# Usage:
#   scripts/release.sh                  # build host-appropriate assets, no upload
#   scripts/release.sh --tag v0.1.1     # build + tag + upload via gh
#   scripts/release.sh --skip-binaries  # reuse existing dist/agentty-*
#                                       # (handy when iterating on packaging)
#
# When --tag is passed each binary uploads the instant its build finishes
# (Linux: both arches build in parallel; whichever wins lands first).
# Packages, tarball, SHA256SUMS, and manifests upload in a final sweep.
#
# Requirements (auto-detected, missing steps SKIPPED with a yellow note):
#   Linux:    docker (static musl + arch pkg), rpmbuild, gh
#   macOS:    Xcode CLT or LLVM, cmake, gh
#   Windows:  MSVC + cmake + vcpkg (or any from-source CMake env), gh
#
# Idempotent: re-runnable. Existing artifacts are overwritten; nothing
# outside dist/ is touched. No system packages installed.

set -eu

# ---- args --------------------------------------------------------------------
TAG=""
SKIP_BINARIES=0
UPLOAD=0
while [ $# -gt 0 ]; do
    case "$1" in
        --tag)            TAG=$2; UPLOAD=1; shift 2 ;;
        --skip-binaries)  SKIP_BINARIES=1; shift ;;
        --no-upload)      UPLOAD=0; shift ;;
        -h|--help)        sed -n '2,50p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "release.sh: unknown arg: $1" >&2; exit 2 ;;
    esac
done

# ---- paths -------------------------------------------------------------------
root=$(cd "$(dirname "$0")/.." && pwd)
dist="$root/dist"
pkgdir="$dist/packaging"
mkdir -p "$dist" "$pkgdir"

# ---- host OS detection -------------------------------------------------------
# Picks the platform-specific code path below. MSYS/CYGWIN/MINGW count as
# Windows so `release.sh` works under git-bash on a Windows host.
uname_s=$(uname -s 2>/dev/null || echo unknown)
case "$uname_s" in
    Linux)                          HOST_OS=linux   ;;
    Darwin)                         HOST_OS=macos   ;;
    CYGWIN*|MINGW*|MSYS*|Windows*)  HOST_OS=windows ;;
    *)                              HOST_OS=unknown ;;
esac

# ---- version (single source of truth: CMakeLists.txt) -----------------------
# `project(agentty VERSION X.Y.Z LANGUAGES CXX)` — every artifact, manifest,
# and tag is derived from this one line.
VERSION=$(sed -nE 's/.*project\(agentty VERSION ([0-9.]+).*/\1/p' "$root/CMakeLists.txt" | head -1)
[ -n "$VERSION" ] || { echo "release.sh: could not read VERSION from CMakeLists.txt" >&2; exit 1; }
if [ -n "$TAG" ]; then
    expected="v$VERSION"
    [ "$TAG" = "$expected" ] || {
        printf '\033[1;33m!\033[0m tag %s does not match CMakeLists VERSION %s — bump CMakeLists first\n' "$TAG" "$VERSION" >&2
        exit 1
    }
fi

# ---- ui ----------------------------------------------------------------------
hr()   { printf '\n\033[1;34m== %s ==\033[0m\n' "$*"; }
info() { printf '\033[1;34m::\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!\033[0m  %s\n' "$*"; }
ok()   { printf '\033[1;32m\xe2\x9c\x93\033[0m %s\n' "$*"; }
skip() { printf '\033[1;33mskip\033[0m %s\n' "$*"; }

hr "agentty release builder — version $VERSION (host: $HOST_OS)"
if [ "$HOST_OS" = unknown ]; then
    warn "unrecognized host OS '$uname_s' — don't know what to build, exiting"
    exit 1
fi

# ---- 0. release shell (so per-binary uploads can stream as builds finish) ---
# Create or reuse the GitHub release up-front when --tag is set; that way each
# arch's musl build can upload its binary the instant it finishes, instead of
# everything waiting for the slowest build at the end.
release_ready=0
if [ "$UPLOAD" -eq 1 ]; then
    if ! command -v gh >/dev/null 2>&1; then
        warn "gh not installed — disabling streaming upload (will build only)"
        UPLOAD=0
    elif gh release view "$TAG" --repo 1ay1/agentty >/dev/null 2>&1; then
        info "release $TAG exists — reusing for streaming uploads"
        release_ready=1
    else
        info "creating empty release $TAG (assets stream in as builds finish)"
        gh release create "$TAG" --repo 1ay1/agentty \
            --title "agentty $TAG" \
            --notes "See [CHANGELOG.md](https://github.com/1ay1/agentty/blob/master/CHANGELOG.md)." \
            >/dev/null
        ok "release $TAG created"
        release_ready=1
    fi
fi

# Upload a single asset to the in-flight release. Quiet on success, warns on
# failure but does not abort the script — failed assets are retried in the
# final upload sweep at the end.
upload_one() {
    [ "$release_ready" -eq 1 ] || return 0
    asset=$1
    name=$(basename "$asset")
    size=$(ls -lh "$asset" | awk '{print $5}')
    if gh release upload "$TAG" --repo 1ay1/agentty --clobber "$asset" >/dev/null 2>&1; then
        ok "uploaded $name ($size)"
    else
        warn "upload failed for $name — will retry in final sweep"
    fi
}

# Build + upload every per-arch downstream package the moment the Linux musl
# binary for that arch is on disk. Called from inside each arch's build
# subshell, so x86_64's deb/rpm/arch-pkg start uploading while aarch64 is
# still compiling (and vice-versa). Two arches × three packagers run
# concurrently with no extra wiring.
#
# Args: $1 = arch (x86_64 | aarch64)
build_linux_packages_for_arch() {
    pkg_arch=$1
    bin="$dist/agentty-linux-$pkg_arch"
    [ -x "$bin" ] || { warn "no binary for $pkg_arch — skipping packages"; return 1; }

    # .deb
    case "$pkg_arch" in
        x86_64)  deb_arch=amd64 ;;
        aarch64) deb_arch=arm64 ;;
    esac
    if sh "$root/packaging/deb/build.sh" "$VERSION" "$deb_arch" "$bin" "$dist" \
            > "$dist/.deb-$pkg_arch.log" 2>&1; then
        deb="$dist/agentty_${VERSION}_${deb_arch}.deb"
        [ -f "$deb" ] && upload_one "$deb"
    else
        warn ".deb $deb_arch failed — see $dist/.deb-$pkg_arch.log"
    fi

    # .rpm
    if command -v rpmbuild >/dev/null 2>&1; then
        if sh "$root/packaging/rpm/build.sh" "$VERSION" "$pkg_arch" "$bin" "$dist" \
                > "$dist/.rpm-$pkg_arch.log" 2>&1; then
            rpm="$dist/agentty-${VERSION}-1.${pkg_arch}.rpm"
            [ -f "$rpm" ] && upload_one "$rpm"
        else
            warn ".rpm $pkg_arch failed — see $dist/.rpm-$pkg_arch.log"
        fi
    else
        skip ".rpm $pkg_arch (rpmbuild not installed)"
    fi

    # arch .pkg.tar.zst (via docker + qemu for cross-arch)
    if command -v docker >/dev/null 2>&1; then
        case "$pkg_arch" in
            x86_64)  docker_arch=amd64; image=archlinux:base-devel ;;
            aarch64) docker_arch=arm64; image=menci/archlinuxarm:base-devel ;;
        esac
        work=$(mktemp -d)
        sed -e "s/^pkgver=.*/pkgver=$VERSION/" \
            -e "s|source_${pkg_arch}=.*|source_${pkg_arch}=(\"agentty-${VERSION}-${pkg_arch}::file:///pkg/agentty-linux-${pkg_arch}\")|" \
            -e "s/sha256sums_${pkg_arch}=.*/sha256sums_${pkg_arch}=('SKIP')/" \
            "$root/packaging/arch/PKGBUILD" > "$work/PKGBUILD"
        cp "$bin" "$work/agentty-linux-$pkg_arch"
        if docker run --rm \
                --platform "linux/$docker_arch" \
                -v "$work":/pkg -w /pkg \
                -e VERSION="$VERSION" -e CARCH="$pkg_arch" \
                "$image" sh -c '
                    useradd -m -u 1000 build 2>/dev/null || true
                    chown -R build:build /pkg
                    su build -c "cd /pkg && PKGDEST=/pkg makepkg -f --skipinteg --noconfirm"
                ' > "$dist/.makepkg-$pkg_arch.log" 2>&1; then
            for pkg in "$work"/agentty-bin-*-${pkg_arch}.pkg.tar.zst; do
                case "$pkg" in *-debug-*) continue ;; esac
                if [ -f "$pkg" ]; then
                    cp "$pkg" "$dist/"
                    upload_one "$dist/$(basename "$pkg")"
                fi
            done
        else
            warn "makepkg failed for $pkg_arch — see $dist/.makepkg-$pkg_arch.log"
        fi
        rm -rf "$work"
    else
        skip "arch pkg $pkg_arch (docker not installed)"
    fi
}

# ---- 1. binaries (host-OS-specific) -----------------------------------------
# Each branch builds, copies into dist/agentty-<os>-<arch>(.exe), and streams
# the resulting binary to the release with upload_one.
if [ "$HOST_OS" = linux ]; then

# ---- 1a. linux static binaries via docker ------------------------------------
hr "linux static binaries (parallel + streamed upload)"
if [ "$SKIP_BINARIES" -eq 1 ]; then
    skip "binaries (--skip-binaries)"
    for a in x86_64 aarch64; do
        [ -x "$dist/agentty-linux-$a" ] || {
            warn "missing $dist/agentty-linux-$a — run without --skip-binaries"
            exit 1
        }
        upload_one "$dist/agentty-linux-$a"
    done
elif command -v docker >/dev/null 2>&1; then
    # Build both arches concurrently in the background. Each subshell:
    #   docker musl build → strip → copy into dist/ → upload binary →
    #   build_linux_packages_for_arch (deb + rpm + arch pkg, each uploads
    #   the instant it's built).
    # The two arches' package pipelines overlap freely, so an x86_64 deb is
    # on the release page while aarch64 is still compiling.
    #
    # Parallelism budget: total jobs across BOTH concurrent arch builds is
    # capped at host nproc — each docker gets nproc/2 (min 1). Faster than
    # serial-with-full-nproc because the two builds overlap (apk install +
    # CMake configure on one arch while the other is compiling), but doesn't
    # oversubscribe the box.
    per_arch_jobs=$(( $(nproc) / 2 ))
    [ "$per_arch_jobs" -lt 1 ] && per_arch_jobs=1
    info "parallelism: 2 arches × -j$per_arch_jobs (host nproc=$(nproc))"
    pids=""
    failed_archs=""
    for arch_pair in 'x86_64 amd64' 'aarch64 arm64'; do
        # shellcheck disable=SC2086
        set -- $arch_pair
        arch=$1; docker_arch=$2
        info "spawning agentty-linux-$arch build (alpine + musl, $docker_arch, -j$per_arch_jobs)"
        (
            builddir="build-release-static-$arch"
            docker run --rm \
                --platform "linux/$docker_arch" \
                -v "$root":/src -w /src \
                -e BUILDDIR="$builddir" \
                -e JOBS="$per_arch_jobs" \
                alpine:3.21 sh -c '
                    apk add --no-cache build-base cmake ninja git openssl-dev openssl-libs-static \
                                       nghttp2-dev nghttp2-static zlib-static linux-headers \
                                       ca-certificates pkgconfig >/dev/null
                    rm -rf "$BUILDDIR"
                    cmake -S . -B "$BUILDDIR" -GNinja \
                          -DCMAKE_BUILD_TYPE=Release \
                          -DAGENTTY_STANDALONE=ON \
                          -DAGENTTY_FULLY_STATIC=ON >/dev/null
                    cmake --build "$BUILDDIR" -j"$JOBS"
                    strip "$BUILDDIR/agentty"
                ' > "$dist/.docker-$arch.log" 2>&1 || exit 1
            cp "$root/$builddir/agentty" "$dist/agentty-linux-$arch"
            chmod +x "$dist/agentty-linux-$arch"
            ok "built agentty-linux-$arch ($(du -h "$dist/agentty-linux-$arch" | awk '{print $1}'))"
            upload_one "$dist/agentty-linux-$arch"
            # Immediately kick off everything derived from this binary:
            # deb, rpm, arch pkg — each uploads as soon as it's built.
            build_linux_packages_for_arch "$arch"
        ) &
        pids="$pids $!:$arch"
    done
    for entry in $pids; do
        pid=${entry%%:*}
        arch=${entry##*:}
        if ! wait "$pid"; then
            warn "static build failed for $arch — see $dist/.docker-$arch.log"
            failed_archs="$failed_archs $arch"
        fi
    done
    [ -z "$failed_archs" ] || warn "failed arches:$failed_archs (continuing with what built)"
else
    warn "docker not available — copying existing release binaries from repo root"
    for a in x86_64 aarch64; do
        if [ -x "$root/agentty-linux-$a" ]; then
            cp "$root/agentty-linux-$a" "$dist/"
            ok "agentty-linux-$a (copied from repo root)"
            upload_one "$dist/agentty-linux-$a"
            build_linux_packages_for_arch "$a"
        else
            warn "no agentty-linux-$a anywhere — install docker or pre-build"
        fi
    done
fi

# Windows .exe is no longer harvested from the Linux host — the Windows branch
# below builds it natively when this script runs on a Windows host.

elif [ "$HOST_OS" = macos ]; then

# ---- 1b. macos native binaries ----------------------------------------------
hr "macos native binaries"
if [ "$SKIP_BINARIES" -eq 1 ]; then
    skip "binaries (--skip-binaries)"
    for a in x86_64 arm64; do
        [ -x "$dist/agentty-macos-$a" ] && upload_one "$dist/agentty-macos-$a"
    done
else
    # Native macOS build via host toolchain. We try both x86_64 and arm64 by
    # asking cmake to set CMAKE_OSX_ARCHITECTURES; on Apple Silicon the x86_64
    # build needs Rosetta SDKs, on Intel the arm64 build needs a cross-capable
    # toolchain. Whichever fails just gets skipped — the other still ships.
    host_arch=$(uname -m)
    jobs=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)
    for arch in x86_64 arm64; do
        info "building agentty-macos-$arch (native, -j$jobs)"
        builddir="build-release-macos-$arch"
        rm -rf "$root/$builddir"
        if cmake -S "$root" -B "$root/$builddir" -GNinja \
                 -DCMAKE_BUILD_TYPE=Release \
                 -DCMAKE_OSX_ARCHITECTURES="$arch" \
                 -DAGENTTY_STANDALONE=ON > "$dist/.macos-$arch.log" 2>&1 \
           && cmake --build "$root/$builddir" -j"$jobs" >> "$dist/.macos-$arch.log" 2>&1; then
            strip -x "$root/$builddir/agentty" 2>/dev/null || true
            cp "$root/$builddir/agentty" "$dist/agentty-macos-$arch"
            chmod +x "$dist/agentty-macos-$arch"
            ok "agentty-macos-$arch ($(du -h "$dist/agentty-macos-$arch" | awk '{print $1}'))"
            upload_one "$dist/agentty-macos-$arch"
        else
            warn "macos $arch build failed (likely cross-arch on host=$host_arch) — see $dist/.macos-$arch.log"
        fi
    done
fi

elif [ "$HOST_OS" = windows ]; then

# ---- 1c. windows binary -----------------------------------------------------
hr "windows binary"
if [ "$SKIP_BINARIES" -eq 1 ]; then
    skip "binaries (--skip-binaries)"
    [ -f "$dist/agentty-windows-x86_64.exe" ] && upload_one "$dist/agentty-windows-x86_64.exe"
else
    info "building agentty-windows-x86_64.exe (native MSVC + vcpkg static)"
    builddir="build-release-win-x86_64"
    rm -rf "$root/$builddir"
    # Honor VCPKG_ROOT if the user has vcpkg configured; CMakeLists already
    # knows to pull from the x64-windows-static triplet under AGENTTY_STANDALONE.
    toolchain_arg=""
    if [ -n "${VCPKG_ROOT:-}" ] && [ -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]; then
        toolchain_arg="-DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
        info "using vcpkg toolchain at $VCPKG_ROOT"
    fi
    if cmake -S "$root" -B "$root/$builddir" \
             -DCMAKE_BUILD_TYPE=Release \
             -DAGENTTY_STANDALONE=ON \
             $toolchain_arg > "$dist/.win-x86_64.log" 2>&1 \
       && cmake --build "$root/$builddir" --config Release -j >> "$dist/.win-x86_64.log" 2>&1; then
        # MSVC puts the .exe under Release/ for multi-config generators.
        for candidate in \
            "$root/$builddir/Release/agentty.exe" \
            "$root/$builddir/agentty.exe"; do
            if [ -f "$candidate" ]; then
                cp "$candidate" "$dist/agentty-windows-x86_64.exe"
                ok "agentty-windows-x86_64.exe ($(du -h "$dist/agentty-windows-x86_64.exe" | awk '{print $1}'))"
                upload_one "$dist/agentty-windows-x86_64.exe"
                break
            fi
        done
    else
        warn "windows build failed — see $dist/.win-x86_64.log"
    fi
fi

fi  # end host-OS-specific binary builds

# ---- 2. source tarball (linux + macos only) ---------------------------------
# Source tarball isn't OS-specific in content, but Homebrew is its only
# downstream consumer, so we generate it on unix hosts only — keeps the
# Windows path off the tarball/SHA256SUMS write paths.
if [ "$HOST_OS" = linux ] || [ "$HOST_OS" = macos ]; then
hr "source tarball"
tarball="agentty-$VERSION.tar.gz"
# git archive alone OMITS submodule contents (mcp-cpp/), which breaks any
# downstream that BUILDS FROM SOURCE (Termux build.sh, distro from-source
# packages). Fold each submodule's own `git archive` into the same prefixed
# tree so the tarball is self-contained. Homebrew (prebuilt-binary consumer)
# is unaffected; from-source consumers now work.
_srctmp=$(mktemp -d)
git -C "$root" archive --format=tar --prefix="agentty-$VERSION/" HEAD \
    -o "$_srctmp/base.tar"
tar -C "$_srctmp" -xf "$_srctmp/base.tar"
git -C "$root" submodule --quiet foreach '
    git archive --format=tar --prefix="agentty-'"$VERSION"'/$sm_path/" HEAD \
        | tar -C "'"$_srctmp"'" -xf -
' 2>/dev/null || warn "submodule archive step failed — tarball may lack submodules"
tar -C "$_srctmp" -czf "$dist/$tarball" "agentty-$VERSION"
rm -rf "$_srctmp"
ok "$tarball ($(du -h "$dist/$tarball" | awk '{print $1}'), submodules bundled)"
upload_one "$dist/$tarball"
fi

# ---- 3-5. linux packages — done in-flight per arch -------------------------
# .deb / .rpm / .pkg.tar.zst for each Linux arch are built by
# build_linux_packages_for_arch() inside that arch's binary-build subshell
# above, so they upload concurrently with the OTHER arch's compile. The
# explicit sequential loops that used to live here are no longer needed.

# ---- 6. checksums + downstream manifests (host-OS-scoped) -------------------
# Only checksum the files THIS host produced. Each platform writes its own
# SHA256SUMS — the last `release.sh --tag` run for a given OS wins on the
# release page, but never deletes the SHA256SUMS that another host uploaded
# earlier (gh release upload --clobber overwrites only the same-name asset).
# If you want a single unified SHA256SUMS, merge by hand after every host has
# run.
hr "checksums + packaging manifests ($HOST_OS)"
case "$HOST_OS" in
    linux)
        ( cd "$dist" && sha256sum \
            agentty-linux-x86_64 \
            agentty-linux-aarch64 \
            agentty-*.tar.gz \
            agentty_*_amd64.deb \
            agentty_*_arm64.deb \
            agentty-*-1.x86_64.rpm \
            agentty-*-1.aarch64.rpm \
            agentty-*-x86_64.pkg.tar.zst \
            agentty-*-aarch64.pkg.tar.zst \
            2>/dev/null | sort -k2 > SHA256SUMS )
        ;;
    macos)
        # macOS ships shasum, not sha256sum.
        ( cd "$dist" && shasum -a 256 \
            agentty-macos-x86_64 \
            agentty-macos-arm64 \
            agentty-*.tar.gz \
            2>/dev/null | sort -k2 > SHA256SUMS )
        ;;
    windows)
        # Git-bash ships sha256sum via coreutils-mingw.
        ( cd "$dist" && sha256sum \
            agentty-windows-x86_64.exe \
            2>/dev/null | sort -k2 > SHA256SUMS )
        ;;
esac
ok "SHA256SUMS"

sum_for() { awk -v f="$1" '$2 == f { print $1 }' "$dist/SHA256SUMS"; }

linux_x64=$(sum_for "agentty-linux-x86_64")
linux_arm=$(sum_for "agentty-linux-aarch64")
win_x64=$(sum_for "agentty-windows-x86_64.exe")
src_tar=$(sum_for "agentty-$VERSION.tar.gz")
mac_x64=$(sum_for "agentty-macos-x86_64")
mac_arm=$(sum_for "agentty-macos-arm64")

# The macOS binaries are built on macOS runners, so a Linux-host release run
# won't have them in dist/. Pull their SHAs from the published release's
# SHA256SUMS so the formula (generated on the Linux host) can pin them.
if [ "$HOST_OS" = linux ] && { [ -z "$mac_x64" ] || [ -z "$mac_arm" ]; }; then
    remote_sums=$(gh release view "$TAG" --repo 1ay1/agentty \
        --json assets -q '.assets[].name' 2>/dev/null | grep -qx SHA256SUMS \
        && gh release download "$TAG" --repo 1ay1/agentty \
             --pattern SHA256SUMS --output - 2>/dev/null)
    [ -z "$mac_x64" ] && mac_x64=$(printf '%s\n' "$remote_sums" | awk '$2=="agentty-macos-x86_64"{print $1}')
    [ -z "$mac_arm" ] && mac_arm=$(printf '%s\n' "$remote_sums" | awk '$2=="agentty-macos-arm64"{print $1}')
fi

# homebrew — only regenerate on the linux host. Pins prebuilt static binaries
# for both Linux and macOS (no source build; agentty needs C++26/GCC).
if [ "$HOST_OS" = linux ] && [ -n "$linux_x64" ] && [ -n "$linux_arm" ] \
     && [ -n "$mac_x64" ] && [ -n "$mac_arm" ]; then
    sed -e "s/^  version \".*\"/  version \"$VERSION\"/" \
        -e "s|@LINUX_X86_64_SHA256@|$linux_x64|g" \
        -e "s|@LINUX_AARCH64_SHA256@|$linux_arm|g" \
        -e "s|@MACOS_X86_64_SHA256@|$mac_x64|g" \
        -e "s|@MACOS_ARM64_SHA256@|$mac_arm|g" \
        "$root/packaging/homebrew/agentty.rb" > "$pkgdir/agentty.rb"
    ok "packaging/agentty.rb (homebrew)"
fi

# scoop — only regenerate on a Windows host where we just built the .exe.
if [ "$HOST_OS" = windows ] && [ -n "$win_x64" ]; then
    sed -e "s/\"version\": \".*\"/\"version\": \"$VERSION\"/" \
        -e "s|/download/v[0-9.]*/|/download/v$VERSION/|g" \
        -e "s|@WINDOWS_X86_64_SHA256@|$win_x64|g" \
        "$root/packaging/scoop/agentty.json" > "$pkgdir/agentty.json"
    ok "packaging/agentty.json (scoop)"
fi

# PKGBUILD (AUR-publishable, with real sha256s pinned) — linux host only.
if [ "$HOST_OS" = linux ] && [ -n "$linux_x64" ] && [ -n "$linux_arm" ]; then
    sed -e "s/^pkgver=.*/pkgver=$VERSION/" \
        -e "s/sha256sums_x86_64=.*/sha256sums_x86_64=('$linux_x64')/" \
        -e "s/sha256sums_aarch64=.*/sha256sums_aarch64=('$linux_arm')/" \
        "$root/packaging/arch/PKGBUILD" > "$pkgdir/PKGBUILD"
    ok "packaging/PKGBUILD (AUR)"
fi

# Alpine APKBUILD — pkgver rewritten from central VERSION; sha512 pinned from
# the release binaries (abuild wants sha512, so we recompute here).
if [ "$HOST_OS" = linux ] && [ -x "$dist/agentty-linux-x86_64" ] \
     && [ -x "$dist/agentty-linux-aarch64" ] && command -v sha512sum >/dev/null 2>&1; then
    a_x64=$(sha512sum "$dist/agentty-linux-x86_64"  | awk '{print $1}')
    a_arm=$(sha512sum "$dist/agentty-linux-aarch64" | awk '{print $1}')
    sed -e "s/^pkgver=@VERSION@/pkgver=$VERSION/" \
        -e "s|^SKIP  agentty-linux-x86_64|$a_x64  agentty-linux-x86_64|" \
        -e "s|^SKIP  agentty-linux-aarch64|$a_arm  agentty-linux-aarch64|" \
        "$root/packaging/alpine/APKBUILD" > "$pkgdir/APKBUILD"
    ok "packaging/APKBUILD (alpine)"
fi

# Nix expression — version + per-arch sha256 pinned from SHA256SUMS.
if [ "$HOST_OS" = linux ] && [ -n "$linux_x64" ] && [ -n "$linux_arm" ]; then
    sed -e "s/@VERSION@/$VERSION/g" \
        -e "s|@LINUX_X86_64_SHA256@|$linux_x64|g" \
        -e "s|@LINUX_AARCH64_SHA256@|$linux_arm|g" \
        "$root/packaging/nix/default.nix" > "$pkgdir/default.nix"
    ok "packaging/default.nix (nix)"
fi

# Snap manifest — version rewritten from central VERSION.
if [ "$HOST_OS" = linux ]; then
    sed -e "s/@VERSION@/$VERSION/g" \
        "$root/packaging/snap/snapcraft.yaml.in" > "$pkgdir/snapcraft.yaml"
    ok "packaging/snapcraft.yaml (snap)"
fi

# Gentoo ebuild — copy the 9999 template to a version-pinned filename so the
# version lives ONLY in the filename (Portage reads PV from there).
if [ "$HOST_OS" = linux ]; then
    cp "$root/packaging/gentoo/agentty-9999.ebuild" "$pkgdir/agentty-$VERSION.ebuild"
    ok "packaging/agentty-$VERSION.ebuild (gentoo)"
fi

# ---- summary -----------------------------------------------------------------
hr "summary"
ls -lh "$dist" | grep -v '^total' | grep -v '^d' | awk '{print "    " $9 "  " $5}' | sort

# ---- upload (final sweep, host-OS-scoped) -----------------------------------
# Per-binary uploads already streamed as builds finished. This final pass picks
# up everything derived from those binaries — tarball, packages, SHA256SUMS —
# plus retries anything the streaming path failed. The asset glob is scoped to
# THIS host's artifacts so a macOS-host run won't try to push Linux debs left
# in dist/ from a previous Linux-host run (`--clobber` overwrites name-matches
# but never deletes siblings, so cross-OS runs accrete cleanly on one release).
if [ "$UPLOAD" -eq 1 ]; then
    hr "final upload sweep for $TAG ($HOST_OS)"
    case "$HOST_OS" in
        linux)
            assets=$(ls \
                "$dist"/agentty-linux-* \
                "$dist"/agentty-bin-*-x86_64.pkg.tar.zst \
                "$dist"/agentty-bin-*-aarch64.pkg.tar.zst \
                "$dist"/agentty-*-1.x86_64.rpm \
                "$dist"/agentty-*-1.aarch64.rpm \
                "$dist"/agentty-$VERSION.tar.gz \
                "$dist"/agentty_*.deb \
                "$dist"/SHA256SUMS \
                2>/dev/null | sort -u)
            ;;
        macos)
            assets=$(ls \
                "$dist"/agentty-macos-* \
                "$dist"/agentty-$VERSION.tar.gz \
                "$dist"/SHA256SUMS \
                2>/dev/null | sort -u)
            ;;
        windows)
            assets=$(ls \
                "$dist"/agentty-windows-* \
                "$dist"/SHA256SUMS \
                2>/dev/null | sort -u)
            ;;
    esac
    total=$(printf '%s\n' "$assets" | grep -c .)
    i=0
    failed=0
    for a in $assets; do
        i=$((i + 1))
        name=$(basename "$a")
        size=$(ls -lh "$a" | awk '{print $5}')
        info "[$i/$total] $name ($size)"
        if gh release upload "$TAG" --repo 1ay1/agentty --clobber "$a" >/dev/null 2>&1; then
            ok "$name"
        else
            warn "$name (upload failed)"
            failed=$((failed + 1))
        fi
    done
    if [ "$failed" -gt 0 ]; then
        warn "$failed/$total asset(s) failed to upload"
        exit 1
    fi
    ok "all $total assets uploaded"
fi

hr "done"
