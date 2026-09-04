// agentty::provider::openai — the Responses-dialect Site for api.openai.com
// (and any OpenAI-compatible host that advertises a `responses_path`).
//
// ─────────────────────────────────────────────────────────────────────────
// Why this file is small
// ─────────────────────────────────────────────────────────────────────────
// Everything expensive about the Responses dialect — building `input[]` with
// tool-call pairing and image parts, replaying reasoning `encrypted_content`
// across tool rounds, and the SSE state machine — already lives in
// provider/responses/codec.cpp and is shared verbatim. A host contributes
// only three things (responses::Site): where to dial + how to authenticate,
// what host-specific fields to layer on the neutral body, and how to phrase
// an HTTP failure. This is the third such host, and the codec needed no
// changes to accept it — which was the design claim in
// docs/PROVIDER_HETEROGENEITY.md, now exercised.
//
// ─────────────────────────────────────────────────────────────────────────
// Why api.openai.com needs a Responses path AT ALL
// ─────────────────────────────────────────────────────────────────────────
// Per OpenAI's reasoning guide, on Chat Completions: from GPT-5.4 tool
// calling is unsupported at any reasoning_effort other than `none`, and
// GPT-6-class models drop chat function calling entirely. agentty always
// sends tools, so on the chat dialect those models cannot run an agent turn
// at all. Routing is decided by provider::dialect_for() — see dialect.hpp;
// this file is only the destination.

#include "agentty/provider/openai/responses_site.hpp"

#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "agentty/auth/auth.hpp"
#include "agentty/io/http.hpp"
#include "agentty/provider/dialect.hpp"
#include "agentty/provider/registry.hpp"
#include "agentty/provider/responses/responses.hpp"
#include "agentty/util/logx.hpp"

