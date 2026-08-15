#include "agentty/tool/registry.hpp"

#include "agentty/mcp/client.hpp"
#include "agentty/tool/mcp_tools_bridge.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <memory>
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
// The tools::progress sink itself now lives in its own TU (tool/progress.cpp)
// so subprocess-only consumers can link it without pulling in build_registry()
// and the MCP bridge behind it.

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
std::vector<ToolDef> build_native_registry() {
    return build_mcp_tool_defs();
}

// Connect external MCP exactly once, on first catalog access. The returned
// ToolDefs are not made part of the immutable native baseline: every later
// generation is rebuilt from the MCP pool's current authoritative snapshot.
std::vector<ToolDef> connect_initial_mcp() {
    if (!mcp::mcp_config_present()) return {};
    static mcp::PoolHandle s_pool;
    return mcp::mcp_tools(s_pool);
}

} // namespace

const std::vector<ToolDef>& native_registry() {
    static const std::vector<ToolDef> r = build_native_registry();
    return r;
}

namespace {
// Published snapshots are immutable and retained for process lifetime because
// dispatch resolves a ToolDef pointer once, drops the cache lock, then may run
// for minutes. Retention prevents a concurrent tools/list_changed refresh from
// invalidating that pointer.
struct Snapshot {
    std::vector<ToolDef> tools;
    std::unordered_map<std::string, const ToolDef*> idx;
};

struct WireCache {
    std::mutex mu;
    unsigned long generation = static_cast<unsigned long>(-1);
    bool connected = false;
    std::vector<ToolDef> initial_mcp;
    std::shared_ptr<const Snapshot> current;
    std::vector<std::shared_ptr<const Snapshot>> retired;
};

WireCache& wire_cache() { static WireCache c; return c; }

std::shared_ptr<const Snapshot> refresh_wire_cache_locked(WireCache& c) {
    if (!c.connected) {
        c.initial_mcp = connect_initial_mcp();
        c.connected = true;
    }

    const unsigned long generation = mcp::mcp_generation();
    if (c.current && c.generation == generation) return c.current;

    std::vector<ToolDef> external = generation == 0
        ? c.initial_mcp
        : mcp::mcp_tools_live();

    auto next = std::make_shared<Snapshot>();
    next->tools.reserve(native_registry().size() + external.size());
    next->tools.insert(next->tools.end(), native_registry().begin(), native_registry().end());

    // External names are stable and namespaced, but still reject duplicates
    // defensively rather than sending ambiguous schemas to a model provider.
    std::unordered_map<std::string, bool> names;
    names.reserve(next->tools.capacity());
    for (const auto& tool : next->tools) names.emplace(tool.name.value, true);
    for (auto& tool : external) {
        if (!names.emplace(tool.name.value, true).second) continue;
        next->tools.push_back(std::move(tool));
    }

    next->idx.reserve(next->tools.size());
    for (const auto& tool : next->tools)
        next->idx.emplace(tool.name.value, &tool);

    if (c.current) c.retired.push_back(c.current);
    c.current = std::move(next);
    c.generation = generation;
    return c.current;
}
} // namespace

const std::vector<ToolDef>& registry() { return wire_tools(); }

const ToolDef* find(std::string_view name) {
    auto& cache = wire_cache();
    std::shared_ptr<const Snapshot> snapshot;
    {
        std::lock_guard<std::mutex> lock(cache.mu);
        snapshot = refresh_wire_cache_locked(cache);
    }
    if (auto it = snapshot->idx.find(std::string{name}); it != snapshot->idx.end())
        return it->second;
    return nullptr;
}

const std::vector<ToolDef>& wire_tools() {
    auto& cache = wire_cache();
    std::shared_ptr<const Snapshot> snapshot;
    {
        std::lock_guard<std::mutex> lock(cache.mu);
        snapshot = refresh_wire_cache_locked(cache);
    }
    return snapshot->tools;
}

std::vector<const ToolDef*> select_wire_tools(
    std::string_view query, std::size_t max_external) {
    const auto& catalog = wire_tools();
    std::vector<const ToolDef*> selected;
    std::vector<std::pair<int, const ToolDef*>> candidates;
    selected.reserve(catalog.size());

    std::string q;
    q.reserve(query.size());
    for (unsigned char c : query)
        q.push_back(std::isalnum(c) ? static_cast<char>(std::tolower(c)) : ' ');
    std::vector<std::string> terms;
    for (std::size_t pos = 0; pos < q.size();) {
        while (pos < q.size() && q[pos] == ' ') ++pos;
        const auto begin = pos;
        while (pos < q.size() && q[pos] != ' ') ++pos;
        if (pos - begin >= 2) terms.emplace_back(q.substr(begin, pos - begin));
    }

    for (const auto& tool : catalog) {
        if (tool.origin == ToolOrigin::Native || tool.always_expose) {
            selected.push_back(&tool);
            continue;
        }
        std::string haystack = tool.name.value + " " + tool.origin_id + " " + tool.description;
        std::ranges::transform(haystack, haystack.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        int score = 0;
        for (const auto& term : terms) {
            if (tool.name.value.find(term) != std::string::npos) score += 8;
            if (tool.origin_id.find(term) != std::string::npos) score += 5;
            if (haystack.find(term) != std::string::npos) score += 2;
        }
        candidates.emplace_back(score, &tool);
    }

    if (candidates.size() <= max_external) {
        for (const auto& [_, tool] : candidates) selected.push_back(tool);
        return selected;
    }
    std::stable_sort(candidates.begin(), candidates.end(),
        [](const auto& a, const auto& b) { return a.first > b.first; });
    for (std::size_t i = 0; i < max_external; ++i)
        selected.push_back(candidates[i].second);
    return selected;
}

unsigned long mcp_generation() noexcept {
    return mcp::mcp_generation();
}

std::size_t reload_mcp_plugins() {
    // Rebuild the MCP pool from the current mcp.json. The bridge installs
    // the new pool + bumps its generation; the next select_wire_tools /
    // refresh_wire_cache_locked sees the changed generation and re-projects
    // the tool surface via mcp_tools_live(). We only need to ensure the
    // wire cache is marked connected (so it consults the live pool rather
    // than the cached startup snapshot) — which it already is after any
    // catalog access, and startup always accesses the catalog once.
    const std::size_t n = mcp::mcp_reload();
    // Drop the current published snapshot so the very next catalog build
    // re-projects immediately (generation changed, so it would rebuild
    // anyway; this just avoids serving one stale read in a race).
    {
        auto& c = wire_cache();
        std::lock_guard lk(c.mu);
        c.generation = static_cast<unsigned long>(-1);
    }
    return n;
}

} // namespace agentty::tools
