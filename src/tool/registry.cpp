#include "agentty/tool/registry.hpp"

#include "agentty/mcp/client.hpp"
#include "agentty/tool/mcp_tools_bridge.hpp"

#include <algorithm>
#include <format>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace agentty::tools {

std::string_view to_string(ErrorKind k) noexcept {
    switch (k) {
        case ErrorKind::InvalidArgs:    return "invalid args";
        case ErrorKind::NotFound:       return "not found";
        case ErrorKind::NotAFile:       return "not a file";
        case ErrorKind::NotADirectory:  return "not a directory";
        case ErrorKind::TooLarge:       return "too large";
        case ErrorKind::Binary:         return "binary";
        case ErrorKind::Ambiguous:      return "ambiguous";
        case ErrorKind::NoMatch:        return "no match";
        case ErrorKind::InvalidRegex:   return "invalid regex";
        case ErrorKind::Network:        return "network";
        case ErrorKind::Spawn:          return "spawn failed";
        case ErrorKind::Subprocess:     return "subprocess failed";
        case ErrorKind::Io:             return "io";
        case ErrorKind::OutOfWorkspace: return "out of workspace";
        case ErrorKind::Unknown:        return "unknown";
    }
    return "unknown";
}

std::string ToolError::render() const {
    return std::format("[{}] {}", to_string(kind), detail);
}

std::string_view to_string(Effect e) noexcept {
    switch (e) {
        case Effect::ReadFs:  return "ReadFs";
        case Effect::WriteFs: return "WriteFs";
        case Effect::Net:     return "Net";
        case Effect::Exec:    return "Exec";
    }
    return "?";
}

std::string to_string(EffectSet e) {
    if (e.empty()) return "Pure";
    std::string out;
    auto add = [&](Effect bit) {
        if (!e.has(bit)) return;
        if (!out.empty()) out += ", ";
        out += to_string(bit);
    };
    add(Effect::Exec);
    add(Effect::WriteFs);
    add(Effect::Net);
    add(Effect::ReadFs);
    return out;
}

// ── Live progress sink (thread-local implementation) ────────────────────
//
// thread_local so the cmd runner's dispatch lambda can be captured without
// cross-thread synchronisation — each tool runs on its own worker, and
// cmd_factory installs/clears the sink on that worker via a RAII Scope.
// Subprocess runners (see util/subprocess.cpp) call progress::emit from the
// same thread, so it's a plain load from TLS — no atomics, no locking.
namespace progress {
namespace {
    thread_local Sink g_sink;
}
void set(Sink s)                       { g_sink = std::move(s); }
void clear()                           { g_sink = nullptr; }
void emit(std::string_view snapshot)   { if (g_sink) g_sink(snapshot); }
}

namespace {

// Assemble every tool. Order matters: the protocol treats the set as
// unordered but the model has a strong recall bias toward earlier-listed
// tools. Putting `edit` ahead of `write` is the cheapest single nudge to
// stop the model from rewriting whole files when a targeted substitution
// would do — and edit's tiny input_json_delta bodies sidestep the long
// mid-stream pause Anthropic's edge applies to multi-KB tool_use content.
// Assemble the local tool set. The implementations live in mcp-cpp's
// batteries-included toolset (mcp::tools::make_provider): build_mcp_tool_defs()
// re-wraps each advertised tool as a ToolDef whose execute() dispatches into
// the provider and decodes the `_mcp_tools` meta (effects + FileChange) back
// into ToolOutput. The host-coupled SHELLS (remember/forget/wipe/todo/skill/
// search_docs/task) are backed by agentty adapters injected via HostServices.
// mcp-cpp is the SOLE source of truth for tools — there is no native path.
std::vector<ToolDef> build_registry() {
    std::vector<ToolDef> r = build_mcp_tool_defs();

    // ── MCP capability providers (essay §2/§10) ──────────────────────────
    // Connect to any configured MCP servers and append their tools as plain
    // ToolDefs — the model can't tell them from local tools. LAZY + OPT-IN:
    // with no .agentty/mcp.json this is a single stat() returning {}, so a
    // user who doesn't use MCP pays nothing at startup. The connection pool
    // (spawned server processes + transports) must outlive every synthesized
    // tool's execute() closure, so it's parked in a function-local static
    // with the same lifetime as the registry itself.
    if (mcp::mcp_config_present()) {
        static mcp::PoolHandle s_pool;       // process-lifetime keep-alive
        auto mcp_tools = mcp::mcp_tools(s_pool);
        for (auto& t : mcp_tools) r.push_back(std::move(t));
    }

    return r;
}

} // namespace