namespace agentty::provider::openai {
namespace {

using json = nlohmann::json;

// The endpoint the CURRENT turn should be dialled on. Set by
// stream_responses() immediately before responses::stream() runs, because
// Site::authorize is a plain function pointer with no user-data slot and the
// destination depends on the active provider row (openai vs openrouter vs a
// custom host that advertises /responses).
//
// thread_local, not global: agentty can run a subagent turn concurrently with
// the foreground turn, and those may sit on different providers. A shared
// global here would let one turn retarget the other's host mid-flight.
struct PendingTarget {
    std::string host;
    std::uint16_t port = 443;
    std::string path;
    bool use_tls = true;
    std::string provider_id;
    auth::AuthHeader auth;
    std::vector<std::pair<std::string, std::string>> extra_headers;
};
thread_local PendingTarget t_pending;

std::expected<responses::Target, std::string>
openai_authorize(provider::Request& req) {
    const auto& p = t_pending;

    // A TLS host must carry a credential. Local/plaintext hosts (a self-run
    // vLLM or llama.cpp that speaks Responses) legitimately have none, which
    // mirrors the chat transport's rule rather than inventing a second one.
    const std::string key = auth::bearer_token(req.auth);
    if (p.use_tls && key.empty())
        return std::unexpected(
            std::string{"not authenticated — set the provider's API key "
                        "(e.g. OPENAI_API_KEY) or run `agentty login`"});

    responses::Target t;
    t.host = p.host;
    t.port = p.port;
    t.path = p.path;
    t.headers = {
        {"content-type", "application/json"},
        {"accept",       "text/event-stream"},
        {"user-agent",   "agentty/" AGENTTY_VERSION},
        // The chat path learned this the hard way (issue #30): gateways gzip
        // responses even with no Accept-Encoding sent, and agentty's HTTP
        // client has no inflater on the streaming path. Ask for identity.
        {"accept-encoding", "identity"},
    };
    if (!key.empty())
        t.headers.push_back({"authorization", "Bearer " + key});
    // Copilot-style editor identification, or a gateway's required extras.
    for (const auto& [k, v] : p.extra_headers)
        t.headers.push_back({k, v});

    // The caller's model is authoritative here: unlike Copilot's Auto
    // session, no server blesses a substitute slug on this path.
    t.model = req.model;
    return t;
}

void openai_decorate_body(json& body, const provider::Request&) {
    // agentty replays the entire conversation every turn, so there is nothing
    // for the server to remember. store:false also keeps transcripts off
    // OpenAI's side, which is the behaviour a terminal agent should default
    // to — the user never asked us to persist their code upstream.
    body["store"] = false;

    // Return each reasoning item's encrypted_content so build_input() can
    // replay it next turn. THIS is the feature that makes Responses worth
    // routing to: without it a reasoning model re-derives its plan from
    // scratch on every tool round (worse answers, more tokens, worse cache
    // hits). Under store:false it is the only channel for chain-of-thought.
    //
    // Newer API versions return it by default under store:false; asking
    // explicitly is accepted and keeps behaviour identical across the
    // versions a user's account may be pinned to.
    body["include"] = json::array({"reasoning.encrypted_content"});
}

std::string openai_explain_http_error(int status, std::string_view body) {
    const std::string b{body};
    switch (status) {
        case 401:
        case 403:
            return "OpenAI rejected the API key (HTTP " + std::to_string(status)
                 + ") — check OPENAI_API_KEY or run `agentty login`";
        case 404:
            // The dialect router will demote this (provider, model) to chat
            // for the rest of the process; say so, so the user understands why
            // the next turn behaves differently.
            return "this host has no /responses endpoint (HTTP 404) — "
                   "falling back to chat completions for this model";
        case 429:
            return "rate limited by OpenAI (HTTP 429)" +
                   (b.empty() ? std::string{} : " — " + b);
        default:
            if (status >= 500)
                return "OpenAI server error (HTTP " + std::to_string(status)
                     + ") — retrying usually works";
            return "OpenAI request failed (HTTP " + std::to_string(status) + ")"
                 + (b.empty() ? std::string{} : ": " + b);
    }
}

const responses::Site kOpenAiSite{
    .id                 = "openai-responses",
    .authorize          = &openai_authorize,
    .decorate_body      = &openai_decorate_body,
    .explain_http_error = &openai_explain_http_error,
};

// A 404 (no such endpoint) or a 400 naming the API/model pairing means this
// host will not serve this model on Responses. Anything else — 401, 429, 5xx,
// a mid-stream drop — is about credentials or weather, NOT about the dialect,
// and must not demote a model that genuinely requires Responses.
[[nodiscard]] bool indicates_dialect_unsupported(int status,
                                                 std::string_view body) noexcept {
    if (status == 404) return true;
    if (status != 400) return false;
    for (std::string_view needle : {"unsupported_api", "unsupported_api_for_model",
                                    "not supported", "unknown_url",
                                    "invalid_url", "must be used with"})
        if (body.find(needle) != std::string_view::npos) return true;
    return false;
}

} // namespace

bool responses_endpoint_for(std::string_view provider_id,
                            ResponsesEndpoint& out) noexcept {
    const auto* row = preset_for(provider_id);
    if (!row || row->responses_path.empty()) return false;
    out.host = std::string{row->host};
    out.port = row->port;
    out.path = std::string{row->responses_path};
    out.use_tls = row->use_tls;
    return true;
}

provider::StreamResult stream_responses(const ResponsesEndpoint& ep,
                                        provider::Request req,
                                        EventSink sink) {
    t_pending = PendingTarget{
        .host = ep.host, .port = ep.port, .path = ep.path,
        .use_tls = ep.use_tls, .provider_id = ep.provider_id,
        .auth = req.auth, .extra_headers = ep.extra_headers,
    };
    const std::string provider_id = ep.provider_id;
    const std::string model = req.model;

    AGT_LOG(Wire, Info, "openai.responses",
            "dialling {}{} for {}", ep.host, ep.path, model);

    auto result = responses::stream(kOpenAiSite, std::move(req), std::move(sink));

    // ── Teach the router from what the wire actually said ────────────────
    // The family tables in dialect.cpp are a prior about model NAMES, and
    // names move faster than releases. A host that answers "no such endpoint"
    // demotes this exact (provider, model) so the next turn goes out on chat
    // and the user sees a working session instead of a repeated failure.
    if (!result.ok() && indicates_dialect_unsupported(result.http_status,
                                                      result.error.value_or("")))
        note_dialect_rejected(provider_id, model, Dialect::Responses);

    return result;
}

} // namespace agentty::provider::openai
