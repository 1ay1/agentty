#pragma once
// agentty catalog — describes an LLM the user can select.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <atomic>
#include <shared_mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agentty/domain/id.hpp"
#include "agentty/domain/capkey.hpp"   // canonical capability-key discipline

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

    // Value equality (used to detect when the active provider's fused catalog
    // has drifted from available_models and needs a re-seed).
    [[nodiscard]] bool operator==(const ModelInfo&) const = default;
};

// A (provider, model) pair — the identity a cross-provider switch targets and
// the MRU/recents unit. Small, value-typed, and comparable so it can key the
// fused picker's selection and dedup the recents list.
struct ModelRef {
    std::string provider_id;   // "anthropic", "openai", "kimi", a host spec…
    std::string model_id;      // ModelId::value on the wire

    [[nodiscard]] bool operator==(const ModelRef& o) const noexcept {
        return provider_id == o.provider_id && model_id == o.model_id;
    }
    [[nodiscard]] bool empty() const noexcept {
        return provider_id.empty() && model_id.empty();
    }
};

// One authenticated provider's model catalog, held in the fused picker's
// merged view. `state` drives the per-provider group header (loading… /
// ready / failed); `models` is the list_models_for() result (which is
// non-empty even without auth thanks to the bundled seed, so a freshly
// added provider still shows rows immediately).
struct ProviderCatalog {
    std::string provider_id;
    std::string label;                    // provider display name
    enum class State : std::uint8_t { Idle, Loading, Ready, Failed };
    State state = State::Idle;
    std::vector<ModelInfo> models;
    std::string account_label;            // active account on this provider
    // Steady-clock ms when this catalog's LIVE fetch last completed (0 = never
    // fetched live; still showing the bundled seed). Drives the on-open
    // freshness check: a catalog older than the TTL (or Failed) is refetched
    // so the fused list stays current instead of freezing after its first load.
    std::int64_t loaded_at_ms = 0;
    // Derived per-model fuzzy haystacks, already lowercased, one per entry in
    // `models` (same order). Rebuilt once when `models` changes (size guard in
    // rebuild_fused_rows), so the per-KEYSTROKE filter never re-allocates or
    // re-lowercases a haystack for every model. Empty ⇒ not yet built (the
    // build falls back to composing the haystack inline).
    std::vector<std::string> search_keys;
    // Derived per-model FOLDED ROW IDENTITY (capkey::norm_row_id), same order
    // as `models`. The fused build's alias-dedup used to call norm_row_id on
    // every (candidate × seen) pair — O(n²) with two string allocations per
    // probe, ~200k allocs per keystroke on a 450-model aggregator list.
    // Folding each id ONCE per catalog change turns dedup into a hash-set
    // probe. Rebuilt alongside search_keys (same size guard).
    std::vector<std::string> row_keys;
    // Derived canonical display labels (ui::model_display_label), same order
    // as `models` — label_fn normalises and allocates, and browse mode (empty
    // query) materialises a label for EVERY model on every rebuild. Cached
    // with the same size-guard invalidation as search_keys.
    std::vector<std::string> display_labels;
    // Derived per-model "has an effort ladder" flags (same order as `models`),
    // memoising effort_capable(resolved_caps(id, provider)) — 3 registry map
    // lookups behind a shared_mutex per call, far too hot for the
    // per-keystroke row build over a 300-model catalog. Rebuilt alongside
    // search_keys whenever `models` changes; also invalidated when the
    // capability registries change (models.dev refresh / learned rejection /
    // ^E override) via the caps_epoch stamp below. Empty ⇒ not built (the
    // row build falls back to the live resolve).
    std::vector<bool> reason_flags;
    // Value of caps_epoch() when reason_flags was built; a mismatch means a
    // capability registry changed underneath us → rebuild the flags.
    std::uint64_t reason_epoch = 0;

    // Drop every derived cache. Call this at EVERY site that replaces
    // `models` — there are three, and they each used to hand-clear the
    // caches they happened to remember, which is how a cache added later
    // silently kept serving stale rows after a same-SIZE model swap (the
    // size guard in rebuild_fused_rows only catches length changes).
    // One method, so adding a cache updates every invalidation site at once.
    void invalidate_derived() noexcept {
        search_keys.clear();
        row_keys.clear();
        display_labels.clear();
        reason_flags.clear();
        reason_epoch = 0;
    }
};

// A provider the user is NOT signed into — rendered as a single "sign in to
// <label>" offer at the bottom of the fused list. Stored on the Model so the
// per-keystroke rebuild never re-derives auth from disk.
struct SigninOffer {
    std::string provider_id;
    std::string label;
};

// A single rendered row in the FUSED cross-provider picker: a concrete
// (provider, model) the user can switch to atomically, OR — when
// `model.id` is empty and `authed` is false — a "sign in to <provider>"
// offer that routes into the login flow. Pure value type: the reducer
// builds a vector of these and the view renders it; selection is keyed by
// (provider_id, model.id) identity so async catalog merges never move the
// cursor's logical target.
struct FusedRow {
    std::string provider_id;
    std::string label;                    // provider display name
    // The canonical, provider-uniform MODEL label the view renders and
    // highlights — the SAME string match_positions index into. Computed
    // in build_fused_rows via FusedInputs::label_fn (ui::model_display_
    // label), so the fused picker reads identically to the per-provider
    // one. Empty for sign-in offer rows (no model). Falls back to the
    // raw display_name/id when no label_fn is injected (unit tests that
    // call build_fused_rows directly).
    std::string model_label;
    ModelInfo   model;                    // empty id ⇒ "sign in" offer
    bool        authed = true;
    bool        active = false;           // == current provider + model
    bool        recent = false;           // belongs in the RECENT section
    bool        provider_group = false;     // set on non-recent rows of the active provider's catalog while BROWSING (so the view can title section 2); false while filtering and on offers
    bool        reasons = false;          // model can reason (precomputed at
                                          // build so the view never decodes
    // Capability tier, precomputed alongside `reasons` for the same reason:
    // ModelCapabilities::tier_for tokenises the id and runs several substring
    // scans, and the view touches every visible row EVERY FRAME. 0..3 maps to
    // Tier::Weak..Flagship.
    std::uint8_t tier = 0;
                                          // caps per row per frame)
    // FALSE only when the provider positively reported that this model cannot
    // call tools (Ollama's /api/show probe). Such a model cannot drive the
    // agent at all — it can only chat — so the picker must say so BEFORE the
    // pick, not leave the user wondering why nothing happens. Unknown (the
    // common case: hosted providers don't advertise this) stays true.
    //
    // Deliberately NOT here: the account. A model is identified by
    // (provider, model) — the account is which credential that provider is
    // currently using, orthogonal to model choice and switchable underneath
    // a fixed selection. Putting it on a model row would imply picking a
    // model picks an account (it does not: switch_to_model_ref resolves the
    // provider's ACTIVE credential) and would repeat one provider-level fact
    // on every one of its rows. Account lives on the provider/account picker
    // and the status bar, where it is actionable.
    bool tool_capable = true;
    // Fuzzy-match byte offsets into the model NAME (display_name, else id) for
    // the current query — the chars the view highlights (fzf-style) so a big
    // filtered list shows WHY each row matched. Empty when no query / matched
    // only on the provider name. Computed once per keystroke in build.
    std::vector<int> match_positions;

    [[nodiscard]] bool is_signin_offer() const noexcept {
        return !authed && model.id.value.empty();
    }
    [[nodiscard]] ModelRef ref() const {
        return ModelRef{provider_id, model.id.value};
    }
};

