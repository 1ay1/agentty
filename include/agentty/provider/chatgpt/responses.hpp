#pragma once
// agentty::provider::chatgpt — direct ChatGPT (Codex) Responses-API transport.
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
#include <string>
#include <vector>

namespace agentty::provider::chatgpt {

// One entry from the ChatGPT account's live `/models` catalog. This is the
// SAME shape codex-rs derives its model picker from: the server tells us which
// slugs the signed-in account may actually use (they change over time — e.g.
// gpt-5.4, not the stale gpt-5.1-codex we used to hardcode), so we never guess.
struct CatalogModel {
    std::string slug;             // wire id sent as body.model
    std::string display_name;     // human label for the picker
    int         context_window = 272000;
    bool        is_default      = false;
};

// GET https://chatgpt.com/backend-api/codex/models?client_version=… with the
// ChatGPT OAuth credential, mirroring codex-rs's ModelsClient. Returns the
// account's live catalog, or an empty vector on any failure (offline, 401,
// parse error) so the caller can fall back to a bundled list. Blocking; short
// timeout — safe to call from the model-picker refresh path.
[[nodiscard]] std::vector<CatalogModel> fetch_models();

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

} // namespace agentty::provider::chatgpt
