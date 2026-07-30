#pragma once
// mcp_tools_backends — the agentty-side HostServices backends for the
// host-coupled tool SHELLS that mcp-cpp's toolset owns (remember/forget/
// wipe, todo, skill, search_docs, task).
//
//   mcp-cpp owns each tool's protocol surface; agentty supplies the work via
//   injected backends. install_host_backends() constructs the adapters
//   (MemoryStore over the JSONL store, SkillResolver over the Agent-Skills
//   engine, DocRetriever over the RAG pipeline, SubagentRunner over the
//   subagent loop) and installs them into the HostServices the bridge passes
//   to make_provider(). A tool whose backend is null isn't advertised.

#include <mcp/tools/host.hpp>

#include <optional>
#include <string>

namespace agentty::tools {

// Populate svc.memory / svc.skills / svc.retriever / svc.subagent with the
// agentty backends. Leaves svc.todo null (the mcp todo shell needs no host
// state) and svc.http untouched (the bridge installs the HttpClient).
void install_host_backends(::mcp::tools::HostServices& svc);

// ── Proactive retrieval (explicit opt-in) ────────────────────────────
// Run the RAG pipeline outside the model's tool loop. The app invokes the
// blocking form on an isolated worker and launches the model only after it
// settles, so each turn performs at most one retrieval. Automatic proactive
// injection is disabled unless AGENTTY_RAG_PROACTIVE=1.
struct ProactiveHit {
    std::string block;        // fenced <retrieved-context> text for the wire
    double      confidence;   // [0,1] retrieval confidence that cleared the bar
    int         passages;     // how many passages the block carries
    // Cross-turn dedup keys (source:path:line) for the passages this block
    // ACTUALLY carries. proactive_retrieve builds the block on a worker that
    // may be abandoned on a latency-budget overrun, so the keys are only
    // COMMITTED to the dedup FIFO once the hit is really returned to the
    // caller — an abandoned worker never suppresses a passage it didn't show.
    std::vector<std::string> dedup_keys;
};
[[nodiscard]] std::optional<ProactiveHit>
proactive_retrieve(const std::string& query, int k = 3);

// Same single-execution funnel used by the app's isolated worker.
// the caller always injects/stages this result. Never throws.
[[nodiscard]] std::optional<ProactiveHit>
proactive_retrieve_blocking(const std::string& query, int k = 3);

} // namespace agentty::tools
