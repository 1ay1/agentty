// cmake/agentty_pch.hpp — precompiled-header prefix for the shared object
// libraries. These are the headers included by 30-60% of agentty's TUs
// (measured), heavily weighted toward the expensive-to-parse STL templates
// (<algorithm>, <chrono>, <filesystem>, <regex>-adjacent). Precompiling them
// once and reusing the IR across every TU is the highest-leverage compile-time
// win for this header/template-heavy tree.
//
// RULES:
//   * ONLY stable, ubiquitous, header-only STL (+ the always-present maya
//     core). Nothing project-specific that changes often — a PCH edit
//     invalidates every dependent TU, so churny headers here would DESTROY
//     the incremental-build win.
//   * No <mimalloc.h>, no <mcp/*>, no <rag/*>, no <nlohmann/*> — those are
//     conditional (compile-gated) and not universal; keep them out so the PCH
//     is identical regardless of feature flags.
//   * Applied via target_precompile_headers on the first objlib, REUSE_FROM'd
//     by the rest, so it is compiled exactly once.
#pragma once

// ── Only the EXPENSIVE-to-parse, ubiquitous headers ─────────────────────────
// A big PCH (21 MB with the full STL prefix) LOSES on a fast many-core box:
// the per-TU load cost + lost parallelism exceed the parse savings (measured).
// Keep the prefix lean — just the templates that are both widely included AND
// genuinely slow to parse — so the PCH stays small and can actually win on CI
// / cold / low-core builds.
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>
