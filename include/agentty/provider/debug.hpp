#pragma once
// agentty::provider — env-gated API debug dump, shared by EVERY transport.
//
// Set AGENTTY_DEBUG_API=1 to append a request/stream trace to
// $AGENTTY_DEBUG_FILE (or ./agentty-api.log). Never truncates; safe to
// leave enabled across runs. Each line is stamped with monotonic
// ms-since-first-log so event pacing can be read without wall clocks.
//
// This started life inside anthropic/sse.hpp, which meant ONLY the
// Anthropic wire produced a dump — debugging a custom OpenAI-compatible
// host (the case that needs the trace most: llama.cpp/vLLM quirks) wrote
// nothing. Hoisted here so all transports share one logger and one file.
//
// Hot-path cost when disabled: one guard-byte test + cached-nullptr load.

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <string>

#include "agentty/util/env.hpp"

namespace agentty::provider {

inline FILE* debug_log() {
    // Function-local static: thread-safe one-time init, then a plain load.
    static FILE* fp = [] () -> FILE* {
        const char* on = util::env::get_or_null<util::env::Var::DebugApi>();
        if (!on || *on == '0') return nullptr;
        const char* path = util::env::get_or_null<util::env::Var::DebugFile>();
        std::string p = (path && *path) ? std::string{path}
                                        : std::string{"agentty-api.log"};
        return std::fopen(p.c_str(), "ab");
    }();
    return fp;
}

inline void dbg(const char* fmt, ...) {
    FILE* fp = debug_log();
    if (!fp) return;
    using clock = std::chrono::steady_clock;
    static const auto t0 = clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  clock::now() - t0).count();
    std::fprintf(fp, "[+%6lldms] ", static_cast<long long>(ms));
    va_list ap; va_start(ap, fmt);
    std::vfprintf(fp, fmt, ap);
    va_end(ap);
    std::fflush(fp);
}

} // namespace agentty::provider
