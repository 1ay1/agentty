#pragma once
// moha catalog — describes an LLM the user can select.

#include <cstddef>
#include <string>
#include <string_view>

#include "moha/domain/id.hpp"

namespace moha {

struct ModelInfo {
    ModelId     id;
    std::string display_name;
    std::string provider;
    int  context_window = 200000;
    bool favorite       = false;
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
    // moha-internal: user opted into the 1M-context-window beta. The
    // tag is `[1m]` appended to the model id at selection time; the
    // upstream id has no such suffix.
    bool extended_context_1m   = false;

    [[nodiscard]] constexpr bool is_haiku()  const noexcept { return family == Family::Haiku; }
    [[nodiscard]] constexpr bool is_sonnet() const noexcept { return family == Family::Sonnet; }
    [[nodiscard]] constexpr bool is_opus()   const noexcept { return family == Family::Opus; }
    [[nodiscard]] constexpr bool is_known_family() const noexcept {
        return family != Family::Unknown;
    }

    // Decode an id string. Pure / noexcept / branchless on the hot path.
    // No allocations — the tokeniser uses string_view splits in place.
    [[nodiscard]] static constexpr ModelCapabilities from_id(std::string_view id) noexcept {
        ModelCapabilities caps{};

        // Strip the `[1m]` extended-context suffix. moha appends this
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
        for (std::size_t i = 0; i <= id.size(); ++i) {
            const bool boundary = (i == id.size() || id[i] == '-');
            if (!boundary) continue;
            if (i > start) {
                std::string_view tok = id.substr(start, i - start);
                if (tok == "haiku")       caps.family = Family::Haiku;
                else if (tok == "sonnet") caps.family = Family::Sonnet;
                else if (tok == "opus")   caps.family = Family::Opus;
                else if (prev == "haiku" || prev == "sonnet" || prev == "opus") {
                    // Generation token — parse as int (no allocations).
                    int g = 0;
                    bool ok = !tok.empty();
                    for (char c : tok) {
                        if (c < '0' || c > '9') { ok = false; break; }
                        g = g * 10 + (c - '0');
                    }
                    if (ok) {
                        caps.generation = g;
                        caps.generation_4_or_later = (g >= 4);
                    }
                }
                prev = tok;
            }
            start = i + 1;
        }
        return caps;
    }
};

} // namespace moha
