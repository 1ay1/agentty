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

#include <functional>
#include <string>

namespace agentty::provider {

struct Selection;  // selection.hpp

// The type-erased Provider seam: exactly the `Provider` concept's `stream`,
// as a callable. Binding a concrete provider is `[&p](Request r, EventSink s){
// p.stream(std::move(r), std::move(s)); }`.
using StreamFn = std::function<void(Request, EventSink)>;

// The two long-lived native providers, as erased routes. Owned by main() so a
// captured reference outlives maya::run / the ACP serve loop. Dispatch never
// sees their concrete types — only these callables — which is what makes the
// routing unit-testable and keeps the registry free of transport types.
struct Routes {
    StreamFn anthropic;   // Kind::Anthropic
    StreamFn chatgpt;     // Kind::OpenAI + Selection::is_chatgpt()
    // Kind::ExternalAcp — drive an external ACP agent subprocess. Bound in
    // main() to stream_external_acp(agent_id, …); erased here so dispatch has
    // ZERO dependency on the acp translation unit and lives in the provider
    // objlib every test links. The agent id comes from the Selection.
    std::function<void(const std::string&, Request, EventSink)> external_acp;
};

// Route ONE turn to the right transport for `sel`. The short-lived
// OpenAI-compat / Ollama / ACP transports are constructed inside; the two
// long-lived ones come from `routes`. Adding a native provider adds one arm
// here (and, if long-lived, one field to Routes).
void dispatch_stream(const Routes& routes, const Selection& sel,
                     Request req, EventSink sink);

// Convenience overload: route the process-global `active()` selection. This
// is the whole body of main.cpp's `stream_fn`.
void dispatch_stream(const Routes& routes, Request req, EventSink sink);

} // namespace agentty::provider
