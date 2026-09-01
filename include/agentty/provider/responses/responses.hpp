#pragma once
// agentty::provider::responses — the OpenAI **Responses API** dialect, as a
// shared codec parameterised by a per-host descriptor.
//
// ─────────────────────────────────────────────────────────────────────────
// Why this module exists
// ─────────────────────────────────────────────────────────────────────────
// Two very different hosts speak this same wire dialect:
//
//   • ChatGPT (Codex)  chatgpt.com/backend-api/codex/responses, ChatGPT OAuth
//   • GitHub Copilot   api.*.githubcopilot.com/responses, proxy token +
//                      an Auto-session token
//
// and the expensive part is IDENTICAL for both: turning an agentty
// conversation into `input[]` (tool-call pairing, image parts, age-tiered
// wire budgets), building `tools[]`, and running the SSE state machine that
// translates `response.output_text.delta`,
// `response.reasoning_summary_text.delta`, `response.output_item.*` and
// friends back into agentty Msgs. That machinery is ~500 lines and has
// nothing to do with WHO is being dialled.
//
// What actually differs between hosts is only the HTTP envelope. Measured,
// not assumed (live probes against both backends):
//
//   ┌───────────────┬──────────────────────────┬───────────────────────────┐
//   │               │ ChatGPT                  │ Copilot                   │
//   ├───────────────┼──────────────────────────┼───────────────────────────┤
//   │ host + path   │ chatgpt.com/backend-api/ │ api.individual.githubcopi │
//   │               │ codex/responses          │ lot.com/responses         │
//   │ auth          │ OAuth access token       │ proxy token FROM the      │
//   │               │ (+ chatgpt-account-id)   │ token exchange            │
//   │ extra headers │ session_id, originator   │ copilot-session-token,    │
//   │               │                          │ x-github-api-version,     │
//   │               │                          │ editor identification     │
//   │ body extras   │ include[] encrypted      │ reasoning.summary         │
//   │               │ reasoning, store:false   │                           │
//   │ error prose   │ "run `agentty login`"    │ entitlement wording       │
//   └───────────────┴──────────────────────────┴───────────────────────────┘
//
// So the split is: ONE codec, and a small `Site` descriptor per host. A host
// supplies where to POST and how to authorise; everything downstream of the
// first byte is shared. Adding a third Responses backend is a new Site row,
// not a second copy of the state machine.
//
// ─────────────────────────────────────────────────────────────────────────
// Why a descriptor of function pointers, not a virtual base
// ─────────────────────────────────────────────────────────────────────────
// agentty's providers are a *concept* (`stream(Request, EventSink) ->
// StreamResult`), satisfied by six unrelated classes with no common base.
// Introducing a hierarchy here would be the only vtable in the provider
// layer. A designated-init row of plain function pointers matches the idiom
// already used by `auth::vault::of()` — data, trivially testable, and every
// host's behaviour is readable in one place.

#include "agentty/provider/provider.hpp"
#include "agentty/provider/stream_epilogue.hpp"   // provider::StreamResult
#include "agentty/io/http.hpp"

#include <expected>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace agentty::provider::responses {

// Everything a host must resolve before a request can go out: WHERE to POST
// and WITH WHAT credentials. Returned by Site::authorize so the (blocking,
// possibly refreshing) auth work happens once per turn, off the codec's path.
struct Target {
    std::string host;                 // "chatgpt.com"
    std::uint16_t port = 443;
    std::string path;                 // "/backend-api/codex/responses"
    http::Headers headers;            // auth + identification, fully formed
    // The model slug to put on the wire. Hosts may REWRITE the caller's
    // model (Copilot's Auto session picks a server-blessed slug), so the
    // codec always sends this rather than req.model.
    std::string model;
};

// A host that speaks the Responses dialect.
//
// `authorize` does the blocking credential work and returns either a Target
// or a user-facing error string (already phrased for this host — the codec
// never invents auth prose). It may also mutate `req` (e.g. Copilot pinning
// the Auto-session's model choice).
struct Site {
    std::string_view id;              // "chatgpt" | "copilot" — routing key.

    // Resolve credentials + destination. Error string on failure.
    std::expected<Target, std::string> (*authorize)(provider::Request& req);

    // Host-specific request-body fields, applied after the shared builder.
    // ChatGPT adds include[]/store; Copilot adds reasoning.summary. Both
    // receive the same neutral body.
    void (*decorate_body)(nlohmann::json& body, const provider::Request& req);

    // Turn a non-2xx response into an actionable diagnostic. Hosts phrase
    // their own remediation ("run `agentty login`…" vs entitlement wording).
    std::string (*explain_http_error)(int status, std::string_view body);
};

// Stream one turn against `site`. Owns the whole lifecycle: authorise, build
// the body, POST, translate SSE → Msgs, and finish through the SHARED
// stream epilogue so this path terminates identically to Anthropic/OpenAI.
[[nodiscard]] provider::StreamResult stream(const Site& site,
                                            provider::Request req,
                                            provider::EventSink sink);

// ── Codec pieces (exposed for tests and for hosts that need them) ────────

// Conversation → Responses `input[]`.
[[nodiscard]] nlohmann::json build_input(const provider::Request& req);
// agentty ToolSpecs → Responses `tools[]`.
[[nodiscard]] nlohmann::json build_tools(const provider::Request& req);
// The neutral request body (model/input/tools/stream + reasoning ladder).
// Hosts layer their extras on top via Site::decorate_body.
[[nodiscard]] nlohmann::json build_body(const provider::Request& req);

// Scripted SSE `data:` payloads → the Msg sequence a reducer would see.
// The single entry point every Responses-dialect test drives.
[[nodiscard]] std::vector<Msg> parse_sse_for_test(
    const std::vector<std::string>& sse_data_lines);

} // namespace agentty::provider::responses
