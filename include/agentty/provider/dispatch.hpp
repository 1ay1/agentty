#pragma once
// agentty::provider — the ONE provider-dispatch seam.
//
// `main.cpp` used to hand-write an if/else ladder (kind == ExternalAcp /
// OpenAI / label == "chatgpt" / native_api / else) to pick which concrete
// transport streams a turn. That ladder is the LAST place a new native
// provider's routing lived, uncorrelated with the registry table — nothing
// forced a new backend to have a dispatch arm.
//
// This function is now that single routing point. `main.cpp` owns the two
// long-lived native providers (Anthropic + ChatGPT hold connection/auth
// state, so they outlive individual turns) and hands them in by reference;
// the short-lived OpenAI-compat / Ollama transports are cheap value types
// built per call from the active Endpoint. Routing decides which one runs a
// given Request, dispatching on the process-global `provider::active()` at
// CALL TIME so a live provider switch (picker → select()) takes effect on the
// very next turn with no seam rebuild.
//
// Keeping this in the provider layer (not inline in main.cpp) means the
// routing is unit-testable and a new native provider adds exactly one arm
// HERE, next to the registry it belongs with — not scattered across the
// runtime bootstrap.

#include "agentty/provider/anthropic/provider.hpp"
#include "agentty/provider/chatgpt/provider.hpp"
#include "agentty/provider/provider.hpp"

namespace agentty::provider {

// The long-lived native providers, owned by main() so a captured reference
// outlives maya::run / the ACP serve loop. Passed to dispatch_stream by ref.
struct NativeProviders {
    anthropic::AnthropicProvider& anthropic;
    chatgpt::ChatGptProvider&     chatgpt;
};

// Route ONE turn to the right native transport based on `provider::active()`.
// This is the whole body of the old main.cpp `stream_fn` lambda: a single
// place, dispatched on the live selection, so a picker switch retargets the
// next request immediately. Adding a native provider adds one arm here.
void dispatch_stream(NativeProviders natives, Request req, EventSink sink);

} // namespace agentty::provider
