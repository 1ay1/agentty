#pragma once
// agentty catalog — describes an LLM the user can select.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agentty/domain/id.hpp"

namespace agentty {

struct ModelInfo {
    ModelId     id;
    std::string display_name;
    std::string provider;
    int  context_window = 200000;
    bool favorite       = false;
    // Ollama-specific: the model reports "tools" in its capabilities list.
    // When false (or unset), agentty skips advertising tools entirely —
    // the model can only be used for plain chat. Set by list_models() via
    // Ollama's /api/show probe. std::optional so unknown = std::nullopt.
    std::optional<bool> supports_tools;
};

// ============================================================================
// ModelCapabilities — typed knowledge about a model derived from its id.
// ============================================================================
//
// Wire-level decisions (which beta headers to send, which color to paint,
// what the context-window cap is) all depend on what model the user
// picked. The provider doesn't expose a capability probe — `/v1/models`
// returns ids and display metadata, not "this model accepts the
// fine-grained-streaming beta" — so we infer the capabilities from the
// model id string. Centralised here so every site that asks "is this
// Sonnet 4?" reads from the same decoded value, and adding support for
// a new generation ("claude-haiku-5-…") only touches `decode()` rather
// than every if-substring check across the runtime.
//
// Decoding strategy: tokenise on '-' rather than substring matching.
// Anthropic ids follow `claude-{family}-{generation}-{revision}[-{date}]`,
// so a positional tokeniser stays robust as the catalog grows. The old
// `model.find("opus-4")` / `model.find("haiku-4")` scheme silently
// stopped recognising the generation the moment a `-5-` model shipped;
// with tokens we read the integer after `family` and the >= 4 check
// keeps working without source edits.
//
// Limitation: this is still inference, not a contract from upstream. If
// Anthropic restructures the id schema (drops the `claude-` prefix,
// inserts a tag between family and generation, etc.) the decoder needs
// a corresponding update — but at a single, structurally explicit site
// rather than scattered substring checks.
struct ModelCapabilities {
    enum class Family : std::uint8_t { Unknown, Haiku, Sonnet, Opus };

    Family family = Family::Unknown;
    // Generation extracted as an int. 0 = unknown / pre-4. Use the
    // numeric value when a downstream cares about the specific
    // generation; the convenience flag below covers the common
    // "are we on Claude 4+ wire?" case.
    int  generation = 0;
    // Pre-decoded "Claude 4-or-later" — the threshold the wire uses to
    // decide whether to send the context-management beta header.
    bool generation_4_or_later = false;
    // Minor/revision token: the integer immediately after the generation
    // (e.g. `opus-4-8` → generation 4, revision 8). 0 = unknown. Lets the
    // wire tell 4.5 from 4.8, which the effort-capability gates below need.
    int  revision = 0;
    // agentty-internal: user opted into the 1M-context-window beta. The
    // tag is `[1m]` appended to the model id at selection time; the
    // upstream id has no such suffix.
    bool extended_context_1m   = false;

    // Heuristic: this model is UNRELIABLE at structured tool-calling and
    // tends to over-call / leak tool JSON into prose (weak local models).
    // Drives the slim decision-first system prompt, the doom-loop guard,
    // and the tool-suppressed retry. Strong hosted models (any known
    // Claude family) and tool-trained local families are NOT weak.
    // Inference lives entirely in from_id (no network probe exists).
    bool weak_tool_use = false;

    [[nodiscard]] constexpr bool is_haiku()  const noexcept { return family == Family::Haiku; }
    [[nodiscard]] constexpr bool is_sonnet() const noexcept { return family == Family::Sonnet; }
    [[nodiscard]] constexpr bool is_opus()   const noexcept { return family == Family::Opus; }
    [[nodiscard]] constexpr bool is_known_family() const noexcept {
        return family != Family::Unknown;
    }
    // True when this model needs the weak-model guards (slim prompt,
    // doom-loop cap, tool-suppressed retry). See infer_weak_tool_use.
    [[nodiscard]] constexpr bool is_weak_tool_user() const noexcept {
        return weak_tool_use;
    }

