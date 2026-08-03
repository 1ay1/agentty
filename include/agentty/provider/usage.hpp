#pragma once
// agentty::provider::usage — the shared token-usage extractors that every
// OpenAI-family transport used to open-code.
//
// Each streaming provider reports token counts in its own wire shape, but
// there are only THREE distinct shapes across the whole codebase and several
// of them were copy-pasted between transports (the Ollama-native
// `prompt_eval_count`/`eval_count` pair appeared THREE times — once in the
// openai transport's native path and twice in the ollama transport). A tweak
// to any of them (e.g. surfacing a new cached-token field) had to be mirrored
// by hand or silently drift. Now each shape is parsed in exactly one place.
//
// All three map onto the single StreamUsage struct (runtime/msg.hpp). They
// return std::nullopt when the object carries no token counts at all, so the
// caller can preserve its existing "only sink if non-empty" guard:
//
//     if (auto su = usage::from_openai(j["usage"])) ctx.sink(*su);
//
// SCOPE — parsing only. The extractors know nothing about framing, stop
// reasons, or the sink; they turn one JSON object into an optional
// StreamUsage. Anthropic's native shape stays inline in its transport because
// StreamUsage's field names ARE the Anthropic wire names (input_tokens /
// output_tokens / cache_creation_input_tokens / cache_read_input_tokens) — a
// direct `.value()` read with no translation, so a helper would only add
// indirection.

#include <optional>

#include <nlohmann/json.hpp>

#include "agentty/runtime/msg.hpp"

namespace agentty::provider::usage {

using json = nlohmann::json;

// True when a StreamUsage carries at least one non-zero count — the shared
// "worth sinking" test the transports applied by hand.
[[nodiscard]] inline bool any(const StreamUsage& su) noexcept {
    return su.input_tokens || su.output_tokens
        || su.cache_creation_input_tokens || su.cache_read_input_tokens;
}

// OpenAI /v1/chat/completions `usage` object:
//   {"prompt_tokens":N, "completion_tokens":N,
//    "prompt_tokens_details":{"cached_tokens":N}}
// prompt_tokens_details.cached_tokens ≈ Anthropic's cache_read — surface it so
// the context gauge reflects cache hits. Used by the whole OpenAI-compat
// family (OpenAI/Groq/OpenRouter/Together/Cerebras/llama.cpp).
[[nodiscard]] inline std::optional<StreamUsage> from_openai(const json& u) {
    if (!u.is_object()) return std::nullopt;
    StreamUsage su;
    su.input_tokens  = u.value("prompt_tokens", 0);
    su.output_tokens = u.value("completion_tokens", 0);
    if (u.contains("prompt_tokens_details")
        && u["prompt_tokens_details"].is_object()) {
        su.cache_read_input_tokens =
            u["prompt_tokens_details"].value("cached_tokens", 0);
    }
    return any(su) ? std::optional{su} : std::nullopt;
}

// OpenAI Responses API `usage` object (ChatGPT/Codex path):
//   {"input_tokens":N, "output_tokens":N,
//    "input_tokens_details":{"cached_tokens":N}}
// Same semantics as the chat shape but the field names match Anthropic's
// input/output naming; the cached-token detail lives under input_tokens_details.
[[nodiscard]] inline std::optional<StreamUsage> from_responses(const json& u) {
    if (!u.is_object()) return std::nullopt;
    StreamUsage su;
    su.input_tokens  = u.value("input_tokens", 0);
    su.output_tokens = u.value("output_tokens", 0);
    if (u.contains("input_tokens_details")
        && u["input_tokens_details"].is_object()) {
        su.cache_read_input_tokens =
            u["input_tokens_details"].value("cached_tokens", 0);
    }
    return any(su) ? std::optional{su} : std::nullopt;
}

// Ollama native /api/chat final frame:
//   {"done":true, "prompt_eval_count":N, "eval_count":N, ...}
// The counts are top-level fields on the DONE frame, not a nested `usage`
// object — so this reads from the frame directly. No cache-token concept
// (local KV cache is prefix-automatic and unreported).
[[nodiscard]] inline std::optional<StreamUsage> from_ollama(const json& frame) {
    if (!frame.is_object()) return std::nullopt;
    StreamUsage su;
    su.input_tokens  = frame.value("prompt_eval_count", 0);
    su.output_tokens = frame.value("eval_count", 0);
    return any(su) ? std::optional{su} : std::nullopt;
}

} // namespace agentty::provider::usage
