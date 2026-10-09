// progress.cpp — the tool progress sink (tools::progress).
//
// Split out of registry.cpp so consumers that only need the sink (e.g. the
// subprocess runner in util/subprocess.cpp) can link it WITHOUT dragging in
// build_registry() and, through it, the whole MCP bridge / tool set. This
// keeps minimal test targets (keystore_test) and any lean subprocess-only
// TU cheap to link.
//
// thread_local so the cmd runner's dispatch lambda can be captured without
// cross-thread synchronisation — each tool runs on its own worker, and
// cmd_factory installs/clears the sink on that worker via a RAII Scope.
// Subprocess runners call progress::emit from the same thread, so it's a
// plain load from TLS — no atomics, no locking.

#include "agentty/tool/registry.hpp"

#include <functional>
#include <memory>
#include <stop_token>
#include <utility>
#include <vector>

#include <maya/runtime.hpp>

namespace agentty::tools {
namespace progress {
namespace {
    thread_local Sink g_sink;
}
void set(Sink s)                       { g_sink = std::move(s); }
void clear()                           { g_sink = nullptr; }
void emit(std::string_view snapshot)   { if (g_sink) g_sink(snapshot); }
Sink current()                         { return g_sink; }
} // namespace progress

namespace reader {
namespace {
    thread_local std::string g_reader;
}
void set(std::string id)       { g_reader = std::move(id); }
const std::string& current()   { return g_reader; }
} // namespace reader

namespace cancellation {
namespace {
    thread_local Probe                        g_probe;
    thread_local std::vector<std::stop_token> g_tokens;
}
void set(Probe probe) { g_probe = std::move(probe); g_tokens.clear(); }
void clear() { g_probe = nullptr; g_tokens.clear(); }
Probe current() { return g_probe; }
bool requested() { return g_probe && g_probe(); }
std::vector<std::stop_token> tokens() { return g_tokens; }

bool wait(std::chrono::milliseconds d, std::stop_token also) {
    // One source that any of them can trip, so a single delay_for covers all.
    std::stop_source merged;
    using Cb = std::stop_callback<std::function<void()>>;
    std::vector<std::unique_ptr<Cb>> cbs;
    const std::function<void()> trip = [&merged] { merged.request_stop(); };
    for (const auto& t : g_tokens) cbs.push_back(std::make_unique<Cb>(t, trip));
    if (also.stop_possible()) cbs.push_back(std::make_unique<Cb>(also, trip));
    if (maya::delay_for(merged.get_token(), d)) return true;
    return requested();
}

Scope::Scope(std::vector<std::stop_token> toks) {
    // The probe is DERIVED from the tokens, so a poller and a subscriber can
    // never disagree about whether the run was cancelled.
    set([toks] {
        for (const auto& t : toks)
            if (t.stop_requested()) return true;
        return false;
    });
    g_tokens = std::move(toks);
}
} // namespace cancellation
} // namespace agentty::tools