    // ── Effort (output_config.effort) capability gates ───────────────────
    // Effort is GA on Opus 4.5+ and Sonnet 4.6+; it 400s on Sonnet 4.5,
    // Haiku, and any pre-4 model. `max` lands on Opus 4.6+ / Sonnet 4.6;
    // `xhigh` shipped with Opus 4.7 (Opus only). Gates read the decoded
    // family + generation + revision so a new id only updates from_id().
    [[nodiscard]] constexpr bool supports_effort() const noexcept {
        if (family == Family::Opus)
            return generation > 4 || (generation == 4 && revision >= 5);
        if (family == Family::Sonnet)
            return generation > 4 || (generation == 4 && revision >= 6);
        return false;
    }
    [[nodiscard]] constexpr bool supports_effort_max() const noexcept {
        if (!supports_effort()) return false;
        if (family == Family::Opus)
            return generation > 4 || (generation == 4 && revision >= 6);
        return true;  // any effort-capable Sonnet (4.6+) also takes `max`
    }
    [[nodiscard]] constexpr bool supports_effort_xhigh() const noexcept {
        if (!supports_effort()) return false;
        return family == Family::Opus
            && (generation > 4 || (generation == 4 && revision >= 7));
    }

    // Decode an id string. Pure / noexcept / branchless on the hot path.
    // No allocations — the tokeniser uses string_view splits in place.
    [[nodiscard]] static constexpr ModelCapabilities from_id(std::string_view id) noexcept {
        ModelCapabilities caps{};

        // Strip the `[1m]` extended-context suffix. agentty appends this
        // when the user picks a 1M-window variant; the upstream id
        // doesn't carry it.
        if (auto pos = id.find("[1m]"); pos != std::string_view::npos) {
            caps.extended_context_1m = true;
            id = id.substr(0, pos);
        }

        // Tokenise on '-'. Family lives at any token equal to "haiku"
        // / "sonnet" / "opus"; generation is the integer-parseable
        // token immediately following.
        std::string_view prev{};
        std::size_t start = 0;
        // True for the token immediately following the generation token —
        // that's the revision (`opus-4-8` → revision 8). Reset by any other
        // token so a later stray integer (a date, a size tag) isn't misread.
        bool expect_revision = false;
        for (std::size_t i = 0; i <= id.size(); ++i) {
            const bool boundary = (i == id.size() || id[i] == '-');
            if (!boundary) continue;
            if (i > start) {
                std::string_view tok = id.substr(start, i - start);
                const bool was_expecting_revision = expect_revision;
                expect_revision = false;
                if (tok == "haiku")       caps.family = Family::Haiku;
                else if (tok == "sonnet") caps.family = Family::Sonnet;
                else if (tok == "opus")   caps.family = Family::Opus;
                else if (was_expecting_revision) {
                    // Revision token — same 1-/2-digit plausibility check as
                    // the generation parse so a date can't slip through.
                    int r = 0;
                    bool ok = !tok.empty() && tok.size() <= 2;
                    for (char c : tok) {
                        if (c < '0' || c > '9') { ok = false; break; }
                        r = r * 10 + (c - '0');
                    }
                    if (ok) caps.revision = r;
                }
                else if (prev == "haiku" || prev == "sonnet" || prev == "opus") {
                    // Generation token — parse as int (no allocations). Only
                    // the NEW id schema puts the generation right after the
                    // family (`claude-sonnet-4-5-...`). The LEGACY schema
                    // (`claude-3-5-sonnet-20241022`) puts a date there
                    // instead; an 8-digit date must NOT be read as
                    // generation 20241022 (which would falsely look like a
                    // 4-or-later model). Reject any token that isn't a
                    // plausible 1- or 2-digit generation.
                    int g = 0;
                    bool ok = !tok.empty() && tok.size() <= 2;
                    for (char c : tok) {
                        if (c < '0' || c > '9') { ok = false; break; }
                        g = g * 10 + (c - '0');
                    }
                    if (ok) {
                        caps.generation = g;
                        caps.generation_4_or_later = (g >= 4);
                        expect_revision = true;  // next int token is the revision
                    }
                }
                prev = tok;
            }
            start = i + 1;
        }
        caps.weak_tool_use = infer_weak_tool_use(id, caps);
        return caps;
    }

private:
    // Decide whether a model is weak at tool-calling from its id alone.
    //
    // Strong (NOT weak):
    //   - Any known Claude family (hosted, excellent tool use).
    //   - Local families with native/trained tool-calling: qwen3, llama3.1,
    //     llama3.3, mistral / mixtral / ministral, command-r, hermes,
    //     firefunction, functionary, devstral, codestral, gpt-oss, granite,
    //     glm-4, deepseek (v3/r1).
    //   - Any model >= ~14B parameters (large enough to follow tool schemas).
    //
    // Weak (treat with guards):
    //   - Small local models (<= ~8B params: 7b, 3b, 1.5b, etc.).
    //   - Older / coder-only small families that leak tool JSON (qwen2.5,
    //     codellama, deepseek-coder, phi, gemma <= 9b, starcoder, stable-code).
    //   - Unknown ids default to NOT weak (assume capable) UNLESS the id
    //     carries a small-parameter tag.
    [[nodiscard]] static constexpr bool infer_weak_tool_use(
            std::string_view id, const ModelCapabilities& caps) noexcept {
        // Known Claude family → always strong.
        if (caps.is_known_family()) return false;

        auto contains = [](std::string_view hay, std::string_view needle) {
            if (needle.size() > hay.size()) return false;
            for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
                bool eq = true;
                for (std::size_t j = 0; j < needle.size(); ++j) {
                    char a = hay[i + j], b = needle[j];
                    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
                    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + 32);
                    if (a != b) { eq = false; break; }
                }
                if (eq) return true;
            }
            return false;
        };

