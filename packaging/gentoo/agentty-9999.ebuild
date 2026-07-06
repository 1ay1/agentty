# Copyright 2024 Gentoo Authors
# Distributed under the terms of the MIT License

# packaging/gentoo/agentty-9999.ebuild
#
# This filename is a TEMPLATE. scripts/release.sh copies it to
# agentty-@VERSION@.ebuild (rewriting @VERSION@ from CMakeLists.txt project
# VERSION and pinning per-arch SHA512 into Manifest via `ebuild ... manifest`).
# Do not hardcode the version anywhere but the ebuild filename.
#
# Install (once in an overlay):
#   emerge agentty

EAPI=8

DESCRIPTION="Blazing-fast Claude in your terminal — single static binary, sandboxed, airgap-capable"
HOMEPAGE="https://github.com/1ay1/agentty"

# agentty publishes a fully-static musl binary per arch; we install that rather
# than building C++26 from source (needs a very recent GCC).
SRC_URI="
	amd64? ( https://github.com/1ay1/agentty/releases/download/v${PV}/agentty-linux-x86_64 -> ${P}-x86_64 )
	arm64? ( https://github.com/1ay1/agentty/releases/download/v${PV}/agentty-linux-aarch64 -> ${P}-aarch64 )
"

LICENSE="MIT"
SLOT="0"
KEYWORDS="-* ~amd64 ~arm64"

# Optional runtime helpers.
RDEPEND="
	sys-apps/bubblewrap
	net-misc/openssh
"

# Prebuilt binary: no compile, no strip (already stripped upstream).
RESTRICT="strip"
QA_PREBUILT="usr/bin/agentty"

S="${WORKDIR}"

src_unpack() {
	# SRC_URI files are the binary itself; copy the arch-appropriate one in.
	if use amd64; then
		cp "${DISTDIR}/${P}-x86_64" "${S}/agentty" || die
	elif use arm64; then
		cp "${DISTDIR}/${P}-aarch64" "${S}/agentty" || die
	else
		die "unsupported arch"
	fi
}

src_install() {
	dobin "${S}/agentty"
}
