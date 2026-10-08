#pragma once
// The OpenAI-compatible endpoint description, on its own so Selection (and
// through it the Model) can hold one without pulling in the transport, which
// includes msg.hpp and so the Model itself.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agentty::provider::openai {

// Endpoint description for an OpenAI-compatible backend. Constructed once at
// startup (from settings / env / --provider) and threaded into every request.
//
//   host        — TLS SNI + Host header + cert pin (e.g. "api.openai.com").
//   port        — usually 443; Ollama default is 11434 over plain HTTP.
//   path        — chat completions path, default "/v1/chat/completions".
//   models_path — model listing path, default "/v1/models".
//   use_tls     — false for a local http:// Ollama / llama.cpp server.
//   label       — provider tag stamped onto ModelInfo (for the picker).
struct Endpoint {
    std::string host        = "api.openai.com";
    std::uint16_t port      = 443;
    std::string path        = "/v1/chat/completions";
    std::string models_path = "/v1/models";
    bool        use_tls     = true;
    std::string label       = "openai";
    // When true, use Ollama's NATIVE /api/chat protocol (NDJSON stream,
    // structured message.tool_calls) instead of the OpenAI-compat
    // /v1/chat/completions shim. The shim makes weak local models leak
    // tool calls as raw JSON in `content` (even on a bare "hi"); the native
    // endpoint applies the model's chat template and answers cleanly. Set
    // for the "ollama" preset only.
    bool        native_api  = false;
    // Custom auth header NAME (e.g. "X-API-Key") for gateways that don't
    // accept `Authorization: Bearer`. Empty (the default) keeps the standard
    // bearer header. When set, the key goes out raw as `<name>: <key>` —
    // no "Bearer " prefix. Populated from --auth-header via
    // provider::parse_selection.
    std::string auth_header_name;

    // Extra static request headers injected verbatim on EVERY request to this
    // endpoint (after the auth header). GitHub Copilot needs its editor-
    // identification block (Copilot-Integration-Id / Editor-Version / …) on
    // every call or the proxy returns 400/403. Empty for the plain OpenAI
    // family. Names should be lowercase (header names are case-insensitive and
    // the rest of agentty sends lowercase).
    std::vector<std::pair<std::string, std::string>> extra_headers;

    // Built-in presets for the common free / hosted backends. Pass a bare
    // name ("openai", "groq", "openrouter", "together", "cerebras",
    // "ollama") or a full "host[:port]" string.
    [[nodiscard]] static Endpoint from_spec(std::string_view spec);

    // Structural equality, so the seam can publish the Model's selection
    // only when it changed.
    [[nodiscard]] bool operator==(const Endpoint&) const = default;
};

} // namespace agentty::provider::openai
