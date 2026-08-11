#pragma once
// agentty::smart::RoutingMemory — a per-workspace, self-supervised routing
// prior for Smart Mode. THE innovation that stateless routers structurally
// can't have: agentty observes its OWN execution outcomes and learns, per
// repo, whether the complexity heuristic under- or over-rates each class of
// turn — then persists that so the router gets measurably better at YOUR
// codebase the more you use it.
//
// Mirrors the RAG FeedbackStore's proven shape (append-only TSV under
// <cwd>/.agentty/, Beta-smoothed evidence, bounded output). Two orthogonal
// signals feed it:
//   • delegation   — the cascade proxy already in finalize_turn (heavy
//                    delegation on a cheap-rated turn ⇒ under-rated).
//   • outcome      — the GROUND-TRUTH signal no query-only router can use: a
//                    turn immediately followed by a user correction, a failed
//                    build, or a git revert is a routing REGRET.
//
// The store keys on a coarse turn SIGNATURE (the classified complexity tier +
// a couple of cheap token-class buckets) so "this kind of turn in this repo"
// accrues evidence. It returns a signed effort-bias prior in [-1, +1] the
// launcher folds into the session bias. Pure of UI; thread-safe; best-effort
// (any I/O failure ⇒ neutral, never throws across the boundary).

#include <cstdint>
#include <string>
#include <string_view>

#include "agentty/domain/complexity.hpp"

namespace agentty::smart {

// A coarse, stable signature for "this kind of turn". Deliberately low-
// cardinality so evidence concentrates instead of scattering across unique
// prompts. Built from the classified tier + whether the turn looks like it
// touches code / asks a question / is long.
[[nodiscard]] std::string turn_signature(Complexity tier, std::string_view text);

class RoutingMemory {
public:
    static RoutingMemory& instance();

    // The learned effort-bias PRIOR for a signature, in roughly [-1, +1].
    // 0 (neutral) when learning is off, the signature is unseen, or evidence
    // is too thin. Positive ⇒ this class of turn has historically been under-
    // rated (needs more effort); negative ⇒ over-rated.
    [[nodiscard]] int prior_bias(const std::string& signature);

    // Record that a turn with this signature RAN (the denominator). Called at
    // launch when learning is on.
    void note_routed(const std::string& signature);

    // Record a REGRET for the last routed signature: the turn's outcome shows
    // it was mis-rated. `direction` > 0 ⇒ under-rated (should have thought
    // harder — a correction/build-fail/revert after a cheap turn); < 0 ⇒
    // over-rated (thought hard, delegated nothing, trivial reply).
    void note_regret(const std::string& signature, int direction);

    // Test seam: force the workspace root (bypasses cwd) and clear cache.
    void set_root_for_test(std::string root);

private:
    RoutingMemory() = default;
    struct Impl;
    // Function-local static impl to keep the header dependency-free.
    Impl& impl();
};

} // namespace agentty::smart
