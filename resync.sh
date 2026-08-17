#!/usr/bin/env sh
# resync.sh — pull every in-tree submodule to its tracking branch, then rebuild.
#
# Normal builds deliberately DO NOT touch the submodules (so an unchanged maya /
# mcp-cpp / rag-cpp / acp-cpp never triggers a needless recompile of the ~64 TUs
# that include their headers). Run THIS when you actually want the latest
# submodule code:
#
#   ./resync.sh              # sync all submodules + incremental rebuild
#   ./resync.sh -B build-rel # use a specific build dir (default: build)
#
# It is a thin wrapper over two CMake targets:
#   submodules_sync   — fast-forwards every submodule (skips any with local work)
#   all               — the normal incremental build; only TUs whose headers
#                       actually changed are recompiled.
set -eu

BUILD_DIR=build
JOBS=${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}

while [ $# -gt 0 ]; do
  case "$1" in
    -B) BUILD_DIR=$2; shift 2 ;;
    -j) JOBS=$2;      shift 2 ;;
    -h|--help)
      sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "resync.sh: unknown arg '$1'" >&2; exit 2 ;;
  esac
done

if [ ! -d "$BUILD_DIR" ]; then
  echo "resync.sh: build dir '$BUILD_DIR' not found — configure it first, e.g.:" >&2
  echo "  cmake -S . -B $BUILD_DIR -GNinja -DCMAKE_BUILD_TYPE=Release" >&2
  exit 1
fi

echo ">> syncing submodules (maya, mcp-cpp, acp-cpp, rag-cpp) …"
cmake --build "$BUILD_DIR" --target submodules_sync

echo ">> incremental rebuild (-j$JOBS) …"
cmake --build "$BUILD_DIR" -j"$JOBS"

echo ">> done."