// Canonical wire id: strip agentty's internal extended-context markers
// (`[1m]` / `[2m]`) so the raw provider id goes on the wire. The suffix is a
// PICKER-ONLY marker (it selects the 1M/2M window + the context beta); the
// upstream API has never heard of it and 404s on `claude-sonnet-5[1m]`. This
// mirrors Claude Code's `Yu()` (which strips /\[(1|2)m\]/gi). Every transport
// MUST route req.model through this before putting it in the request body.
[[nodiscard]] inline std::string wire_model_id(std::string_view id) {
    std::string out;
    out.reserve(id.size());
    for (std::size_t i = 0; i < id.size();) {
        if (id[i] == '[' && i + 3 < id.size() && id[i + 2] == 'm'
            && id[i + 3] == ']'
            && (id[i + 1] == '1' || id[i + 1] == '2')) {
            i += 4;   // skip "[1m]" / "[2m]"
            continue;
        }
        out.push_back(id[i]);
        ++i;
    }
    return out;
}

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
    // Fable / Mythos are the 2026 flagship lane (Fable 5 = general-access,
    // Mythos 5 = restricted; same underlying model). They sit ABOVE Opus in
    // the hierarchy and share Opus-class specs (1M ctx, 128k output, effort).
    enum class Family : std::uint8_t { Unknown, Haiku, Sonnet, Opus, Fable, Mythos, Gpt };

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

    // This is an OpenAI-Chat-wire model that exposes configurable reasoning
    // via the top-level `reasoning_effort` enum (low|medium|high) — it is NOT
    // in the Claude/GPT `Family` ladder but still supports effort control.
    // Covers Mistral (Small 4 / Medium 3.5 — adjustable reasoning),
    // DeepSeek-Reasoner/R1, xAI Grok reasoning, and Gemini `*-thinking`.
    // Decoded in from_id; consulted by supports_effort(). (Mistral's Magistral
    // reasons natively and REJECTS reasoning_effort, so it is NOT included.)
    // Kept ORTHOGONAL to `family` so tier/context/output ceilings (which key
    // off family+generation) are completely unaffected — only the effort
    // gates read this flag. These models take low/medium/high only (no
    // `max`/`xhigh`, which are Claude/GPT extensions), so the ladder gates
    // below deliberately do NOT open for reasoning_compat.
    bool reasoning_compat = false;

    // The model's effort enum is BINARY: it accepts only {none, high} and
    // 400s on low/medium (and every Claude/GPT extension). This is Mistral's
    // platform-wide contract — probing api.mistral.ai shows every reasoning
    // model there (mistral-medium 3.5+, mistral-small 4, magistral) rejects
    // low|medium|minimal|xhigh|max with "supported values: [high, none]".
    // Decoded from the id's "stral" house naming (mistral/magistral/devstral/
    // ministral/leanstral …) so the picker ladder collapses to off→high and
    // clamp/wire promote any stale low/medium pick to high instead of 400ing.
    // Orthogonal to reasoning_compat: this only shapes WHICH levels exist,
    // reasoning_compat (+ catalog/overrides) decides IF effort exists.
    bool effort_high_only = false;

    // ── Exact effort-value SET (the models.dev / learned-caps shape) ─────
    // When effort_set_known, `effort_set` is the EXACT set of ON levels this
    // model's API accepts, one bit per Effort (see effort_bit). 0 + known =
    // the model rejects reasoning_effort entirely. This is the ground-truth
    // representation every other effort field approximates: family gates,
    // reasoning_compat and effort_high_only are all just DERIVATIONS used
    // when no exact set is known. Populated by resolved_caps() from (in
    // precedence order) the learned-from-rejection registry (the provider
    // TOLD us via a 400 what it accepts) and the models.dev snapshot.
    // Never set by the constexpr from_id — static inference only guesses.
    std::uint8_t effort_set       = 0;
    bool         effort_set_known = false;

    [[nodiscard]] constexpr bool is_haiku()  const noexcept { return family == Family::Haiku; }
    [[nodiscard]] constexpr bool is_sonnet() const noexcept { return family == Family::Sonnet; }
    [[nodiscard]] constexpr bool is_opus()   const noexcept { return family == Family::Opus; }
    [[nodiscard]] constexpr bool is_fable()  const noexcept { return family == Family::Fable; }
    [[nodiscard]] constexpr bool is_mythos() const noexcept { return family == Family::Mythos; }
    // OpenAI gpt-5.x (ChatGPT/Codex Responses line: gpt-5.6-sol, gpt-5.4, …).
    [[nodiscard]] constexpr bool is_gpt()    const noexcept { return family == Family::Gpt; }
    // Fable/Mythos share Opus-class capabilities; group them for the gates
    // below so a single check covers the whole flagship lane.
    [[nodiscard]] constexpr bool is_flagship() const noexcept {
        return family == Family::Opus || family == Family::Fable
            || family == Family::Mythos;
    }
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
        // Flagship lane (Fable/Mythos 5+) ships with effort control GA
        // (medium is the sweet spot, max the ceiling).
        if (family == Family::Fable || family == Family::Mythos)
            return generation >= 5;
        if (family == Family::Opus)
            return generation > 4 || (generation == 4 && revision >= 5);
        if (family == Family::Sonnet)
            return generation > 4 || (generation == 4 && revision >= 6);
        // OpenAI gpt-5.x (ChatGPT/Codex): the whole line is effort-driven
        // (reasoning.effort low..ultra; medium is the account default). The
        // Responses backend expects an effort on every turn, so expose it.
        if (family == Family::Gpt)
            return generation >= 5;
        // OpenAI-Chat-wire reasoning models (Mistral Small/Medium, DeepSeek-
        // Reasoner, Grok reasoning, Gemini thinking): top-level `reasoning_
        // effort` low|medium|high. Not in the family ladder, gated purely by
        // the decoded flag (or a user override) — see reasoning_compat above.
        if (reasoning_compat)
            return true;
        return false;
    }
    [[nodiscard]] constexpr bool supports_effort_max() const noexcept {
        if (!supports_effort()) return false;
        // Compat reasoning models expose a 3-level enum (low|medium|high) only;
        // `max`/`xhigh` are Claude/GPT-lane extensions. Cap here so a stale Max
        // pick degrades to `high` in effort_wire_for rather than being sent.
        if (reasoning_compat) return false;
        if (family == Family::Fable || family == Family::Mythos)
            return true;  // flagship lane takes every level incl. max
        if (family == Family::Opus)
            return generation > 4 || (generation == 4 && revision >= 6);
        // gpt-5.6 flagship line (sol/terra/luna) accepts `max`; earlier
        // gpt-5.x (5.5/5.4) top out at xhigh.
        if (family == Family::Gpt)
            return generation > 5 || (generation == 5 && revision >= 6);
        return true;  // any effort-capable Sonnet (4.6+) also takes `max`
    }
    [[nodiscard]] constexpr bool supports_effort_xhigh() const noexcept {
        if (!supports_effort()) return false;
        if (reasoning_compat) return false;  // 3-level enum only — see above
        if (family == Family::Fable || family == Family::Mythos)
            return true;  // flagship lane exposes the full ladder
        // Every current gpt-5.x model supports xhigh.
        if (family == Family::Gpt) return true;
        return family == Family::Opus
            && (generation > 4 || (generation == 4 && revision >= 7));
    }

    // Anthropic thinking-mode selection (the interface changed at the 4.6
    // boundary). TRUE => send thinking:{type:"adaptive"} + output_config.effort;
    // FALSE => send the legacy thinking:{type:"enabled", budget_tokens:N}.
    //
    //   • Opus/Sonnet 4.5 and earlier: ONLY "enabled" — "adaptive" 400s
    //     ("adaptive thinking is not supported on this model").
    //   • Opus 4.6: both accepted; we prefer adaptive (the forward path).
    //   • Opus 4.7 / 4.8, Sonnet 4.6+, and the flagship 5+ lane: ONLY
    //     "adaptive" ("enabled"/budget_tokens 400s).
    // Unknown ids default to adaptive (every current flagship is 4.6+; a
    // stale-guess 400 self-heals — but see transport.cpp for the exact wire).
    [[nodiscard]] constexpr bool uses_adaptive_thinking() const noexcept {
        if (family == Family::Fable || family == Family::Mythos)
            return generation >= 5;
        if (family == Family::Opus || family == Family::Sonnet)
            return generation > 4 || (generation == 4 && revision >= 6);
        // Non-Claude families don't use this interface at all; the caller
        // only consults this on the Anthropic wire.
        return generation >= 5;
    }

    // ── Context-window detection (the 1M-window question) ────────────────
    // Verified against the Claude Code binary's model catalog (each entry
    // carries `context: {window:200000, supports_1m_beta, supports_1m_suffix}`).
    // Claude Code does NOT auto-detect 1M from the account tier: the base
    // window is ALWAYS 200k, and 1M is an explicit picker VARIANT the user
    // selects — an id ending in `[1m]` (its `Yu()` strips /\[(1|2)m\]/gi, so
    // a future `[2m]` is anticipated too). The variant is only offered when
    // the model's catalog entry has `supports_1m_suffix:true` AND the account
    // holds `context_1m_entitlement`. agentty mirrors this exactly: the
    // `[1m]` suffix sets extended_context_1m (below, in from_id) which both
    // sends the context-1m beta and widens context_window() to 1M.
    //
    // supports_1m_suffix(): may this model be offered a `[1m]` variant? Per
    // Claude Code's catalog the Sonnet-4 line, Opus-4 line, and Haiku 4.5 all
    // carry supports_1m_suffix:true; older (gen<=3) models do not. The
    // flagship next-gen lane (Fable/Mythos 5+) is treated as 1M-capable too.
    [[nodiscard]] constexpr bool supports_1m_suffix() const noexcept {
        if (family == Family::Sonnet) return generation >= 4;
        if (family == Family::Opus)   return generation >= 4;
        if (family == Family::Haiku)  return generation >= 4;
        if (family == Family::Fable || family == Family::Mythos)
            return generation >= 5;
        return false;
    }

    // The model's context window in TOKENS — the ctx-% and auto-compaction
    // denominator. Matches Claude Code: base 200k for every known Claude
    // model, widened to 1M ONLY when the explicit `[1m]` suffix set
    // extended_context_1m. Unknown families report 0 so the caller prefers a
    // real probed window (Ollama /api/show, OpenAI /v1/models).
    [[nodiscard]] constexpr int context_window() const noexcept {
        if (extended_context_1m) return 1'000'000;   // explicit [1m] variant
        if (is_known_family()) return 200'000;
        return 0;   // unknown: caller falls back to a probed window
    }

    // ── Capability tier: a provider-RELATIVE strength ordinal ────────────
    //
    // No provider ships a "power" number; /v1/models returns ids, not a
    // strength metric. So we derive a coarse tier from the fields already
    // decoded from the id — the vendor's OWN naming is the ordering (Haiku <
    // Sonnet < Opus <= Fable/Mythos; gpt-5-mini < gpt-5 < gpt-5.6; local models
    // by parameter count). Meaningful for comparing models WITHIN one provider
    // (the model router picks the cheapest capable model the active provider
    // offers); NOT an absolute cross-vendor score.
    //
    //   0  weak / tiny / unreliable at tools     — never route work here
    //   1  small-but-capable "cheap lane"         — Haiku, *-mini/nano, 7–13B
    //   2  mid / workhorse                        — Sonnet, plain gpt-5.x, 14–69B
    //   3  flagship                               — Opus/Fable/Mythos, gpt-5.6+, 70B+
    enum class Tier : std::uint8_t { Weak = 0, Cheap = 1, Mid = 2, Flagship = 3 };

    [[nodiscard]] constexpr Tier tier() const noexcept {
        if (weak_tool_use) return Tier::Weak;
        // Flagship lane: Opus/Fable/Mythos, or a gpt-5.x that takes `max`.
        if (is_flagship()) return Tier::Flagship;
        if (is_gpt() && supports_effort_max()) return Tier::Flagship;
        // Cheap lane: Haiku is the canonical small hosted Claude.
        if (is_haiku()) return Tier::Cheap;
        // Mid lane: Sonnet and the plain gpt-5.x workhorse line.
        if (is_sonnet() || is_gpt()) return Tier::Mid;
        // Unknown family (local Ollama / OpenAI-compat): fall back to the
        // params-in-B signal the weak-model inference already computes. A
        // non-weak unknown with no size hint is assumed mid-capable.
        return Tier::Mid;
    }

    // Same tier, but also honouring the `mini`/`nano`/`small`/`lite` id hints
    // that mark a provider's explicitly-cheaper variant (gpt-5-mini,
    // gpt-4o-mini, *-nano). Those decode into the same family as their full
    // sibling, so the id string is the only distinguishing signal; a router
    // holding the id passes it here to demote them to the Cheap lane.
    [[nodiscard]] static constexpr Tier tier_for(std::string_view id) noexcept {
        const Tier base = from_id(id).tier();
        if (base == Tier::Weak) return base;
        auto has = [&](std::string_view n) {
            if (n.size() > id.size()) return false;
            for (std::size_t i = 0; i + n.size() <= id.size(); ++i) {
                bool eq = true;
                for (std::size_t j = 0; j < n.size(); ++j) {
                    char a = id[i + j], b = n[j];
                    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
                    if (a != b) { eq = false; break; }
                }
                if (eq) return true;
            }
            return false;
        };
        const bool cheap_hint =
            has("mini") || has("nano") || has("small") || has("lite")
            || has("flash") || has("turbo");
        // OpenAI o-series reasoning models (o1/o3/o4): the base o* variant is a
        // pricey, slow reasoning model — NOT a cheap workhorse — so it must
        // never be picked as a "cheaper Mid". Only the -mini/-nano o-series
        // variants are genuinely cheap. Detect an `o<digit>` token so `o1`,
        // `o3-pro` rank Flagship while `o1-mini`, `o3-mini` fall to Cheap.
        auto is_o_series = [&]() {
            for (std::size_t i = 0; i + 1 < id.size(); ++i) {
                const bool at_start = (i == 0) || id[i - 1] == '-' || id[i - 1] == '/';
                if (at_start && (id[i] == 'o' || id[i] == 'O')
                    && id[i + 1] >= '1' && id[i + 1] <= '9')
                    return true;
            }
            return false;
        };
        if (is_o_series()) return cheap_hint ? Tier::Cheap : Tier::Flagship;
        if (cheap_hint) return Tier::Cheap;
        return base;
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
                else if (tok == "fable")  caps.family = Family::Fable;
                else if (tok == "mythos") caps.family = Family::Mythos;
                // NOTE: `gpt` is NOT matched here. gpt-4o / gpt-4.1 / gpt-3.5
                // must stay Family::Unknown (generic OpenAI-compat: 16k cap,
                // no effort). Only the gpt-5.x Responses line gets Family::Gpt,
                // set below once the version token confirms generation >= 5.
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
                else if (prev == "gpt") {
                    // OpenAI schema packs the version into ONE dotted token
                    // after `gpt` (`gpt-5.6-sol` → "5.6"; `gpt-5` → "5").
                    // Parse the integer part as the generation and the
                    // fractional part as the revision so the effort gates can
                    // tell gpt-5.6 (accepts `max`) from gpt-5.4 (xhigh top).
                    // Only the gpt-5+ Responses line becomes Family::Gpt;
                    // gpt-4o / gpt-4.1 / gpt-3.5 stay Unknown (generic compat).
                    int g = 0, r = 0;
                    bool ok = !tok.empty(), frac = false, any_frac = false;
                    for (char c : tok) {
                        if (c == '.') { if (frac) { ok = false; break; } frac = true; continue; }
                        if (c < '0' || c > '9') { ok = false; break; }
                        if (frac) { r = r * 10 + (c - '0'); any_frac = true; }
                        else        g = g * 10 + (c - '0');
                    }
                    if (ok && g >= 5 && g <= 99) {
                        caps.family = Family::Gpt;
                        caps.generation = g;
                        caps.generation_4_or_later = true;
                        if (any_frac) caps.revision = r;
                    }
                }
                else if (prev == "haiku" || prev == "sonnet" || prev == "opus"
                         || prev == "fable" || prev == "mythos") {
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
        caps.weak_tool_use     = infer_weak_tool_use(id, caps);
        caps.reasoning_compat  = infer_reasoning_compat(id, caps);
        caps.effort_high_only  = infer_effort_high_only(id);
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
    //     glm-4, deepseek (v3/v4/r1/reasoner/chat), grok, gemini, magistral.
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
            contains(id, "deepseek-v4")  || contains(id, "deepseek-r1")||
            contains(id, "deepseek-reasoner") || contains(id, "deepseek-chat") ||
            contains(id, "grok")         || contains(id, "gemini")      ||
            contains(id, "magistral");
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

    // Decide whether an OpenAI-Chat-wire model exposes configurable reasoning
    // via the top-level `reasoning_effort` enum (low|medium|high), so the
    // effort chip and `reasoning_effort` payload should light up for it.
    //
    // This is deliberately id-string inference (no network probe), mirroring
    // infer_weak_tool_use. Claude/GPT are handled by the `family` ladder and
    // must NOT set this flag (their effort is family-gated). We recognise the
    // hosted reasoning lines that accept the top-level `reasoning_effort`:
    //   • Mistral — the ADJUSTABLE-reasoning models take reasoning_effort:
    //       - "mistral-medium" (Medium 3.5, i.e. mistral-medium-latest /
    //         mistral-medium-3-5 …)
    //       - "mistral-small"  (Small 4, i.e. mistral-small-latest /
    //         mistral-small-3-* …)
    //     Mistral's "magistral-*" models reason NATIVELY and REJECT the
    //     parameter with HTTP 422 — so they are explicitly EXCLUDED below.
    //     (Verified against Mistral's reasoning docs, issue #20.)
    //   • DeepSeek "deepseek-reasoner" / "deepseek-r1" (r1 distills too).
    //   • xAI Grok reasoning lines: "grok-4*", "grok-3-mini" (grok-3-mini
    //     reasons; grok-code-fast is a non-reasoning coder → excluded).
    //   • Google Gemini "*-thinking" via the OpenAI compat shim.
    //   • OpenAI o-series proxied over the Chat wire ("o1", "o3", "o4-mini").
    //
    // A user override (AGENTTY_FORCE_EFFORT, read at runtime) is applied
    // separately by effort_capable() at the effort chokepoints — it is
    // intentionally NOT consulted here so from_id() stays a pure constexpr
    // decode (no getenv on the hot path, usable in constant expressions).
    [[nodiscard]] static constexpr bool infer_reasoning_compat(
            std::string_view id, const ModelCapabilities& caps) noexcept {
        // Claude/GPT are family-gated; never route them through this flag.
        if (caps.is_known_family() || caps.family == Family::Gpt) return false;

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

        // Exclusions FIRST — non-reasoning coder lines and native-reasoning
        // models that share a prefix with a reasoning family. These must lose
        // to nothing below, so return early.
        //   • grok-code-fast: a non-reasoning coder (shares "grok").
        //   • magistral-*: reasons NATIVELY and REJECTS reasoning_effort (422),
        //     even though it shares the "mistral"… no — "magistral" is a
        //     distinct token, but guard explicitly so a future substring match
        //     can't grab it.
        if (contains(id, "grok-code")) return false;
        if (contains(id, "magistral")) return false;

        return contains(id, "mistral-small")     // Mistral Small 4 (adjustable)
            || contains(id, "mistral-medium")    // Mistral Medium 3.5 (adjustable)
            || contains(id, "deepseek-reasoner")
            || contains(id, "deepseek-r1")
            || contains(id, "grok-4")
            || contains(id, "grok-3-mini")
            || contains(id, "gpt-oss")           // OpenAI open-weight reasoner
            || contains(id, "-thinking")
            || contains(id, "o1")
            || contains(id, "o3")
            || contains(id, "o4-mini");
    }

    // Mistral-platform models: the reasoning_effort enum is BINARY. Probing
    // api.mistral.ai (Nov 2026): every model that takes the parameter accepts
    // ONLY {none, high} and 400s on low/medium/minimal/xhigh/max with
    // "supported values: [high, none]". Recognised by the house "-stral"
    // naming (mistral / magistral / ministral / devstral / codestral /
    // voxtral / leanstral) — non-reasoning lines in that family have effort
    // OFF anyway, so an over-match here is harmless (the flag only shapes the
    // ladder when effort is on). Pure id decode: correct from frame 1, before
    // any catalog fetch lands.
    [[nodiscard]] static constexpr bool infer_effort_high_only(
            std::string_view id) noexcept {
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
        return contains(id, "stral");
    }
};

// ── Provider scope for capability keys ───────────────────────────────
// The SAME bare model id can live on several providers with DIFFERENT
// contracts (gpt-oss-120b on Groq vs Cerebras vs local Ollama). Capability
// facts are therefore recorded under a scoped key "provider/model" and
// looked up scoped-first, bare-second. The provider layer pushes the active
// provider id here on every switch (provider::select), so resolved_caps —
// which only receives a model id — can consult the scoped fact without the
// domain layer depending on provider state.
namespace caps_scope_detail {
inline std::mutex& mu() { static std::mutex m; return m; }
inline std::string& val() { static std::string s; return s; }
} // namespace caps_scope_detail

inline void set_caps_provider_scope(std::string provider_id) {
    std::lock_guard lk(caps_scope_detail::mu());
    caps_scope_detail::val() = std::move(provider_id);
}
[[nodiscard]] inline std::string caps_provider_scope() {
    std::lock_guard lk(caps_scope_detail::mu());
    return caps_scope_detail::val();
}
// "provider/model" when a scope is set; "" when not (callers then use bare).
// The one-arg form uses the ACTIVE provider scope; pass `scope` explicitly
// when resolving for a row that belongs to a DIFFERENT provider (the fused
// picker renders every authed provider's models at once).
// The model component is canonicalised (capkey::norm_model) so "3.5" vs
// "3-5" vs case variants are one key — the registries' writers normalise
// identically, making spelling-based misses structurally impossible.
[[nodiscard]] inline std::string scoped_caps_key(std::string_view model_id,
                                                 std::string_view scope) {
    if (scope.empty()) return {};
    return capkey::scoped(scope, model_id);
}
[[nodiscard]] inline std::string scoped_caps_key(std::string_view model_id) {
    return scoped_caps_key(model_id, caps_provider_scope());
}

// ── Live-catalog reasoning registry ──────────────────────────────────────────
// Some providers DECLARE per-model reasoning support in their /v1/models
// payload (Mistral: capabilities.reasoning true|false). That live flag beats
// id inference — it tracks the provider's actual dispatch table, which drifts
// under our static heuristics in BOTH directions (magistral-* now ACCEPTS
// reasoning_effort though from_id excludes it; dated mistral-medium-2505/2508
// REJECT it though the substring matches). list_models() records the flag
// here; resolved_caps() folds it in UNDER the user's explicit override but
// OVER the from_id guess. Same lock discipline as the override registry
// (atomic emptiness probe → shared lock), since this too sits on the render
// path.
// ── Capability epoch ──────────────────────────────────────────────────────
// Monotonic counter bumped by EVERY capability-registry write (catalog
// reasoning declarations, declared/learned effort sets, user overrides).
// Callers that memoise resolved_caps-derived values (the fused picker's
// per-model reason_flags) stamp the epoch at build time and rebuild when it
// moves — so a models.dev refresh landing mid-session invalidates exactly
// the caches that depend on it, and nothing polls.
namespace caps_epoch_detail {
inline std::atomic<std::uint64_t>& counter() {
    static std::atomic<std::uint64_t> c{1};
    return c;
}
} // namespace caps_epoch_detail
[[nodiscard]] inline std::uint64_t caps_epoch() noexcept {
    return caps_epoch_detail::counter().load(std::memory_order_relaxed);
}
inline void bump_caps_epoch() noexcept {
    caps_epoch_detail::counter().fetch_add(1, std::memory_order_relaxed);
}


// Normalise a registry key in place: the model component (after the last
// '/', or the whole string when bare) goes through capkey::norm_model so
// writers and readers agree byte-for-byte regardless of source spelling.
[[nodiscard]] inline std::string norm_caps_key(std::string_view key) {
    if (auto slash = key.rfind('/'); slash != std::string_view::npos) {
        std::string out{key.substr(0, slash + 1)};
        out += capkey::norm_model(key.substr(slash + 1));
        return out;
    }
    return capkey::norm_model(key);
}

namespace catalog_reasoning_detail {
inline std::shared_mutex& mu() { static std::shared_mutex m; return m; }
inline std::map<std::string, bool>& map_() {
    static std::map<std::string, bool> m; return m;
}
// Keys whose recorded reasoning fact CONFLICTED across sources (one source
// said reasons, another said not). Cross-provider aggregators disagree about
// the SAME bare id (e.g. one lists `mistral-large-2402` as reasoning, another
// as not) — and a flat bare namespace would otherwise be last-writer-wins,
// silently lighting a reasoning chip on an instruct model. A poisoned key is
// treated as "no info" so resolution falls through to the scoped fact or
// id-inference instead of trusting a coin-flip. Provider-agnostic: no model
// ids are hardcoded; the data corrects itself by disagreeing.
inline std::set<std::string>& poisoned_() {
    static std::set<std::string> s; return s;
}
inline std::atomic<bool>& any() { static std::atomic<bool> a{false}; return a; }
} // namespace catalog_reasoning_detail

inline void set_catalog_reasoning(std::string model_id, bool reasons) {
    bump_caps_epoch();
    model_id = norm_caps_key(model_id);
    std::unique_lock lk(catalog_reasoning_detail::mu());
    catalog_reasoning_detail::map_()[std::move(model_id)] = reasons;
    catalog_reasoning_detail::any().store(true, std::memory_order_relaxed);
}

// Merge a fact that may share a key with a DIFFERENT source (the bare-tail
// path in modelsdev). First writer records; a later writer that AGREES is a
// no-op; a later writer that DISAGREES poisons the key so it reads as "no
// info". Scoped keys never collide across providers, so they use the plain
// setter above; only the ambiguous bare tail goes through here.
inline void merge_catalog_reasoning(const std::string& raw_id, bool reasons) {
    const std::string model_id = norm_caps_key(raw_id);
    std::unique_lock lk(catalog_reasoning_detail::mu());
    auto& m = catalog_reasoning_detail::map_();
    auto& poisoned = catalog_reasoning_detail::poisoned_();
    if (poisoned.count(model_id)) return;          // already ambiguous
    if (auto it = m.find(model_id); it != m.end()) {
        if (it->second != reasons) {               // sources disagree → poison
            m.erase(it);
            poisoned.insert(model_id);
        }
        return;
    }
    m[model_id] = reasons;
    catalog_reasoning_detail::any().store(true, std::memory_order_relaxed);
}
// Tri-state: 1 (catalog says reasons), 0 (catalog says not), -1 (no info).
// Scoped-first ("provider/model"), bare fallback — see scoped_caps_key.
[[nodiscard]] inline int catalog_reasoning_for(std::string_view model_id,
                                               std::string_view scope = {}) {
    if (!catalog_reasoning_detail::any().load(std::memory_order_relaxed))
        return -1;
    std::shared_lock lk(catalog_reasoning_detail::mu());
    auto& m = catalog_reasoning_detail::map_();
    auto scoped = scope.empty() ? scoped_caps_key(model_id)
                                : scoped_caps_key(model_id, scope);
    if (!scoped.empty())
        if (auto it = m.find(scoped); it != m.end())
            return it->second ? 1 : 0;
    auto it = m.find(capkey::norm_tail(model_id));
    if (it == m.end()) return -1;
    return it->second ? 1 : 0;
}

// DECLARED effort-value sets: a metadata source (models.dev snapshot, or a
// provider that publishes its effort enum) names the exact ON levels a model
// accepts. Sits BELOW the learned-from-rejection registry (ground truth from
// the provider's own 400 beats third-party metadata) and below user
// overrides, but ABOVE from_id derivation. Same keying and lock discipline
// as the reasoning declarations above.
namespace catalog_effort_detail {
inline std::shared_mutex& mu() { static std::shared_mutex m; return m; }
inline std::map<std::string, std::uint8_t>& map_() {
    static std::map<std::string, std::uint8_t> m; return m;
}
inline std::set<std::string>& poisoned_() {
    static std::set<std::string> s; return s;
}
inline std::atomic<bool>& any() { static std::atomic<bool> a{false}; return a; }
} // namespace catalog_effort_detail

inline void set_catalog_effort_set(std::string model_id, std::uint8_t set) {
    bump_caps_epoch();
    model_id = norm_caps_key(model_id);
    std::unique_lock lk(catalog_effort_detail::mu());
    catalog_effort_detail::map_()[std::move(model_id)] = set;
    catalog_effort_detail::any().store(true, std::memory_order_relaxed);
}

// Bare-tail merge for the effort-set registry — same cross-provider collision
// semantics as merge_catalog_reasoning: agree → keep, disagree → poison to
// "no declaration" so resolution falls back to the scoped fact / id-inference.
inline void merge_catalog_effort_set(const std::string& raw_id,
                                     std::uint8_t set) {
    const std::string model_id = norm_caps_key(raw_id);
    std::unique_lock lk(catalog_effort_detail::mu());
    auto& m = catalog_effort_detail::map_();
    auto& poisoned = catalog_effort_detail::poisoned_();
    if (poisoned.count(model_id)) return;
    if (auto it = m.find(model_id); it != m.end()) {
        if (it->second != set) { m.erase(it); poisoned.insert(model_id); }
        return;
    }
    m[model_id] = set;
    catalog_effort_detail::any().store(true, std::memory_order_relaxed);
}
// -1 = no declaration; else the bitmask of declared ON levels.
// Scoped-first ("provider/model"), bare fallback — see scoped_caps_key.
[[nodiscard]] inline int catalog_effort_set_for(std::string_view model_id,
                                                std::string_view scope = {}) {
    if (!catalog_effort_detail::any().load(std::memory_order_relaxed))
        return -1;
    std::shared_lock lk(catalog_effort_detail::mu());
    auto& m = catalog_effort_detail::map_();
    auto scoped = scope.empty() ? scoped_caps_key(model_id)
                                : scoped_caps_key(model_id, scope);
    if (!scoped.empty())
        if (auto it = m.find(scoped); it != m.end())
            return static_cast<int>(it->second);
    auto it = m.find(capkey::norm_tail(model_id));
    if (it == m.end()) return -1;
    return static_cast<int>(it->second);
}

// ── Per-model reasoning-effort override registry ─────────────────────────
// The "configure it myself" seam (issue #20). The constexpr from_id() decode
// only INFERS whether a compat model reasons; users can override that per
// model — persisted in Settings.reasoning_effort_overrides and pushed here at
// startup (and on every in-app toggle), mirroring provider::set_custom_auth
// _header. Resolution precedence, applied by resolved_caps():
//     per-model override  >  AGENTTY_FORCE_EFFORT env  >  from_id inference.
// Claude/GPT stay family-gated — an override only opens/closes the COMPAT
// lane (low|medium|high); it never fabricates the max/xhigh ladder.
namespace reasoning_override_detail {
inline std::shared_mutex&                    reasoning_override_mu() {
    static std::shared_mutex m; return m;
}
inline std::map<std::string, bool>&         reasoning_override_map() {
    static std::map<std::string, bool> m; return m;
}
// Lock-free "is the map non-empty?" flag. The override map is empty for the
// vast majority of users (nobody set a ^E override), yet reasoning_override_for
// is called on the PER-FRAME UI render path (model_badge, the picker). Reading
// this atomic first lets the common case return without touching the mutex at
// all — which matters because the mutex is also taken by BACKGROUND threads
// (launch_stream, subagent) via resolved_caps, and blocking the render thread
// on that contention was a source of intermittent input lag.
inline std::atomic<bool>&                    reasoning_override_any() {
    static std::atomic<bool> any{false}; return any;
}
} // namespace reasoning_override_detail

// Set/clear a single model's override (true = force effort on, false = off).
inline void set_reasoning_override(std::string model_id, bool on) {
    bump_caps_epoch();
    model_id = norm_caps_key(model_id);
    std::unique_lock lk(reasoning_override_detail::reasoning_override_mu());
    reasoning_override_detail::reasoning_override_map()[std::move(model_id)] = on;
    reasoning_override_detail::reasoning_override_any().store(
        !reasoning_override_detail::reasoning_override_map().empty(),
        std::memory_order_relaxed);
}
inline void clear_reasoning_override(const std::string& model_id) {
    bump_caps_epoch();
    std::unique_lock lk(reasoning_override_detail::reasoning_override_mu());
    reasoning_override_detail::reasoning_override_map().erase(model_id);
    reasoning_override_detail::reasoning_override_any().store(
        !reasoning_override_detail::reasoning_override_map().empty(),
        std::memory_order_relaxed);
}
inline void clear_reasoning_overrides() {
    bump_caps_epoch();
    std::unique_lock lk(reasoning_override_detail::reasoning_override_mu());
    reasoning_override_detail::reasoning_override_map().clear();
    reasoning_override_detail::reasoning_override_any().store(false,
        std::memory_order_relaxed);
}
inline void set_reasoning_overrides(std::map<std::string, bool> all) {
    bump_caps_epoch();
    // Normalise each key: these maps hydrate from persisted settings, which
    // may predate the capkey discipline or carry a different spelling than
    // today's lookups. Raw installation would silently orphan those facts.
    std::map<std::string, bool> normed;
    for (auto& [k, v] : all) normed[norm_caps_key(k)] = v;
    std::unique_lock lk(reasoning_override_detail::reasoning_override_mu());
    reasoning_override_detail::reasoning_override_any().store(!normed.empty(),
        std::memory_order_relaxed);
    reasoning_override_detail::reasoning_override_map() = std::move(normed);
}
// Tri-state lookup: 1 (force on), 0 (force off), -1 (no override for this id).
// Lock-free when no overrides exist (the common case). Otherwise a SHARED lock
// — concurrent per-frame readers never block each other; only the rare write
// (init / ^E toggle) is exclusive.
[[nodiscard]] inline int reasoning_override_for(std::string_view model_id) {
    if (!reasoning_override_detail::reasoning_override_any().load(
            std::memory_order_relaxed))
        return -1;
    std::shared_lock lk(reasoning_override_detail::reasoning_override_mu());
    auto& m = reasoning_override_detail::reasoning_override_map();
    auto it = m.find(capkey::norm_tail(model_id));
    if (it == m.end()) return -1;
    return it->second ? 1 : 0;
}

// AGENTTY_FORCE_EFFORT env override, read once. Returns 1 (force on),
// 0 (force off), or -1 (unset). Global fallback below the per-model override.
[[nodiscard]] inline int effort_force_override() noexcept {
    static const int v = [] {
        const char* e = std::getenv("AGENTTY_FORCE_EFFORT");
        if (!e || !*e) return -1;
        if (e[0] == '0' && e[1] == '\0') return 0;
        const char c = (e[0] >= 'A' && e[0] <= 'Z')
                           ? static_cast<char>(e[0] + 32) : e[0];
        // "1", "true", "yes", "on" → force on; anything else → unset.
        if (e[0] == '1' || c == 't' || c == 'y' || c == 'o') return 1;
        return -1;
    }();
    return v;
}

// ── Learned effort-set registry (feature DETECTION, not enumeration) ──────
// When a provider rejects a reasoning_effort value it usually names the
// contract in the error body — Mistral: "supported values: [high, none]";
// generic OpenAI-compat: "reasoning_effort is not enabled for this model".
// The stream reducer parses that (provider::parse_effort_rejection), records
// the EXACT accepted set here, persists it (Settings.learned_effort_sets),
// and silently retries with the clamped value. From then on the ladder, the
// clamps and the wire are all correct for that model — across restarts,
// without a release, for providers no database has ever heard of. Same lock
// discipline as the other registries (render-path readers).
namespace learned_effort_detail {
inline std::shared_mutex& mu() { static std::shared_mutex m; return m; }
// model id → effort_set bitmask (0 = rejects the parameter entirely).
inline std::map<std::string, std::uint8_t>& map_() {
    static std::map<std::string, std::uint8_t> m; return m;
}
inline std::atomic<bool>& any() { static std::atomic<bool> a{false}; return a; }
} // namespace learned_effort_detail

inline void set_learned_effort_set(std::string model_id, std::uint8_t set) {
    bump_caps_epoch();
    model_id = norm_caps_key(model_id);
    std::unique_lock lk(learned_effort_detail::mu());
    learned_effort_detail::map_()[std::move(model_id)] = set;
    learned_effort_detail::any().store(true, std::memory_order_relaxed);
}
inline void set_learned_effort_sets(std::map<std::string, std::uint8_t> all) {
    bump_caps_epoch();
    // Normalise each key — same persisted-spelling hazard as
    // set_reasoning_overrides above.
    std::map<std::string, std::uint8_t> normed;
    for (auto& [k, v] : all) normed[norm_caps_key(k)] = v;
    std::unique_lock lk(learned_effort_detail::mu());
    learned_effort_detail::any().store(!normed.empty(),
                                       std::memory_order_relaxed);
    learned_effort_detail::map_() = std::move(normed);
}
[[nodiscard]] inline std::map<std::string, std::uint8_t>
learned_effort_sets_snapshot() {
    std::shared_lock lk(learned_effort_detail::mu());
    return learned_effort_detail::map_();
}
// -1 = nothing learned for this id; else the bitmask (may be 0 = param off).
// Scoped-first lookup: a fact learned while Groq was active is keyed
// "groq/gpt-oss-120b" and does NOT leak to the same id on Cerebras; a bare
// entry (legacy, or explicitly unscoped) still matches everywhere.
[[nodiscard]] inline int learned_effort_set_for(std::string_view model_id,
                                                std::string_view scope = {}) {
    if (!learned_effort_detail::any().load(std::memory_order_relaxed))
        return -1;
    std::shared_lock lk(learned_effort_detail::mu());
    auto& m = learned_effort_detail::map_();
    auto scoped = scope.empty() ? scoped_caps_key(model_id)
                                : scoped_caps_key(model_id, scope);
    if (!scoped.empty())
        if (auto it = m.find(scoped); it != m.end())
            return static_cast<int>(it->second);
    auto it = m.find(capkey::norm_tail(model_id));
    if (it == m.end()) return -1;
    return static_cast<int>(it->second);
}

// Decode a model id AND fold in the runtime reasoning-effort override (per
// model, then AGENTTY_FORCE_EFFORT env). This is the id-aware sibling of the
// pure constexpr from_id(): effort call sites use THIS so a user's override
// reaches supports_effort()/effort_wire_for()/the picker uniformly — every
// consumer already funnels through the returned caps.
//
// `scope`: the provider whose contract to resolve against. Empty = the
// ACTIVE provider (right for the wire path). Pass the
// row's own provider_id when rendering CROSS-provider surfaces (the fused
// picker shows every authed provider's rows at once — resolving a Groq row
// under the active Mistral scope would read the wrong host's facts).
[[nodiscard]] inline ModelCapabilities resolved_caps(std::string_view model_id,
                                                     std::string_view scope = {}) {
    ModelCapabilities caps = ModelCapabilities::from_id(model_id);
    // The LEARNED set applies to every lane — even Claude/GPT — because it is
    // ground truth straight from the provider's own rejection. (In practice
    // Claude/GPT never land here; the guard exists for aggregator hosts that
    // serve claude-* ids over the OpenAI wire with different contracts.)
    if (const int learned = learned_effort_set_for(model_id, scope);
        learned >= 0) {
        caps.effort_set       = static_cast<std::uint8_t>(learned);
        caps.effort_set_known = true;
    }
    // Claude/GPT are family-gated; overrides only touch the compat lane so we
    // never fabricate their max/xhigh ladder or disturb tier/context logic.
    if (caps.is_known_family() || caps.family == ModelCapabilities::Family::Gpt)
        return caps;
    // Declared (models.dev / publishing provider) effort enums — compat lane
    // only: family ladders are already exact, and third-party metadata for
    // Claude/GPT expresses different wire semantics (adaptive thinking).
    if (!caps.effort_set_known)
        if (const int declared = catalog_effort_set_for(model_id, scope);
            declared >= 0) {
            caps.effort_set       = static_cast<std::uint8_t>(declared);
            caps.effort_set_known = true;
        }
    // Precedence: explicit per-model override > global env > the provider's
    // OWN catalog declaration (capabilities.reasoning, recorded by
    // list_models) > from_id inference. The catalog layer is what absorbs
    // provider drift — e.g. Mistral flipping magistral to accept
    // reasoning_effort, or a dated mistral-medium revision that rejects it —
    // without a code change.
    const int per_model = reasoning_override_for(model_id);
    if (per_model >= 0) {
        caps.reasoning_compat = (per_model == 1);
        // An explicit user override beats even the learned set: force-on
        // reopens the derived ladder, force-off kills it.
        if (caps.effort_set_known) {
            if (per_model == 0) { caps.effort_set = 0; }
            else if (caps.effort_set == 0) { caps.effort_set_known = false; }
        }
    } else if (const int env = effort_force_override(); env >= 0) {
        caps.reasoning_compat = (env == 1);
        if (env == 0 && caps.effort_set_known) caps.effort_set = 0;
    } else if (const int live = catalog_reasoning_for(model_id, scope);
               live >= 0) {
        caps.reasoning_compat = (live == 1);
    }
    return caps;
}

// Convenience: infer weak-tool-use straight from a model id string.
// Used by the provider/runtime paths that only hold the id, not the caps.
[[nodiscard]] inline bool is_weak_model(std::string_view model_id) noexcept {
    return ModelCapabilities::from_id(model_id).is_weak_tool_user();
}

// Does this model BEGIN its output in reasoning with NO opening think-tag?
// vLLM calls this reason-by-default; the families that do it are Magistral
// (Mistral's reasoning line — emits thoughts, then [/THINK], then the
// answer) and DeepSeek-R1 / deepseek-reasoner (prepends <think> only
// sometimes). For these, leading content before the FIRST close tag is
// implicit reasoning and must be routed to the thinking block. Any other
// model must NEVER have leading text treated as reasoning — a stray
// "</think>" in ordinary answer prose stays verbatim.
//
// A MODEL fact, not a transport fact: it describes how the model was
// trained to emit tokens, identically on every host that serves it. Lives
// here (with is_weak_model / infer_reasoning_compat) so every transport
// reads one authority — the openai-compat transport used to sniff this
// inline, which is exactly how a second transport would have drifted.
[[nodiscard]] constexpr bool reasons_by_default(std::string_view model_id) noexcept {
    // constexpr-friendly case-insensitive substring scan (mirrors the
    // contains() helpers used by from_id's inference blocks).
    constexpr auto has = [](std::string_view id, std::string_view n) {
        if (n.size() > id.size()) return false;
        for (std::size_t i = 0; i + n.size() <= id.size(); ++i) {
            bool eq = true;
            for (std::size_t j = 0; j < n.size(); ++j) {
                char a = id[i + j], b = n[j];
                if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
                if (a != b) { eq = false; break; }
            }
            if (eq) return true;
        }
        return false;
    };
    return has(model_id, "magistral")
        || has(model_id, "deepseek-r1")
        || has(model_id, "deepseek-reasoner");
}
static_assert(reasons_by_default("Magistral-Small-2506"));
static_assert(reasons_by_default("deepseek-r1:70b"));
static_assert(!reasons_by_default("mistral-small-latest"));
static_assert(!reasons_by_default("qwen3:32b"));   // explicit-tag family


// Is this id a chat/completions model an agent can actually DRIVE (stream text
// + call tools), as opposed to an embedding / image / audio / moderation /
// realtime endpoint that a raw provider `/v1/models` dump also lists?
//
// The subagent router picks the cheapest CAPABLE candidate; without this gate
// it would happily pick `text-embedding-3-small` or `dall-e-3` as "cheapest
// Mid" and route a whole subagent to a model that can't chat — breaking the
// task outright. OpenAI's /v1/models is the worst offender (it returns every
// asset the key can touch); Anthropic/ChatGPT/Ollama lists are chat-only, so
// this is a no-op there. Deny-list by well-known non-chat id fragments — a
// deny-list (not an allow-list) so a new chat model is never wrongly excluded.
[[nodiscard]] inline bool is_dispatchable_model(std::string_view id) noexcept {
    auto has = [&](std::string_view n) {
        if (n.size() > id.size()) return false;
        for (std::size_t i = 0; i + n.size() <= id.size(); ++i) {
            bool eq = true;
            for (std::size_t j = 0; j < n.size(); ++j) {
                char a = id[i + j], b = n[j];
                if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
                if (a != b) { eq = false; break; }
            }
            if (eq) return true;
        }
        return false;
    };
    // Non-chat asset families that appear in a raw /v1/models listing.
    if (has("embedding") || has("embed")     ||   // text-embedding-3-*, *-embed
        has("dall-e")    || has("dalle")     ||   // image generation
        has("whisper")                       ||   // speech-to-text
        has("tts")                           ||   // text-to-speech
        has("audio")     || has("realtime")  ||   // audio / realtime sockets
        has("moderation")                    ||   // omni-moderation-*
        has("image")                         ||   // *-image-* generation
        has("transcribe")                    ||   // gpt-4o-transcribe
        has("search")                        ||   // *-search-preview retrieval
        has("rerank")    || has("reranker")  ||   // rerankers (cohere/voyage)
        has("guard"))                             // llama-guard safety filters
        return false;
    return true;
}

// ── Model router: pick the cheapest capable model for a delegated role ──────
//
// Read-only subagent roles (explorer/reviewer) do grunt work — read, grep,
// map, summarise — that a small model handles as well as a flagship at a
// fraction of the cost. Routing them to a cheaper model is the single biggest
// subagent economy lever, but it must NEVER silently downgrade quality on a
// provider that has nothing cheaper, and must stay within the SAME provider
// (cross-vendor auth/caching don't transfer).
//
// Strategy (all signals are already-decoded, no network):
//   • Only consider models the active provider actually offers (`candidates`,
//     i.e. m.d.available_models — already provider-scoped).
//   • Rank by ModelCapabilities::tier_for(id): reject Weak (tier 0, unreliable
//     at tools) and anything a probe marked no-tools.
//   • Pick the LOWEST tier that is still >= `min_tier` (the capability floor
//     for the role) — that's the cheapest capable model on this provider.
//   • Never route UP: if the cheapest capable candidate isn't cheaper than the
//     parent, keep the parent model (return it unchanged). So a user on only
//     Opus, or only one model, sees no change and no regression.
//
// `parent_model` is what the main turn uses; `candidates` is the provider's
// model list. Returns the id to run the role on.
[[nodiscard]] inline std::string cheapest_capable_model(
        std::string_view parent_model,
        const std::vector<ModelInfo>& candidates,
        ModelCapabilities::Tier min_tier = ModelCapabilities::Tier::Cheap) {
    using Tier = ModelCapabilities::Tier;
    const Tier parent_tier = ModelCapabilities::tier_for(parent_model);

    std::string best;
    Tier best_tier = Tier::Flagship;
    int  best_ctx  = -1;
    for (const auto& mi : candidates) {
        const std::string_view id = mi.id.value;
        // A probe that reported no tool support disqualifies the model — a
        // subagent's whole job is tool use.
        if (mi.supports_tools.has_value() && !*mi.supports_tools) continue;
        // Never route to a non-chat asset (embedding/image/audio/moderation)
        // that a raw /v1/models dump also lists — it can't run an agent turn.
        if (!is_dispatchable_model(id)) continue;
        const Tier t = ModelCapabilities::tier_for(id);
        if (t == Tier::Weak) continue;              // unreliable at tools, ever
        if (t < min_tier) continue;                 // below the role's floor
        if (t >= parent_tier) continue;             // not cheaper than parent
        // Prefer the cheapest tier; within a tier prefer the LARGER context
        // window (a real chat model over a 4k stub / preview variant).
        const int ctx = mi.context_window;
        const bool better = best.empty() || t < best_tier
                          || (t == best_tier && ctx > best_ctx);
        if (better) {
            best = mi.id.value;
            best_tier = t;
            best_ctx  = ctx;
        }
    }
    // Whichever id we route to, DROP the `[1m]`/`[2m]` extended-context
    // picker marker. That marker is a parent-chat-only signal: it widens the
    // window AND makes the transport send the `context-1m` beta header. A
    // subagent never needs it — read-only roles run on a tight output budget
    // with age-faded tool results and never approach even a 200K window, let
    // alone 1M. Worse, sending `context-1m` on a subscription that isn't
    // entitled to the long-context beta 400s the whole subagent request
    // ("long context beta is not yet available for this subscription"),
    // which is exactly the failure this strip prevents. When nothing is
    // strictly cheaper we still fall through here, so a parent that picked a
    // `[1m]` variant hands its subagents the plain, always-accepted id.
    return wire_model_id(best.empty() ? parent_model : std::string_view{best});
}

// ── Model picker ordering: strength first, never a fixed family bucket ──────
//
// An id's FAMILY NAME must never dictate its position in the picker — an
// earlier design bucketed "Opus, Sonnet, Haiku, Fable, Mythos" in that fixed
// order, which sank Fable/Mythos to the very bottom even though they're the
// NEWEST flagship-tier lane (same strength class as Opus, just a different
// codename that happens to sort last alphabetically/positionally). Instead,
// order by actual capability: tier() descending (Flagship models — Opus AND
// Fable/Mythos — always lead), then newest generation.revision within a
// tier, then a small family tie-break so same-generation peers still group
// predictably instead of interleaving on id string alone.
[[nodiscard]] inline bool model_picker_less(const ModelInfo& a,
                                            const ModelInfo& b) noexcept {
    const auto ca = ModelCapabilities::from_id(a.id.value);
    const auto cb = ModelCapabilities::from_id(b.id.value);
    const auto ta = ca.tier(), tb = cb.tier();
    if (ta != tb) return ta > tb;                       // higher tier first
    if (ca.generation != cb.generation)
        return ca.generation > cb.generation;           // newest gen first
    if (ca.revision != cb.revision)
        return ca.revision > cb.revision;                // newest revision first
    auto family_name_rank = [](ModelCapabilities::Family f) -> int {
        switch (f) {
            case ModelCapabilities::Family::Opus:   return 0;
            case ModelCapabilities::Family::Fable:  return 1;
            case ModelCapabilities::Family::Mythos: return 2;
            case ModelCapabilities::Family::Sonnet: return 3;
            case ModelCapabilities::Family::Haiku:  return 4;
            default:                                return 5;
        }
    };
    const int fa = family_name_rank(ca.family);
    const int fb = family_name_rank(cb.family);
    if (fa != fb) return fa < fb;
    return a.id.value < b.id.value;   // final stable tie-break
}

// ============================================================================
// Effort — user-selectable reasoning/spend tier (output_config.effort).
// ============================================================================
// `None` sends nothing — preserving the default no-thinking, replay-safe
// wire. Any other level makes the Claude provider send adaptive thinking +
// the matching `output_config.effort`. Selectable live from the model picker.
// Ordered LOWEST→HIGHEST reasoning intent. `Minimal` is OpenAI's gpt-5
// bottom tier (reasoning_effort="minimal" — fastest, least reasoning); it
// sits BELOW Low in the ladder. NB the enum ORDINAL order is the ladder
// order, but the persisted bitmask (effort_bit) is INDEPENDENT of it —
// Minimal takes a fresh high bit so existing learned_effort_sets masks
// (which encoded low..max as bits 0..4) survive untouched.
enum class Effort : std::uint8_t { None, Minimal, Low, Medium, High, Xhigh, Max };

// Wire value for output_config.effort. None → "" (the field is omitted).
[[nodiscard]] constexpr std::string_view effort_wire(Effort e) noexcept {
    switch (e) {
        case Effort::None:    return "";
        case Effort::Minimal: return "minimal";
        case Effort::Low:     return "low";
        case Effort::Medium:  return "medium";
        case Effort::High:    return "high";
        case Effort::Xhigh:   return "xhigh";
        case Effort::Max:     return "max";
    }
    return "";
}

// Short label for the picker UI (None renders as "off").
[[nodiscard]] constexpr std::string_view effort_label(Effort e) noexcept {
    return e == Effort::None ? std::string_view{"off"} : effort_wire(e);
}

// Parse a persisted wire value back to Effort. Unknown / "" → None.
[[nodiscard]] constexpr Effort effort_from_wire(std::string_view s) noexcept {
    if (s == "minimal") return Effort::Minimal;
    if (s == "low")    return Effort::Low;
    if (s == "medium") return Effort::Medium;
    if (s == "high")   return Effort::High;
    if (s == "xhigh")  return Effort::Xhigh;
    if (s == "max")    return Effort::Max;
    return Effort::None;
}

// AGENTTY_FORCE_EFFORT override is defined earlier (before resolved_caps),
// which folds it into the per-model resolution. See effort_force_override().

// Bit for an ON effort level in ModelCapabilities::effort_set. None has no
// bit — "off" (omit the field) is always available and never learned away.
[[nodiscard]] constexpr std::uint8_t effort_bit(Effort e) noexcept {
    switch (e) {
        case Effort::Low:     return 1u << 0;
        case Effort::Medium:  return 1u << 1;
        case Effort::High:    return 1u << 2;
        case Effort::Xhigh:   return 1u << 3;
        case Effort::Max:     return 1u << 4;
        // Minimal takes a fresh bit ABOVE the historical low..max range so a
        // persisted mask written before this tier existed still decodes
        // identically (no migration). Ladder position is set by the enum
        // ordinal + the explicit ladders below, not by this bit value.
        case Effort::Minimal: return 1u << 5;
        case Effort::None:    return 0;
    }
    return 0;
}

// The model's ON-level set as a bitmask, from the best information we have:
// the exact set when known (learned/models.dev), else DERIVED from the
// family/compat gates. This is THE single source every ladder/clamp/wire
// helper below reads — heterogeneity is data, not code paths.
[[nodiscard]] constexpr std::uint8_t effort_set_of(
        const ModelCapabilities& caps) noexcept {
    if (caps.effort_set_known) return caps.effort_set;
    if (!caps.supports_effort()) return 0;
    if (caps.effort_high_only) return effort_bit(Effort::High);
    std::uint8_t s = static_cast<std::uint8_t>(
        effort_bit(Effort::Low) | effort_bit(Effort::Medium)
        | effort_bit(Effort::High));
    // `minimal` is OpenAI's gpt-5+ bottom tier (reasoning_effort="minimal").
    // It is NOT a Claude/compat level, so gate it to the GPT family from
    // gen 5 up. When the exact set is known (learned/models.dev) that wins
    // above; this only seeds the DERIVED default so the picker offers it.
    if (caps.family == ModelCapabilities::Family::Gpt && caps.generation >= 5)
        s |= effort_bit(Effort::Minimal);
    if (caps.supports_effort_xhigh()) s |= effort_bit(Effort::Xhigh);
    if (caps.supports_effort_max())   s |= effort_bit(Effort::Max);
    return s;
}

// True when the model should expose effort control. `caps` is expected to be
// the RESOLVED caps (from resolved_caps(id)), which has already folded in the
// per-model + env override; this is a thin, readable alias for supports_effort
// used by the picker view and the effort free functions below.
[[nodiscard]] inline bool effort_capable(const ModelCapabilities& caps) noexcept {
    return effort_set_of(caps) != 0;
}

// Nearest supported ON level to a requested one: prefer the closest level AT
// OR BELOW the request (don't think harder than asked), else the lowest level
// above it. The "map intent to nearest wire value" primitive — with a binary
// {high} set, every request maps to high; with {low,high}, medium maps low.
[[nodiscard]] constexpr Effort nearest_effort(
        Effort e, std::uint8_t set) noexcept {
    if (set == 0 || e == Effort::None) return Effort::None;
    constexpr Effort ladder[] = {Effort::Minimal, Effort::Low, Effort::Medium,
                                 Effort::High, Effort::Xhigh, Effort::Max};
    constexpr int kN = 6;
    int want = 0;
    for (int i = 0; i < kN; ++i) if (ladder[i] == e) { want = i; break; }
    for (int i = want; i >= 0; --i)          // at-or-below first
        if (set & effort_bit(ladder[i])) return ladder[i];
    for (int i = want + 1; i < kN; ++i)      // else lowest above
        if (set & effort_bit(ladder[i])) return ladder[i];
    return Effort::None;   // unreachable while set != 0
}

// Clamp an Effort to what a model actually supports and return its wire
// value. "" when the model can't take effort at all (or e == None). The
// provider calls this so a stale pick (e.g. Xhigh chosen, then a swap to a
// model without xhigh; or medium on a binary-enum Mistral model) silently
// degrades to the nearest supported level instead of 400ing.
[[nodiscard]] inline std::string_view effort_wire_for(
        Effort e, const ModelCapabilities& caps) noexcept {
    return effort_wire(nearest_effort(e, effort_set_of(caps)));
}

// Typed sibling of effort_wire_for: degrade a stored Effort to what `caps`
// actually supports and return the Effort itself (not the wire string). Used
// when SWITCHING MODEL/PROVIDER to fix up Model::effort so the picker chip and
// the request path agree — a stale Xhigh carried onto a model without xhigh
// becomes High here, and effort on a non-reasoning model collapses to None.
[[nodiscard]] inline Effort clamp_effort(
        Effort e, const ModelCapabilities& caps) noexcept {
    return nearest_effort(e, effort_set_of(caps));
}

// Ordered efforts the user may cycle for a given model: off + exactly the ON
// levels the model's API accepts, in ladder order. The picker renders THIS,
// so the user can never land on a level that would 400 — a binary-enum model
// shows off·high, a full-ladder flagship shows off·low·medium·high·xhigh·max.
[[nodiscard]] inline std::vector<Effort> available_efforts(
        const ModelCapabilities& caps) {
    const std::uint8_t set = effort_set_of(caps);
    std::vector<Effort> out{Effort::None};
    for (Effort e : {Effort::Minimal, Effort::Low, Effort::Medium, Effort::High,
                     Effort::Xhigh, Effort::Max})
        if (set & effort_bit(e)) out.push_back(e);
    return out;
}

// Step `cur` by `delta` (wrapping) within a model's available efforts.
// Returns None when the model doesn't support effort at all.
[[nodiscard]] inline Effort cycle_effort(
        Effort cur, int delta, const ModelCapabilities& caps) {
    if (!effort_capable(caps)) return Effort::None;
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
        //   Fable/Mythos 5+  -> 64000 (flagship lane, 128k-capable; 64k is the
        //                       safe raisable default, matching 4.x Opus/Sonnet.
        //                       AGENTTY_MAX_OUTPUT_TOKENS reaches the full 128k.)
        //   4.x Sonnet/Opus  -> 64000 (officially supported output ceiling)
        //   Haiku (any gen)  -> 8192  (Haiku's real output cap is 8k)
        //   <= 3.x           -> 8192  (older models 400 above this)
        //   unknown gen      -> 16384 (roomy but universally accepted)
        if (caps.is_haiku()) return 8192;
        // OpenAI gpt-5.x (ChatGPT/Codex Responses): large-output capable
        // (272k context); 64k is a safe, universally-accepted ceiling that
        // matches the flagship Claude lane. Explicit so intent is clear and a
        // future gpt generation doesn't silently fall through.
        if (caps.is_gpt()) return 64000;
        // Flagship lane (Fable/Mythos) and any Claude 4-or-later Sonnet/Opus.
        if (caps.is_fable() || caps.is_mythos()) return 64000;
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
