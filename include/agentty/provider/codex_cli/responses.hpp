#pragma once
// agentty::provider::codex_cli — direct ChatGPT (Codex) Responses-API transport.
//
// This is the "works like Claude" path: given a native ChatGPT OAuth
// credential (from `agentty login` → ChatGPT), it POSTs the agentty
// conversation straight to https://chatgpt.com/backend-api/codex/responses as
// an OpenAI Responses-API stream and translates the SSE back into agentty's
// Stream* Msgs — real text, reasoning, and tool-call cards — with NO `codex`
// binary at runtime. The access token is auto-refreshed in place, exactly like
// the Anthropic OAuth path.

#include "agentty/provider/provider.hpp"

#include <nlohmann/json.hpp>
#include <vector>

namespace agentty::provider::codex_cli {

// True iff a saved ChatGPT credential exists (so the provider can pick the
// direct transport over the app-server subprocess fallback).
[[nodiscard]] bool responses_available();

// Stream one turn against the ChatGPT Responses backend. Emits StreamStarted →
// text/thinking/tool deltas → StreamFinished (or StreamError). Blocking; runs
// on the turn's worker thread and honours req.cancel.
void stream_responses(provider::Request req, provider::EventSink sink);

// ── Test seams (pure, no network) ──────────────────────────────────────────
// The Request → Responses-API JSON body the transport would POST.
[[nodiscard]] nlohmann::json build_body_for_test(const provider::Request& req);
// Scripted SSE `data:` payloads → the agentty Msg sequence the reducer sees.
[[nodiscard]] std::vector<Msg> parse_sse_for_test(
    const std::vector<std::string>& sse_data_lines);

} // namespace agentty::provider::codex_cli
