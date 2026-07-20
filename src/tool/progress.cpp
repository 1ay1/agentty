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

#include <utility>

namespace agentty::tools {
namespace progress {
namespace {
    thread_local Sink g_sink;
}
void set(Sink s)                       { g_sink = std::move(s); }
void clear()                           { g_sink = nullptr; }
void emit(std::string_view snapshot)   { if (g_sink) g_sink(snapshot); }
} // namespace progress
} // namespace agentty::tools