        // Parameter size in billions, parsed from a `<N>[.<M>]b` token (e.g.
        // qwen2.5-coder:7b, llama3.1:70b, mistral-small:24b, phi3:3.8b). The
        // integer part is used (floor) — 1.7b counts as 1, 6.7b as 6. 0 =
        // unknown. We scan for 'b'/'B' preceded by a numeric run (with an
        // optional single '.' fraction) that starts at a separator.
        int params_b = 0;
        for (std::size_t i = 0; i < id.size(); ++i) {
            const char c = id[i];
            const bool is_b = (c == 'b' || c == 'B');
            if (!is_b || i == 0) continue;
            // char after 'b' must not be a letter (so "bf16" etc. is skipped)
            if (i + 1 < id.size()) {
                char n = id[i + 1];
                if ((n >= 'a' && n <= 'z') || (n >= 'A' && n <= 'Z')) continue;
            }
            // Walk back over the numeric token: digits, with at most one '.'.
            std::size_t d = i;
            bool seen_dot = false, seen_digit = false;
            while (d > 0) {
                char p = id[d - 1];
                if (p >= '0' && p <= '9') { seen_digit = true; --d; }
                else if (p == '.' && !seen_dot) { seen_dot = true; --d; }
                else break;
            }
            if (!seen_digit) continue;            // no number before 'b'
            // char before the numeric run must be a separator, not a letter
            if (d > 0) {
                char p = id[d - 1];
                bool sep = !((p >= 'a' && p <= 'z') || (p >= 'A' && p <= 'Z'));
                if (!sep) continue;
            }
            // Parse the INTEGER part only (everything before the first '.').
            int v = 0;
            for (std::size_t k = d; k < i; ++k) {
                if (id[k] == '.') break;
                v = v * 10 + (id[k] - '0');
            }
            if (v > params_b) params_b = v;       // take the largest match
        }

        // Tool-trained / instruction-strong local families → strong even at
        // smaller sizes.
        const bool strong_family =
            contains(id, "qwen3")        || contains(id, "llama3.1")   ||
            contains(id, "llama-3.1")    || contains(id, "llama3.3")   ||
            contains(id, "llama-3.3")    || contains(id, "mistral")    ||
            contains(id, "mixtral")      || contains(id, "ministral")  ||
            contains(id, "command-r")    || contains(id, "hermes")     ||
            contains(id, "firefunction") || contains(id, "functionary")||
            contains(id, "devstral")     || contains(id, "codestral")  ||
            contains(id, "gpt-oss")      || contains(id, "granite")    ||
            contains(id, "glm-4")        || contains(id, "deepseek-v3")||
            contains(id, "deepseek-r1");
        // A strong family at >= ~7B is reliable; only flag it weak if it's
        // explicitly tiny (<= 3B), where even good families struggle.
        if (strong_family) return params_b != 0 && params_b <= 3;

