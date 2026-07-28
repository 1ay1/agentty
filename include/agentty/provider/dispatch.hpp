#pragma once
// agentty::provider — the ONE provider-routing seam.
//
// `main.cpp` used to hand-write an if/else ladder (kind == ExternalAcp /
// OpenAI / is_chatgpt / native_api / else) to pick which concrete transport
// streams a turn. That ladder was the LAST place a native provider's routing
// lived, uncorrelated with the registry and impossible to unit-test.
//
// This is now that single routing point, and it is provider-AGNOSTIC: it
// names no concrete transport type. The long-lived native providers
// (Anthropic + ChatGPT — they hold connection / OAuth-token state, so they
// outlive individual turns) are handed in as type-erased `StreamFn`s bound
// once in main(); the short-lived OpenAI-compat / Ollama transports are cheap
// value types built per call from the active `Endpoint`. Because the seam is
// type-erased, a test can inject fake routes and assert which one a given
// Selection reaches — no network, no concrete provider.
//
// Routing dispatches on `provider::active()` at CALL TIME (not a frozen
// branch), so a live provider switch (picker → select()) retargets the very
// next request with no seam rebuild. A `Selection` can be passed explicitly
// (tests) to route a specific selection deterministically.

#include "agentty/provider/provider.hpp"
#include "agentty/provider/stream_epilogue.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace agentty::provider {

struct Selection;  // selection.hpp

// The type-erased Provider seam: exactly the `Provider` concept's `stream`,
// as a callable — including its StreamResult return, so the outcome flows
// through the erased boundary instead of being silently dropped. Binding a
// concrete provider is `[&p](Request r, EventSink s){ return
// p.stream(std::move(r), std::move(s)); }`.
using StreamFn = std::function<StreamResult(Request, EventSink)>;

// Which long-lived provider slot a descriptor routes to. A LongLived provider
// holds cross-turn state (refreshed OAuth tokens / a warm transport), so it is
// constructed ONCE in main() and reused; dispatch looks it up here instead of
// re-deriving "is this chatgpt" from a label. `None` means the selection is
// served by a per-call transport (OpenAI-compat / Ollama) or the ACP arm.
//
// Adding a long-lived provider = one enumerator here + one slot in Router +
// one `slot_for` arm. Adding a per-call provider needs NONE of this — it flows
// through the generic OpenAI-compat builder purely from its registry row.
enum class LongLived : std::uint8_t { None, Anthropic, ChatGpt };

// Map a selection to its long-lived slot, purely from registry data: the
// oauth_native flag (ChatGPT/Codex) and the Anthropic dialect. No label
// compares — a new OAuth-native provider is added by a row + an arm here.
[[nodiscard]] LongLived long_lived_slot(const Selection& sel);

// The router: the long-lived providers as erased StreamFns (owned by main() so
// a captured reference outlives maya::run / the ACP serve loop) plus the ACP
// arm. Dispatch never names a concrete provider type — it indexes this by the
// descriptor-derived LongLived slot — which is what makes routing unit-testable
// and keeps the registry free of transport types. Per-call transports
// (OpenAI-compat / Ollama) are built inside dispatch from the active Endpoint,
// so they need no slot here.
struct ProviderRouter {
    StreamFn long_lived[3]{};   // indexed by LongLived: [None]=unused, [Anthropic], [ChatGpt]

    // Kind::ExternalAcp — drive an external ACP agent subprocess. Bound in
    // main() to stream_external_acp(agent_id, …); erased here so dispatch has
    // ZERO dependency on the acp translation unit and lives in the provider
    // objlib every test links. The agent id comes from the Selection.
    std::function<StreamResult(const std::string&, Request, EventSink)> external_acp;

    // Ergonomic setters so main() reads as data, not array-index noise.
    ProviderRouter& set(LongLived slot, StreamFn fn) {
        long_lived[static_cast<std::size_t>(slot)] = std::move(fn);
        return *this;
    }
};

// Back-compat alias: `Routes` was the struct's name before it became
// descriptor-indexed. Kept so existing call sites/tests keep compiling.
using Routes = ProviderRouter;

// Route ONE turn to the right transport for `sel`. The short-lived
// OpenAI-compat / Ollama / ACP transports are constructed inside; the two
// long-lived ones come from `routes`. Adding a native provider adds one arm
// here (and, if long-lived, one field to Routes). Returns the turn's
// StreamResult from whichever transport ran it.
StreamResult dispatch_stream(const Routes& routes, const Selection& sel,
                             Request req, EventSink sink);

// Convenience overload: route the process-global `active()` selection. This
// is the whole body of main.cpp's `stream_fn`.
StreamResult dispatch_stream(const Routes& routes, Request req, EventSink sink);

} // namespace agentty::provider
