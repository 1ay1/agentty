#include "agentty/tool/registry.hpp"

#include "agentty/mcp/client.hpp"
#include "agentty/tool/mcp_tools_bridge.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <memory>
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
        case ErrorKind::Denied:         return "denied";
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
// Snapshots are immutable and never freed: dispatch resolves a ToolDef
// pointer once and may run for minutes, so a refresh must not invalidate it.
// Each snapshot keeps the one it replaced alive through `prev`.
struct Snapshot {
    std::vector<ToolDef> tools;
    std::unordered_map<std::string, const ToolDef*> idx;
    unsigned long generation = 0;
    bool stale = false;   // marker published by invalidation; never served
    std::shared_ptr<const Snapshot> prev;
};

enum class Phase { Idle, Connecting, Connected };
struct Connect {
    Phase phase = Phase::Idle;
    std::vector<ToolDef> initial_mcp;
};

maya::guarded<Connect>& connect_state() { static maya::guarded<Connect> c; return c; }
// Leaked on purpose: snapshots live for the whole process, and tearing down
// a long `prev` chain at exit would only recurse.
maya::published<const Snapshot>& catalog() {
    static auto* c = new maya::published<const Snapshot>;
    return *c;
}

void index(Snapshot& s) {
    s.idx.reserve(s.tools.size());
    for (const auto& tool : s.tools) s.idx.emplace(tool.name.value, &tool);
}

// Served while another thread is mid-connect, so nobody waits on it.
std::shared_ptr<const Snapshot> native_only() {
    static const std::shared_ptr<const Snapshot> s = [] {
        auto ns = std::make_shared<Snapshot>();
        ns->tools = native_registry();
        index(*ns);
        return std::shared_ptr<const Snapshot>(std::move(ns));
    }();
    return s;
}

std::shared_ptr<const Snapshot> refresh_wire_cache() {
    // The first caller connects external MCP; anyone arriving meanwhile gets
    // the native tools right away (it may be the UI thread, and the connect
    // can take up to 15 s).
    enum class Claim { Mine, Busy, Done };
    const Claim claim = connect_state().with([](Connect& c) {
        if (c.phase == Phase::Connected)  return Claim::Done;
        if (c.phase == Phase::Connecting) return Claim::Busy;
        c.phase = Phase::Connecting;
        return Claim::Mine;
    });
    if (claim == Claim::Busy) return native_only();
    if (claim == Claim::Mine) {
        std::vector<ToolDef> ext;
        try {
            ext = connect_initial_mcp();
        } catch (...) {
            connect_state().with([](Connect& c) { c.phase = Phase::Idle; });
            throw;
        }
        connect_state().with([](Connect& c, std::vector<ToolDef> e) {
            c.initial_mcp = std::move(e);
            c.phase = Phase::Connected;
        }, std::move(ext));
    }

    for (;;) {
        auto cur = catalog().current();
        const unsigned long generation = mcp::mcp_generation();
        if (cur && !cur->stale && cur->generation == generation) return cur;

        std::vector<ToolDef> external = generation == 0
            ? connect_state().read([](const Connect& c) { return c.initial_mcp; })
            : mcp::mcp_tools_live();

        auto next = std::make_shared<Snapshot>();
        next->tools.reserve(native_registry().size() + external.size());
        next->tools.insert(next->tools.end(), native_registry().begin(), native_registry().end());

        // External names are namespaced, but still drop duplicates rather
        // than send ambiguous schemas to a provider.
        std::unordered_map<std::string, bool> names;
        names.reserve(next->tools.capacity());
        for (const auto& tool : next->tools) names.emplace(tool.name.value, true);
        for (auto& tool : external) {
            if (!names.emplace(tool.name.value, true).second) continue;
            next->tools.push_back(std::move(tool));
        }
        index(*next);
        next->generation = generation;
        next->prev = cur;

        std::shared_ptr<const Snapshot> out = std::move(next);
        if (catalog().publish_if(cur, out)) return out;
        // Someone else published (a rebuild or an invalidation); re-check.
    }
}

// Mark the current catalog stale so the next access rebuilds it.
void invalidate_catalog() {
    for (;;) {
        auto cur = catalog().current();
        if (!cur) return;
        auto marker = std::make_shared<Snapshot>();
        marker->stale = true;
        marker->prev = cur;
        if (catalog().publish_if(cur, std::move(marker))) return;
    }
}
} // namespace

const std::vector<ToolDef>& registry() { return wire_tools(); }