        // Known weak / coder-only / small families that leak tool JSON.
        const bool weak_family =
            contains(id, "qwen2.5")      || contains(id, "qwen2")      ||
            contains(id, "codellama")    || contains(id, "code-llama") ||
            contains(id, "deepseek-coder")|| contains(id, "starcoder") ||
            contains(id, "stable-code")  || contains(id, "phi")        ||
            contains(id, "gemma")        || contains(id, "tinyllama")  ||
            contains(id, "smollm")       || contains(id, "codegemma")  ||
            contains(id, "sqlcoder");
        if (weak_family) return true;

        // Large models (no weak-family signal) follow tool schemas reliably.
        if (params_b >= 14) return false;

        // No family signal: rely on size. <= 8B → weak; otherwise assume the
        // model (or hosted endpoint, unknown id) is capable.
        if (params_b != 0 && params_b <= 8) return true;
        return false;
    }
};

// Convenience: infer weak-tool-use straight from a model id string.
// Used by the provider/runtime paths that only hold the id, not the caps.
[[nodiscard]] inline bool is_weak_model(std::string_view model_id) noexcept {
    return ModelCapabilities::from_id(model_id).is_weak_tool_user();
}

// ============================================================================
// Effort — user-selectable reasoning/spend tier (output_config.effort).
// ============================================================================
// `None` sends nothing — preserving the default no-thinking, replay-safe
// wire. Any other level makes the Claude provider send adaptive thinking +
// the matching `output_config.effort`. Selectable live from the model picker.
enum class Effort : std::uint8_t { None, Low, Medium, High, Xhigh, Max };

// Wire value for output_config.effort. None → "" (the field is omitted).
[[nodiscard]] constexpr std::string_view effort_wire(Effort e) noexcept {
    switch (e) {
        case Effort::None:   return "";
        case Effort::Low:    return "low";
        case Effort::Medium: return "medium";
        case Effort::High:   return "high";
        case Effort::Xhigh:  return "xhigh";
        case Effort::Max:    return "max";
    }
    return "";
}

// Short label for the picker UI (None renders as "off").
[[nodiscard]] constexpr std::string_view effort_label(Effort e) noexcept {
    return e == Effort::None ? std::string_view{"off"} : effort_wire(e);
}

// Parse a persisted wire value back to Effort. Unknown / "" → None.
[[nodiscard]] constexpr Effort effort_from_wire(std::string_view s) noexcept {
    if (s == "low")    return Effort::Low;
    if (s == "medium") return Effort::Medium;
    if (s == "high")   return Effort::High;
    if (s == "xhigh")  return Effort::Xhigh;
    if (s == "max")    return Effort::Max;
    return Effort::None;
}

// Clamp an Effort to what a model actually supports and return its wire
// value. "" when the model can't take effort at all (or e == None). The
// provider calls this so a stale high pick (e.g. Xhigh chosen, then a swap
// to a model without xhigh) silently degrades to `high` instead of 400ing.
[[nodiscard]] inline std::string_view effort_wire_for(
        Effort e, const ModelCapabilities& caps) noexcept {
    if (e == Effort::None || !caps.supports_effort()) return "";
    if (e == Effort::Max   && !caps.supports_effort_max())   e = Effort::High;
    if (e == Effort::Xhigh && !caps.supports_effort_xhigh()) e = Effort::High;
    return effort_wire(e);
}

// Ordered efforts the user may cycle for a given model: always
// {None, Low, Medium, High}, plus Xhigh / Max where supported. The picker
// cycles within this so the user never lands on a level the model 400s on.
[[nodiscard]] inline std::vector<Effort> available_efforts(
        const ModelCapabilities& caps) {
    std::vector<Effort> out{Effort::None, Effort::Low,
                            Effort::Medium, Effort::High};
    if (caps.supports_effort_xhigh()) out.push_back(Effort::Xhigh);
    if (caps.supports_effort_max())   out.push_back(Effort::Max);
    return out;
}

