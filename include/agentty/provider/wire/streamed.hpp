#pragma once
// unseen() — reconcile a streamed value delivered as fragments, as snapshots,
// or as any mix of the two.
//
// Every streaming chat protocol carries a tool call's arguments two ways:
// incremental fragments ("the next 12 bytes") and authoritative totals ("the
// arguments, complete"). Both are spec-legal, and which you get is a property
// of the SERVER: Codex streams fragments, GitHub Copilot's proxy coalesces and
// sends only the final snapshot.
//
// Our Responses codec decoded fragments and ignored the snapshot, so against
// Copilot every tool call dispatched with `{}` — `grep` failed "pattern
// required", `read` failed "path required", and the zero-argument tools
// (repo_map, list) still worked, which made it look like a flaky model rather
// than a dropped event.
//
// The fix is to stop choosing: append what you're given, keep what you have,
// and emit only what is newly known. Then routing EVERY carrier into it is
// safe, because a redundant one contributes nothing.

#include <string>
#include <string_view>

namespace agentty::provider::wire {

// Merge `chunk` into `have` and return the bytes that just became known.
//
// `total` says whether `chunk` is the server's complete view (a snapshot) or
// the next piece (a fragment). Snapshots are idempotent — replaying one, or
// receiving one after the fragments that already spelled it out, yields "".
//
// A snapshot shorter than what we hold is ignored: bytes already emitted
// cannot be un-emitted, so keeping them is the only coherent choice.
[[nodiscard]] inline std::string_view unseen(std::string& have,
                                             std::string_view chunk,
                                             bool total) {
    const std::size_t at = have.size();
    if (!total) { have += chunk; }
    else if (chunk.size() > at) { have.assign(chunk); }
    else { return {}; }
    return std::string_view{have}.substr(at);
}

} // namespace agentty::provider::wire
