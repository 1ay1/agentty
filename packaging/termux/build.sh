TERMUX_PKG_HOMEPAGE=https://github.com/1ay1/agentty
TERMUX_PKG_DESCRIPTION="AI pair programming in your terminal — one binary, any model"
TERMUX_PKG_LICENSE="MIT"
TERMUX_PKG_LICENSE_FILE="LICENSE"
TERMUX_PKG_MAINTAINER="@1ay1"
TERMUX_PKG_VERSION="0.9.1"
# agentty pulls maya / acp-cpp / mcp-cpp / rag-cpp as git submodules, and its
# CMake FetchContent's nlohmann-json + simdjson + mimalloc at configure time.
# A GitHub source TARBALL contains none of that, so build from a git clone
# rather than TERMUX_PKG_SRCURL pointing at archive/refs/tags.
TERMUX_PKG_SRCURL=git+https://github.com/1ay1/agentty
TERMUX_PKG_GIT_BRANCH=v${TERMUX_PKG_VERSION}
TERMUX_PKG_AUTO_UPDATE=true

# Runtime + build dependencies from the Termux repos. Both of the
# FetchContent'd libraries declare FIND_PACKAGE_ARGS, so having these
# installed means CMake finds them instead of cloning at configure time —
# which is what keeps this buildable in a network-restricted sandbox.
TERMUX_PKG_DEPENDS="openssl, libnghttp2, libc++"
TERMUX_PKG_BUILD_DEPENDS="nlohmann-json, simdjson"

# agentty is C++26. Termux's clang handles it, but its libc++ lacks
# std::atomic<std::shared_ptr<T>> (P0718R2), which GCC/libstdc++ has had
# since 12 — util/snapshot.hpp falls back to a mutex behind the same API,
# gated on __cpp_lib_atomic_shared_ptr rather than on the compiler.
#
# AGENTTY_USE_MIMALLOC=OFF matters for packaging specifically: mimalloc is
# the one dependency with no Termux package and no find_package fallback,
# so leaving it on would mean a configure-time clone. Bionic's allocator is
# fine here; the mimalloc win is a desktop-workload optimisation.
#
# Link normally (NOT the fully-static release artifact): a standard
# dynamically-linked PIE is what the Termux linker wants on Android 14+.
TERMUX_PKG_EXTRA_CONFIGURE_ARGS="
-DCMAKE_BUILD_TYPE=Release
-DAGENTTY_AUTO_PULL_SUBMODULES=OFF
-DAGENTTY_STANDALONE=OFF
-DAGENTTY_BUILD_TESTS=OFF
-DAGENTTY_COMPILER_CACHE=OFF
-DAGENTTY_USE_MIMALLOC=OFF
"

termux_step_post_get_source() {
	# The clone above is shallow and does NOT recurse submodules. Pull
	# maya / acp-cpp / mcp-cpp / rag-cpp so the tree is complete — this step
	# runs before configure and network is allowed here.
	git -C "$TERMUX_PKG_SRCDIR" submodule update --init --recursive --depth 1
}

termux_step_make_install() {
	install -Dm755 "$TERMUX_PKG_BUILDDIR/agentty" \
		"$TERMUX_PREFIX/bin/agentty"
}
