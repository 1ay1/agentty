#pragma once
// agentty::catalog — THE single bundled model floor for every provider.
//
// Historically each provider's offline "floor" list lived in a different
// place: seed_models() (init.cpp), the anthropic transport's inline fallback,
// selection.cpp's bundled_models_for(), and per-provider bundled_models()
// (chatgpt / copilot / kimi). Four+ hand-maintained lists that could (and did)
// drift — a model listed in one but not another is exactly how a newly-shipped
// flagship showed in one picker surface but not the fused one.
//
// This is the SINGLE SOURCE for all of them. Every site that needs a floor
// calls catalog::bundled(provider_id); the live /v1/models fetch still
// supersedes it (the floor is the floor, not the ceiling). Capabilities stay
// DERIVED from the id (ModelCapabilities::from_id) — this file only owns WHICH
// ids exist offline, never what they can do.
//
// Newest-first: front() is the sensible default when the user gives no -m.
// Providers whose real catalog is too large / user-defined to guess
// (openrouter, custom hosts, locals) intentionally return empty.

#include <string>
#include <string_view>
#include <vector>

#include "agentty/domain/catalog.hpp"

namespace agentty::catalog {

// Append the `[1m]` long-context companion for every suffix-capable model,
// right after its base row. Shared by the bundled floor and the live-fetch
// path so seed and catalog agree. Entitlement self-heals downstream
// (context_1m_blocked strips the rows if the account can't stream 1M).
inline void add_1m_variants(std::vector<ModelInfo>& v) {
    std::vector<ModelInfo> out;
    out.reserve(v.size() * 2);
    for (auto& mi : v) {
        const bool suffixable =
            ModelCapabilities::from_id(mi.id.value).supports_1m_suffix()
            && mi.id.value.find("[1m]") == std::string::npos;
        out.push_back(mi);
        if (suffixable) {
            ModelInfo one_m = mi;
            one_m.id = ModelId{mi.id.value + "[1m]"};
            one_m.display_name = mi.display_name + " (1M context)";
            one_m.context_window = 1'000'000;
            one_m.favorite = false;
            out.push_back(std::move(one_m));
        }
    }
    v = std::move(out);
}

// The offline floor for `provider_id` (== registry id / endpoint label),
// newest-first. Empty when there is no sensible guess.
[[nodiscard]] inline std::vector<ModelInfo> bundled(std::string_view provider_id) {
    auto mk = [&](const char* id, const char* name = nullptr,
                  int ctx = 200000, bool fav = false,
                  std::optional<bool> tools = std::nullopt) {
        return ModelInfo{ .id = ModelId{id},
                          .display_name = name ? name : id,
                          .provider = std::string{provider_id},
                          .context_window = ctx,
                          .favorite = fav,
                          .supports_tools = tools };
    };

    std::vector<ModelInfo> v;

    if (provider_id == "anthropic") {
        // Flagship lane (Fable/Mythos 5) sits above Opus; 4.5 line is the GA
        // floor. add_1m_variants() below supplies the `[1m]` companions.
        v = { mk("claude-fable-5",  "Claude Fable 5",  200000, true),
              mk("claude-opus-4-5",   "Claude Opus 4.5",   200000, true),
              mk("claude-sonnet-4-5", "Claude Sonnet 4.5", 200000, true),
              mk("claude-haiku-4-5",  "Claude Haiku 4.5",  200000, false) };
        add_1m_variants(v);
    } else if (provider_id == "chatgpt") {
        // Only slugs a Codex-enabled ChatGPT account accepts on /responses.
        v = { mk("gpt-5", "GPT-5", 272000, false, true) };
    } else if (provider_id == "copilot") {
        v = { mk("gpt-4o"), mk("gpt-4.1"), mk("o4-mini"),
              mk("claude-sonnet-4"), mk("gemini-2.5-pro") };
    } else if (provider_id == "kimi") {
        // Kimi K2 ships a 256k window. Stated explicitly because Kimi's
        // /models payload carries no context length, so nothing overwrites
        // this later — leaving it on the 200k default silently under-reported
        // the window by 56k and made the context gauge (and compaction
        // threshold) fire early on every Kimi turn.
        constexpr int k256 = 262144;
        v = { mk("kimi-k2-turbo-preview", nullptr, k256),
              mk("kimi-k2-0905-preview", nullptr, k256),
              mk("kimi-k2-0711-preview", nullptr, k256) };
    } else if (provider_id == "xai") {
        v = { mk("grok-4.6"), mk("grok-4"), mk("grok-code-fast-1"),
              mk("grok-3"), mk("grok-3-mini") };
    } else if (provider_id == "mistral") {
        v = { mk("mistral-large-latest"), mk("magistral-medium-latest"),
              mk("codestral-latest"), mk("mistral-medium-latest"),
              mk("mistral-small-latest") };
    } else if (provider_id == "gemini") {
        v = { mk("gemini-2.5-pro"), mk("gemini-2.5-flash"),
              mk("gemini-2.5-flash-lite"), mk("gemini-2.0-flash") };
    } else if (provider_id == "fireworks") {
        v = { mk("accounts/fireworks/models/kimi-k2-instruct"),
              mk("accounts/fireworks/models/deepseek-v3"),
              mk("accounts/fireworks/models/qwen3-235b-a22b"),
              mk("accounts/fireworks/models/llama-v3p3-70b-instruct") };
    } else if (provider_id == "deepseek") {
        v = { mk("deepseek-chat"), mk("deepseek-reasoner"),
              mk("deepseek-v4-pro"), mk("deepseek-v4-flash") };
    } else if (provider_id == "groq") {
        v = { mk("llama-3.3-70b-versatile"), mk("moonshotai/kimi-k2-instruct"),
              mk("qwen/qwen3-32b"), mk("llama-3.1-8b-instant") };
    } else if (provider_id == "cerebras") {
        v = { mk("llama-3.3-70b"), mk("qwen-3-235b-a22b-instruct-2507"),
              mk("llama3.1-8b") };
    } else if (provider_id == "together") {
        v = { mk("deepseek-ai/DeepSeek-V3"),
              mk("meta-llama/Llama-3.3-70B-Instruct-Turbo"),
              mk("Qwen/Qwen3-235B-A22B-Instruct-2507-tput") };
    }
    // openrouter, custom hosts, and locals have no floor — too large /
    // user-defined to guess; they stay empty until the live fetch lands.
    return v;
}

} // namespace agentty::catalog
