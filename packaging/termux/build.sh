TERMUX_PKG_HOMEPAGE=https://github.com/1ay1/agentty
TERMUX_PKG_DESCRIPTION="AI pair programming in your terminal — one static binary, any model"
TERMUX_PKG_LICENSE="MIT"
TERMUX_PKG_LICENSE_FILE="LICENSE"
TERMUX_PKG_MAINTAINER="@1ay1"
TERMUX_PKG_VERSION="0.2.8"
# agentty pulls maya / acp-cpp / mcp-cpp as git submodules, and its CMake
# FetchContent's nlohmann-json + simdjson at configure time. A GitHub source
# TARBALL contains none of that, so we build from a recursive git clone
# instead of TERMUX_PKG_SRCURL.
TERMUX_PKG_SRCURL=git+https://github.com/1ay1/agentty
TERMUX_PKG_GIT_BRANCH=v${TERMUX_PKG_VERSION}
TERMUX_PKG_AUTO_UPDATE=true

# Runtime + build dependencies available in the Termux repos. mimalloc is
# NOT packaged for Termux — agentty's CMake silently disables it when absent,
# and we pass -DAGENTTY_USE_MIMALLOC=OFF below to be explicit.
TERMUX_PKG_DEPENDS="openssl, libnghttp2, libc++"
TERMUX_PKG_BUILD_DEPENDS="nlohmann-json, simdjson"

# agentty is C++26 (needs Termux's clang, which supports it) and links its
# own copy of maya/acp/mcp statically. The resulting binary is a normal
# dynamically-linked Termux executable (PIE) — NOT the fully-static release
# artifact — so it plays nicely with the Termux linker on Android 14+.
TERMUX_PKG_EXTRA_CONFIGURE_ARGS="
-DCMAKE_BUILD_TYPE=Release
-DAGENTTY_AUTO_PULL_MAYA=OFF
-DAGENTTY_USE_MIMALLOC=OFF
-DAGENTTY_STANDALONE=OFF
-DAGENTTY_BUILD_TESTS=OFF
"

termux_step_post_get_source() {
	# The GIT_BRANCH clone above is shallow and does NOT recurse submodules.
	# Pull maya / acp-cpp / mcp-cpp so the build tree is complete. This runs
	# in TERMUX_PKG_SRCDIR before configure; network is allowed at this step.
	git -C "$TERMUX_PKG_SRCDIR" submodule update --init --recursive --depth 1
}

termux_step_make_install() {
	install -Dm700 "$TERMUX_PKG_BUILDDIR/agentty" \
		"$TERMUX_PREFIX/bin/agentty"
}