const std::vector<ToolDef>& registry() {
    static const std::vector<ToolDef> r = build_registry();
    return r;
}

// Process-wide name → ToolDef* index, built once on first access.
// Replaces the prior O(N) linear scan in `find()` with a hash-table
// hit. With N=16 the absolute speedup is small (~50 ns vs ~5 ns), but
// the dispatch path runs on every model tool call, every retry,
// every permission prompt — keeping it constant-time is the right
// shape for an agent loop. Stored alongside the registry so both
// share the same lifetime + initialisation order.
//
// The map keys are `std::string` (owning) rather than `string_view`
// to insulate the map from reallocations of the underlying vector.
// In practice the vector never grows after init, but std::string keys
// are the safer default and the lookup cost is identical (heterogeneous
// `find` lets `string_view` callers query without allocating).
namespace {
const std::unordered_map<std::string, const ToolDef*>& index() {
    static const std::unordered_map<std::string, const ToolDef*> m = []{
        const auto& r = registry();
        std::unordered_map<std::string, const ToolDef*> out;
        out.reserve(r.size());
        for (const auto& t : r) out.emplace(t.name.value, &t);
        return out;
    }();
    return m;
}
} // namespace

namespace detail { const ToolDef* find_live_mcp(std::string_view name); }

const ToolDef* find(std::string_view name) {
    const auto& m = index();
    if (auto it = m.find(std::string{name}); it != m.end()) return it->second;
    // Live fallback: an MCP server may have advertised a NEW tool after
    // startup via tools/list_changed, so it isn't in the static index. The
    // wire_tools() snapshot owns stable storage for those; search it.
    if (const auto* td = detail::find_live_mcp(name)) return td;
    return nullptr;
}

namespace {
// Process-wide snapshot of (static registry ∪ live MCP tools), rebuilt only
// when the MCP generation moves (a *_list_changed notification). The vector
// is stable between rebuilds so `find()` can hand out `const ToolDef*` into
// it. Guarded by a mutex; the rebuild is O(static + mcp) and runs at most
// once per turn (callers read mcp_generation() which is O(1)).
struct WireCache {
    std::mutex                       mu;
    unsigned long                    gen     = static_cast<unsigned long>(-1);
    bool                             built   = false;
    bool                             has_live = false;   // cache differs from registry()
    std::vector<ToolDef>             tools;     // owns the merged set
    std::unordered_map<std::string, const ToolDef*> idx;
};
WireCache& wire_cache() { static WireCache c; return c; }

// Rebuild the merged snapshot if MCP's generation moved. Sets c.has_live to
// true iff live MCP tools made the cache differ from the static registry
// (whose MCP tools were captured at startup). Returns c.has_live.
bool refresh_wire_cache_locked(WireCache& c) {
    const unsigned long g = mcp::mcp_generation();
    if (c.built && c.gen == g) return c.has_live;
    c.gen   = g;
    c.built = true;
    c.tools.clear();
    c.idx.clear();
    c.has_live = false;

    // Live MCP set (already namespaced, includes the generic resource/prompt
    // tools). At generation 0 this equals what the static registry already
    // captured, so there's nothing to merge — the registry IS the wire set.
    if (g == 0) return false;
    auto live = mcp::mcp_tools_live();
    if (live.empty()) return false;

    const auto& base = registry();
    std::unordered_map<std::string, std::size_t> live_names;
    for (std::size_t i = 0; i < live.size(); ++i) live_names.emplace(live[i].name.value, i);
    // Carry over static tools; drop any superseded by a live MCP tool of the
    // same name (so a re-listed server replaces, never duplicates).
    for (const auto& t : base) {
        if (live_names.contains(t.name.value)) continue;
        c.tools.push_back(t);
    }
    for (auto& t : live) c.tools.push_back(std::move(t));
    c.idx.reserve(c.tools.size());
    for (const auto& t : c.tools) c.idx.emplace(t.name.value, &t);
    c.has_live = true;
    return true;
}
} // namespace

namespace detail {
const ToolDef* find_live_mcp(std::string_view name) {
    auto& c = wire_cache();
    std::lock_guard<std::mutex> lk(c.mu);
    if (!refresh_wire_cache_locked(c)) return nullptr;
    if (auto it = c.idx.find(std::string{name}); it != c.idx.end()) return it->second;
    return nullptr;
}
} // namespace detail

const std::vector<ToolDef>& wire_tools() {
    auto& c = wire_cache();
    std::lock_guard<std::mutex> lk(c.mu);
    if (refresh_wire_cache_locked(c)) return c.tools;
    return registry();
}

unsigned long mcp_generation() noexcept {
    return mcp::mcp_generation();
}

} // namespace agentty::tools