// Step `cur` by `delta` (wrapping) within a model's available efforts.
// Returns None when the model doesn't support effort at all.
[[nodiscard]] inline Effort cycle_effort(
        Effort cur, int delta, const ModelCapabilities& caps) {
    if (!caps.supports_effort()) return Effort::None;
    const auto list = available_efforts(caps);
    const int n = static_cast<int>(list.size());
    int idx = 0;
    for (int i = 0; i < n; ++i)
        if (list[static_cast<std::size_t>(i)] == cur) { idx = i; break; }
    idx = ((idx + delta) % n + n) % n;
    return list[static_cast<std::size_t>(idx)];
}

// Per-model max OUTPUT-token budget for a normal turn.
//
// WHY this exists: the global default (provider::kSafeMaxTokens = 16384) is
// shared across a whole turn — reasoning prose AND the tool-call JSON come out
// of the same budget. An `edit` is the worst case: the model reproduces the
// existing `old_text` VERBATIM (for the fuzzy match) plus the `new_text`, so a
// single edit can be ~2x the tokens of a `write` over the same span. When the
// reasoning + the edit JSON together overrun 16384, the provider stops the
// stream mid-`input_json` with stop_reason=max_tokens and the args arrive
// truncated — surfacing as "Tool call arguments look incomplete". Subagents
// already dodge this (task.cpp hard-codes 32000); the main turn never did.
//
// ROBUSTNESS: we set the ceiling HIGH, matching what shipping agents do rather
// than guessing conservatively — a too-low cap silently truncates edits, which
// is the failure we're fixing. References (verified against real source):
//   • Claude Code  default 32000 for Sonnet/Opus, raisable via
//                  CLAUDE_CODE_MAX_OUTPUT_TOKENS; Opus 4.6 → 64k–128k.
//   • Aider        Claude 3.7 Sonnet → 64000 (output-128k beta header);
//                  3.5 Sonnet/Haiku → 8192; DeepSeek-V3 → 128000.
// Modern Claude 4.x Sonnet/Opus officially support 64k output tokens, so that
// is our default for the family. Older 3.5 Haiku/Sonnet cap at 8k; we detect
// the legacy ids and clamp so we never request more than the model allows
// (Anthropic 400s a max_tokens above the model ceiling).
//
// OVERRIDE: AGENTTY_MAX_OUTPUT_TOKENS (mirrors Claude Code's env knob). A
// positive integer wins for every model — the escape hatch for a user on a
// model/endpoint we don't have hard-coded, or who wants 128k beta output.
[[nodiscard]] inline int max_output_tokens_for(std::string_view model_id) noexcept {
    if (const char* env = std::getenv("AGENTTY_MAX_OUTPUT_TOKENS")) {
        int v = 0;
        for (const char* p = env; *p >= '0' && *p <= '9'; ++p) v = v * 10 + (*p - '0');
        if (v > 0) return v;
    }

    const auto caps = ModelCapabilities::from_id(model_id);
    if (caps.is_known_family()) {
        // Claude family. Generation drives the ceiling:
        //   4.x Sonnet/Opus  -> 64000 (officially supported output ceiling)
        //   Haiku (any gen)  -> 8192  (Haiku's real output cap is 8k)
        //   <= 3.x           -> 8192  (older models 400 above this)
        //   unknown gen      -> 16384 (roomy but universally accepted)
        if (caps.is_haiku()) return 8192;
        if (caps.generation >= 4) return 64000;
        // Legacy schema (`claude-3-5-sonnet-...`, `claude-3-opus-...`) puts
        // the generation BEFORE the family, so caps.generation is 0 here.
        // Detect the `claude-3-` prefix and treat the whole 3.x Sonnet/Opus
        // line as 8k-capped — requesting more 400s on those models.
        if (model_id.find("claude-3") != std::string_view::npos) return 8192;
        if (caps.generation == 3) return 8192;
        return 16384;
    }

    // Non-Claude (local Ollama / OpenAI-compat / unknown). Maps onto
    // num_predict / max_tokens. Keep the conservative shared default —
    // already ~4x a typical edit, and weak local models don't emit huge
    // bodies.
    return 16384;
}

} // namespace agentty