// Legacy tool-name aliases → canonical registry name. The exec tool was renamed
// bash → shell, but `bash` is so dominant in model training data (it is Claude
// Code's actual tool name — commonly emitted as "Bash", capital B) that a model
// reliably calls it by the old name on the FIRST turn, before it internalises
// our schema. Without this the call 404s ("unknown tool: Bash"), the model
// burns a wasted failed call, then retries as `shell`. Canonicalising here —
// the ONE lookup every dispatch path (agent loop, subagent, ACP, mcp-serve,
// permission check) funnels through — absorbs that misfire so the legacy name
// just works.
//
// The match is CASE-INSENSITIVE and whitespace-trimmed: models emit "bash",
// "Bash", and "BASH" interchangeably, and the exact-case check was the bug that
// let "Bash" slip through to a hard failure.
[[nodiscard]] static std::string_view canonical_tool_name(std::string_view name) noexcept {
    // Trim surrounding whitespace without allocating.
    auto b = name.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) return name;
    auto e = name.find_last_not_of(" \t\r\n");
    std::string_view trimmed = name.substr(b, e - b + 1);
    auto ieq = [](std::string_view s, std::string_view lit) {
        if (s.size() != lit.size()) return false;
        for (std::size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != lit[i]) return false;
        }
        return true;
    };
    if (ieq(trimmed, "bash")) return "shell";
    return name;
}

std::string unknown_tool_error(std::string_view name) {
    std::string msg = "unknown tool: " + std::string{name}
        + ". This tool is not part of agentty's toolset — if a proxy or "
          "gateway advertised it, call it through that channel, not here. "
          "Available tools:";
    for (const auto& td : wire_tools()) {
        msg += ' ';
        msg += td.name.value;
        msg += ',';
    }
    if (msg.back() == ',') msg.pop_back();
    msg += '.';
    return msg;
}

const ToolDef* find(std::string_view name) {
    const auto snapshot = refresh_wire_cache();
    if (auto it = snapshot->idx.find(std::string{name}); it != snapshot->idx.end())
        return it->second;
    // Miss: retry once under the canonical name so a legacy alias resolves.
    if (auto canon = canonical_tool_name(name); canon != name)
        if (auto it = snapshot->idx.find(std::string{canon}); it != snapshot->idx.end())
            return it->second;
    return nullptr;
}

const std::vector<ToolDef>& wire_tools() {
    const auto snapshot = refresh_wire_cache();
    return snapshot->tools;
}

std::vector<ToolDef> wire_tools_snapshot() {
    // A copy the caller owns; safe on any thread that may overlap a reload.
    const auto snapshot = refresh_wire_cache();
    return snapshot->tools;   // deep copy while snapshot keeps it alive
}

std::vector<const ToolDef*> select_wire_tools(
    std::string_view query, std::size_t max_external) {
    return select_wire_tools_from(wire_tools(), query, max_external);
}

std::vector<const ToolDef*> select_wire_tools_from(
    const std::vector<ToolDef>& catalog, std::string_view query, std::size_t max_external) {
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
        // Dispatch-only tools (passthrough fulfilling a proxy-advertised
        // schema) never enter the wire list — the proxy already injected
        // their schema into the request; ours would collide with it.
        if (!tool.advertise) continue;
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
    // Score decides WHICH tools make the cut, never their ORDER on the wire.
    // The tools block is the head of the prompt-cache prefix (tools → system
    // → messages), so emitting in score order reordered it on every user
    // message even when the chosen set was the same, busting the whole cache.
    // Emit the chosen ones in catalog order: same set, same bytes.
    std::vector<const ToolDef*> chosen;
    chosen.reserve(max_external);
    for (std::size_t i = 0; i < max_external; ++i) chosen.push_back(candidates[i].second);
    std::ranges::sort(chosen, std::less<>{});   // catalog is one contiguous vector
    for (const auto* t : chosen) selected.push_back(t);
    return selected;
}

unsigned long mcp_generation() noexcept {
    return mcp::mcp_generation();
}

std::size_t reload_mcp_plugins() {
    // One reload at a time. Each plugin toggle fires this on its own thread
    // and a reconnect can take seconds, so a request that lands mid-reload
    // just sets `pending` and returns; the running reload loops once more to
    // pick up the newer config. Checking `pending` and clearing `running`
    // happen in one locked step, so no request is lost.
    struct Reload { bool running = false; bool pending = false; };
    static maya::guarded<Reload> state;

    const bool mine = state.with([](Reload& r) {
        if (r.running) { r.pending = true; return false; }
        r.running = true;
        return true;
    });
    if (!mine) return 0;

    std::size_t n = 0;
    do {
        state.with([](Reload& r) { r.pending = false; });
        n = mcp::mcp_reload();
        invalidate_catalog();
    } while (state.with([](Reload& r) {
        if (r.pending) return true;
        r.running = false;
        return false;
    }));
    return n;
}

void invalidate_mcp_catalog() {
    // No re-spawn: bump the pool generation and mark the catalog stale.
    // project_tools re-reads tools.exclude on the rebuild, so an
    // enable/disable toggle lands on the next access with no server churn.
    mcp::mcp_bump_generation();
    invalidate_catalog();
}

} // namespace agentty::tools
