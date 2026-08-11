#pragma once
// agentty::smart::DecompositionMemory — retrieval-augmented orchestration.
//
// Innovation 4 (deep). When a COMPLEX turn succeeds by delegating work to
// subagents, the SEQUENCE of delegations that worked ("explore call sites →
// edit → test") is a reusable artifact. This store captures those successful
// decompositions per-workspace and, on a future similar turn, retrieves the
// closest one so the orchestrator gets a concrete few-shot example of how THIS
// codebase decomposes this kind of work — instead of re-deriving it every
// time. The router doesn't just learn HOW HARD a turn is (RoutingMemory); it
// learns HOW TO STRUCTURE it.
//
// Persisted append-only as JSONL under <cwd>/.agentty/decompositions.jsonl,
// mirroring the memory/feedback stores' shape. Best-effort; thread-safe; any
// I/O failure degrades to "no recall", never throws across the boundary.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace agentty::smart {

// One captured, successful decomposition of a turn.
struct Decomposition {
    std::string              signature;   // RoutingMemory turn_signature
    std::string              gist;        // first line of the user turn (context)
    std::vector<std::string> steps;       // "explorer: map the auth call sites"
};

class DecompositionMemory {
public:
    static DecompositionMemory& instance();

    // Record a successful decomposition. `steps` are "agent_type: brief"
    // one-liners in call order. No-op if steps is empty. Deduped against the
    // most recent record for the same signature so a repeated pattern doesn't
    // flood the file.
    void record(const std::string& signature, std::string_view gist,
                std::vector<std::string> steps);

    // Retrieve the best matching past decomposition for a signature, or empty
    // when none / recall is off. Prefers an exact-signature match; falls back
    // to the same complexity tier (signature prefix before the first ':').
    [[nodiscard]] std::vector<Decomposition> recall(const std::string& signature,
                                                    std::size_t k = 1);

    // Number of captured decompositions in this workspace — telemetry.
    [[nodiscard]] std::size_t learned_count();

    // Wipe this workspace's captured decompositions (file + cache).
    void reset();

    // Test seam: force the workspace root and clear cache.
    void set_root_for_test(std::string root);

private:
    DecompositionMemory() = default;
    struct Impl;
    Impl& impl();
};

} // namespace agentty::smart
