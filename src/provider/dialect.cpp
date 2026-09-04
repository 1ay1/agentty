// agentty::provider — dialect selection. See dialect.hpp for the rationale.

#include "agentty/provider/dialect.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include "agentty/provider/registry.hpp"
#include "agentty/util/logx.hpp"

namespace agentty::provider {
namespace {

// Case/punctuation-insensitive family matching. Model slugs arrive in many
// shapes for the SAME family — "gpt-5", "openai/gpt-5-mini", "gpt-5.4-2026",
// "GPT-5" — so match on a normalised form rather than a literal prefix.
[[nodiscard]] std::string normalise(std::string_view id) {
    std::string out;
    out.reserve(id.size());
    // Drop any vendor prefix ("openai/gpt-5" → "gpt-5"): OpenRouter and
    // gateways namespace their slugs, the family is the part after the slash.
    if (const auto slash = id.rfind('/'); slash != std::string_view::npos)
        id.remove_prefix(slash + 1);
    for (const char c : id)
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    return out;
}

[[nodiscard]] bool starts_with(std::string_view s, std::string_view p) noexcept {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

// The GPT-5 generation number, if this is a gpt-5 model: "gpt-5.4-mini" → 54,
// "gpt-5" → 50, "gpt-5-mini" → 50. Returns -1 for non-gpt-5 slugs.
//
// Needed because the chat tool-calling cutoff is a POINT RELEASE (5.4), not a
// whole family: gpt-5 and gpt-5.1 still tool-call on chat, 5.4+ does not.
[[nodiscard]] int gpt5_generation(std::string_view m) noexcept {
    if (!starts_with(m, "gpt-5")) return -1;
    auto rest = m.substr(5);
    if (rest.empty() || rest.front() != '.') return 50;   // plain "gpt-5*"
    rest.remove_prefix(1);
    int minor = 0;
    std::size_t i = 0;
    for (; i < rest.size() && std::isdigit(static_cast<unsigned char>(rest[i])); ++i)
        minor = minor * 10 + (rest[i] - '0');
    if (i == 0) return 50;
    return 50 + std::min(minor, 9);
}

// ── Learned rejections ───────────────────────────────────────────────────
using Key = std::pair<std::string, std::string>;   // (provider, model)

std::mutex& obs_mu() { static std::mutex m; return m; }
std::set<Key>& rejected_responses() { static std::set<Key> s; return s; }
std::set<Key>& rejected_chat()      { static std::set<Key> s; return s; }

[[nodiscard]] bool contains(std::set<Key>& s, std::string_view p, std::string_view m) {
    return s.count(Key{std::string{p}, std::string{m}}) > 0;
}

} // namespace

bool model_requires_responses(std::string_view model) noexcept {
    const std::string m = normalise(model);
    if (m.empty()) return false;

    // ── GPT-5.4 and later ────────────────────────────────────────────────
    // OpenAI's reasoning guide: "Starting with GPT-5.4, Chat Completions does
    // not support tool calling with reasoning_effort values other than none."
    // agentty always sends tools, so for an agent this is a hard requirement.
    if (const int gen = gpt5_generation(m); gen >= 54) return true;

    // ── GPT-6 / Astra generation ─────────────────────────────────────────
    // "Use the Responses API for function calling. Chat Completions does not
    // support function calling with GPT-6 Astra." No effort caveat: the chat
    // dialect simply cannot carry an agent turn for this family.
    if (starts_with(m, "gpt-6") || m.find("astra") != std::string::npos)
        return true;

    // ── Copilot's mai-code-* ─────────────────────────────────────────────
    // MEASURED against a live Auto session: chat answers 400
    // unsupported_api_for_model. Responses is the only way to reach these.
    if (starts_with(m, "mai-code")) return true;

    // Earlier reasoning families (gpt-5.0/5.1, o1, o3, o4) remain LEGAL on
    // chat — they just can't show their thinking there. They're handled as a
    // preference below, not a requirement, so a chat-only gateway still works.
    return false;
}

bool model_emits_reasoning_summaries(std::string_view model) noexcept {
    const std::string m = normalise(model);
    if (m.empty()) return false;
    // MEASURED: gpt-5* on /responses streams real
    // response.reasoning_summary_text.delta frames. The o-series and the
    // gpt-6 line are reasoning models on the same dialect and behave alike.
    if (gpt5_generation(m) >= 0) return true;
    if (starts_with(m, "gpt-6") || m.find("astra") != std::string::npos) return true;
    if (starts_with(m, "o1") || starts_with(m, "o3") || starts_with(m, "o4")) return true;
    if (starts_with(m, "mai-code")) return true;
    // Explicitly NOT claimed: claude-* and gpt-4.x return no summaries even
    // when a host will accept them on Responses. Claiming otherwise is what
    // makes the thinking pane spin forever on an empty channel.
    return false;
}

Dialect dialect_for(std::string_view provider_id, std::string_view model) noexcept {
    const auto* row = preset_for(provider_id);

    // Rows on a non-OpenAI dialect answer structurally: Anthropic speaks
    // Messages, ACP is a subprocess, Ollama's native API is its own protocol.
    if (row) {
        if (row->wire == Wire::AnthropicMessages || row->wire == Wire::Acp)
            return Dialect::Native;
        if (row->native_api) return Dialect::Native;
        // A row whose DEFAULT is Responses has no second dialect to choose
        // between (endpoints_consistent forbids it) — e.g. ChatGPT/Codex.
        if (row->wire == Wire::OpenAIResponses) return Dialect::Responses;
        // No Responses endpoint advertised → nothing to route to. This is the
        // answer for DeepSeek, Mistral, Groq, xAI, Together, Cerebras and a
        // custom llama.cpp host: they carry reasoning over chat's
        // `reasoning_content` and have no /responses to offer.
        if (row->responses_path.empty()) return Dialect::Chat;
    } else {
        // Unknown/custom host (a user's own OpenAI-compatible base URL). We
        // cannot know it has /responses, and guessing wrong costs a failed
        // turn on a host we can't probe cheaply. Stay on chat.
        return Dialect::Chat;
    }

    // ── The row CAN do both. Now the model decides. ──────────────────────
    if (model.empty()) {
        // Provider-level question ("could anything here use Responses?").
        return Dialect::Responses;
    }

    const bool required = model_requires_responses(model);

    // Runtime evidence outranks the prior in BOTH directions.
    {
        std::scoped_lock lk(obs_mu());
        if (contains(rejected_responses(), provider_id, model)) {
            // The host told us no. Honour it even for a "required" model:
            // a failing turn on chat is strictly better than a certain 404,
            // and the user gets an error from the model rather than from us.
            return Dialect::Chat;
        }
        if (contains(rejected_chat(), provider_id, model))
            return Dialect::Responses;
    }

    if (required) return Dialect::Responses;

    // Not required, but preferred when the model has thinking worth showing
    // and the host can carry it: better tool use, better cache hits, and the
    // reasoning survives across tool rounds via encrypted_content.
    if (model_emits_reasoning_summaries(model)) return Dialect::Responses;

    // Everything else (gpt-4o, claude-* on Copilot, generic chat models)
    // stays on the dialect its row was built for. Dragging a chat-only family
    // onto Responses 400s — that regression would break every Claude turn on
    // Copilot, which is why this default is conservative.
    return Dialect::Chat;
}

bool streams_reasoning_text(std::string_view provider_id,
                            std::string_view model) noexcept {
    const auto* row = preset_for(provider_id);
    if (!row) return true;   // unknown/custom host: assume yes, as before.

    if (row->wire == Wire::AnthropicMessages) return true;
    if (row->wire == Wire::Acp) return false;

    // The ONE authority: ask where the turn will actually be sent, then
    // answer for THAT dialect. This is the join that used to be a second
    // guess — the reason a mislabelled row could teach the UI to promise
    // reasoning the wire never delivers.
    switch (dialect_for(provider_id, model)) {
        case Dialect::Responses:
            // Responses carries reasoning summaries, but only for models that
            // actually produce them. Empty model = the provider-level "can
            // anything here?" question, which is a yes.
            return model.empty() || model_emits_reasoning_summaries(model);
        case Dialect::Native:
            // Ollama and friends: local reasoning models emit <think> spans,
            // which the transport routes unconditionally.
            return true;
        case Dialect::Chat:
            break;
    }

    // On chat, first-party OpenAI drops reasoning entirely — api.openai.com
    // does not transmit it on /chat/completions at any effort.
    if (provider_id == "openai") return false;
    // Copilot on chat likewise: claude-* and gpt-4.x return none there.
    if (provider_id == "copilot") return false;

    // Every other OpenAI-COMPAT host on this dialect (DeepSeek, Mistral,
    // OpenRouter's chat path, vLLM, Ollama) populates reasoning_content,
    // which the transport parses unconditionally.
    return true;
}

void note_dialect_rejected(std::string_view provider_id,
                           std::string_view model, Dialect rejected) noexcept {
    if (model.empty()) return;
    try {
        std::scoped_lock lk(obs_mu());
        auto& set = rejected == Dialect::Responses ? rejected_responses()
                                                   : rejected_chat();
        set.insert(Key{std::string{provider_id}, std::string{model}});
    } catch (...) {
        return;   // an allocation failure here must never break a turn.
    }
    AGT_LOG(Wire, Info, "provider.dialect",
            "{}/{} rejected {} — routing the other way",
            provider_id, model, dialect_name(rejected));
}

void reset_dialect_observations() noexcept {
    std::scoped_lock lk(obs_mu());
    rejected_responses().clear();
    rejected_chat().clear();
}

} // namespace agentty::provider
