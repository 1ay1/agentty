// model_caps_test — ModelCapabilities::from_id weak-model inference.
//
// The weak_tool_use heuristic decides which models get the slim prompt +
// doom-loop guards. Lock the classification so a catalog edit can't silently
// flip a hosted/strong model into degraded mode (or vice-versa).

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "agtest.hpp"

#include "agentty/domain/catalog.hpp"

using namespace agentty;



static bool weak(std::string_view id) { return is_weak_model(id); }

TEST_CASE("claude never weak") {
    // Every hosted Claude family is strong, all generations.
    CHECK(!weak("claude-opus-4-5"));
    CHECK(!weak("claude-sonnet-4-5-20250101"));
    CHECK(!weak("claude-haiku-4-5"));
    CHECK(!weak("claude-3-5-haiku-20241022"));
    CHECK(!weak("claude-opus-4-5[1m]"));      // 1m suffix stripped first
}

TEST_CASE("small local coder weak") {
    // The model from the bug report and its small siblings.
    CHECK(weak("qwen2.5-coder:7b"));
    CHECK(weak("qwen2.5-coder:3b"));
    CHECK(weak("qwen2.5:7b"));
    CHECK(weak("codellama:7b"));
    CHECK(weak("deepseek-coder:6.7b"));
    CHECK(weak("phi3:3.8b"));
    CHECK(weak("gemma2:9b"));
    CHECK(weak("starcoder2:7b"));
    CHECK(weak("tinyllama:1.1b"));
    CHECK(weak("smollm2:1.7b"));
}

TEST_CASE("large models strong") {
    // Large models with NO weak-family signal follow tool schemas reliably.
    CHECK(!weak("llama3.1:70b"));
    CHECK(!weak("mixtral:8x22b"));            // 22b matched, no weak family
}

TEST_CASE("weak family wins over size") {
    // Known weak / coder-only families leak tool JSON at ANY size — the
    // weak-family signal beats the raw >= 14B size shortcut.
    CHECK(weak("qwen2.5-coder:14b"));         // the live case
    CHECK(weak("qwen2.5-coder:32b"));
    CHECK(weak("codellama:34b"));
    CHECK(weak("deepseek-coder:33b"));
}

TEST_CASE("tool trained families strong") {
    // Tool-trained families are strong even at smaller sizes.
    CHECK(!weak("qwen3:8b"));
    CHECK(!weak("llama3.1:8b"));
    CHECK(!weak("llama3.3:70b"));
    CHECK(!weak("mistral:7b"));
    CHECK(!weak("mistral-small:24b"));
    CHECK(!weak("ministral:8b"));
    CHECK(!weak("command-r:35b"));
    CHECK(!weak("hermes3:8b"));
    CHECK(!weak("firefunction-v2"));
    CHECK(!weak("functionary-small:7b"));
    CHECK(!weak("devstral:24b"));
    CHECK(!weak("codestral:22b"));
    CHECK(!weak("granite3.1-dense:8b"));
    CHECK(!weak("deepseek-v3"));
    CHECK(!weak("deepseek-r1:32b"));
}

TEST_CASE("tiny strong family still weak") {
    // Even a tool-trained family is unreliable when explicitly tiny (<= 3B).
    CHECK(weak("llama3.1:1b"));
    CHECK(weak("qwen3:1.7b"));               // 1b matched from "1.7b"? see note
    CHECK(weak("granite3.1-moe:3b"));
}

TEST_CASE("unknown id defaults strong") {
    // A hosted OpenAI-family id with no size/family signal is assumed capable.
    CHECK(!weak("gpt-4o"));
    CHECK(!weak("gpt-4o-mini"));
    CHECK(!weak("o1-preview"));
    CHECK(!weak("grok-2"));
    CHECK(!weak(""));
    CHECK(!weak("some-random-hosted-model"));
}

TEST_CASE("bare size signal") {
    // No recognised family but a small size tag → weak; large → strong.
    CHECK(weak("mystery:7b"));
    CHECK(!weak("mystery:70b"));
    // 'b' inside a word (not a param tag) must not be misread.
    CHECK(!weak("turbo-model"));             // 'b' in "turbo" ignored
    CHECK(!weak("bigbird"));                 // no leading digits
}

TEST_CASE("max output tokens") {
    // Claude 4.x Sonnet/Opus → 64000 (the edit-truncation fix).
    CHECK(max_output_tokens_for("claude-sonnet-4-5") == 64000);
    CHECK(max_output_tokens_for("claude-opus-4-5") == 64000);
    CHECK(max_output_tokens_for("claude-sonnet-4-5-20250101") == 64000);
    CHECK(max_output_tokens_for("claude-opus-4-5[1m]") == 64000);
    // Haiku caps at 8k at every generation.
    CHECK(max_output_tokens_for("claude-haiku-4-5") == 8192);
    CHECK(max_output_tokens_for("claude-3-5-haiku-20241022") == 8192);
    // Legacy 3.x Sonnet/Opus cap at 8k.
    CHECK(max_output_tokens_for("claude-3-5-sonnet-20241022") == 8192);
    CHECK(max_output_tokens_for("claude-3-opus-20240229") == 8192);
    // Non-Claude (local / OpenAI-compat / unknown) → conservative default.
    CHECK(max_output_tokens_for("qwen2.5-coder:7b") == 16384);
    CHECK(max_output_tokens_for("gpt-4o") == 16384);
    CHECK(max_output_tokens_for("") == 16384);
    // 2026 flagship lane (Fable/Mythos 5): Opus-class 64k ceiling, NOT the
    // 16k non-Claude default. Regression guard for the mid-turn truncation
    // caused by "fable" not being a recognised family token.
    CHECK(max_output_tokens_for("claude-fable-5") == 64000);
    CHECK(max_output_tokens_for("claude-mythos-5") == 64000);
    CHECK(max_output_tokens_for("claude-fable-5[1m]") == 64000);
}

TEST_CASE("flagship lane caps") {
    using agentty::ModelCapabilities;
    // Fable/Mythos 5 must decode as a KNOWN Claude family (else they'd be
    // treated as unknown/non-Claude: wrong output cap, no effort, no betas).
    for (const char* id : {"claude-fable-5", "claude-mythos-5"}) {
        const auto c = ModelCapabilities::from_id(id);
        CHECK(c.is_known_family());
        CHECK(c.is_flagship());
        CHECK(c.generation == 5);
        CHECK(c.generation_4_or_later);   // gates context-management beta
        CHECK(!c.is_weak_tool_user());
        // Effort control is GA on the flagship lane, full ladder.
        CHECK(c.supports_effort());
        CHECK(c.supports_effort_max());
        CHECK(c.supports_effort_xhigh());
    }
    // The [1m] extended-context suffix is stripped before decode.
    CHECK(ModelCapabilities::from_id("claude-fable-5[1m]").is_fable());
    CHECK(ModelCapabilities::from_id("claude-fable-5[1m]").extended_context_1m);
}

TEST_CASE("gpt5 codex caps") {
    using agentty::ModelCapabilities;
    using agentty::Effort;
    using agentty::effort_wire_for;

    // gpt-5.x (ChatGPT/Codex Responses) decodes as Family::Gpt with effort.
    {
        const auto c = ModelCapabilities::from_id("gpt-5.6-sol");
        CHECK(c.is_gpt());
        CHECK(c.is_known_family());
        CHECK(c.generation == 5);
        CHECK(c.revision == 6);
        CHECK(c.supports_effort());
        CHECK(c.supports_effort_xhigh());
        CHECK(c.supports_effort_max());     // gpt-5.6 flagship line takes max
        CHECK(!c.is_weak_tool_user());
    }
    // gpt-5.4 supports the ladder up to xhigh but NOT max.
    {
        const auto c = ModelCapabilities::from_id("gpt-5.4");
        CHECK(c.is_gpt());
        CHECK(c.generation == 5);
        CHECK(c.revision == 4);
        CHECK(c.supports_effort());
        CHECK(c.supports_effort_xhigh());
        CHECK(!c.supports_effort_max());
        // A stale `max` pick degrades to `high` rather than 400ing.
        CHECK(effort_wire_for(Effort::Max, c) == "high");
        CHECK(effort_wire_for(Effort::Xhigh, c) == "xhigh");
    }
    // Plain `gpt-5` (no revision) still gets effort.
    {
        const auto c = ModelCapabilities::from_id("gpt-5");
        CHECK(c.is_gpt());
        CHECK(c.generation == 5);
        CHECK(c.supports_effort());
    }
    // gpt-5.4-mini output ceiling is the large-output 64k (not the 16k
    // non-Claude default).
    CHECK(agentty::max_output_tokens_for("gpt-5.4-mini") == 64000);
    CHECK(agentty::max_output_tokens_for("gpt-5.6-sol") == 64000);

    // CRITICAL non-regression: gpt-4o / gpt-4.1 / gpt-3.5 must NOT be caught
    // by the gpt-5 family logic — they stay generic OpenAI-compat (no effort,
    // 16k cap). Older gpt-* over the OpenAI-compat transport relies on this.
    for (const char* legacy : {"gpt-4o", "gpt-4.1", "gpt-4o-mini", "gpt-3.5-turbo"}) {
        const auto c = ModelCapabilities::from_id(legacy);
        CHECK(!c.is_gpt());
        CHECK(!c.supports_effort());
        CHECK(agentty::max_output_tokens_for(legacy) == 16384);
    }
}

TEST_CASE("compat reasoning effort (chat wire)") {
    using agentty::ModelCapabilities;
    using agentty::Effort;
    using agentty::effort_wire_for;
    using agentty::cycle_effort;

    // OpenAI-Chat-wire reasoning models are NOT in the Claude/GPT family
    // ladder, but expose the top-level reasoning_effort enum (low|med|high).
    for (const char* id : {"mistral-small-latest", "mistral-medium-latest",
                           "mistral-small-3-2", "mistral-medium-3-5",
                           "deepseek-reasoner", "deepseek-r1",
                           "grok-4", "grok-4-fast", "grok-3-mini",
                           "gemini-2.5-flash-thinking",
                           "o1", "o3", "o4-mini"}) {
        const auto c = ModelCapabilities::from_id(id);
        CHECK(c.reasoning_compat);
        CHECK(c.supports_effort());
        // 3-level enum only — no max / xhigh on this wire.
        CHECK(!c.supports_effort_max());
        CHECK(!c.supports_effort_xhigh());
        CHECK(!c.is_known_family());   // stays orthogonal to the family ladder
        // A stale Max/Xhigh pick degrades to `high` instead of 400ing.
        CHECK(effort_wire_for(Effort::Max, c)    == "high");
        CHECK(effort_wire_for(Effort::Xhigh, c)  == "high");
        CHECK(effort_wire_for(Effort::Medium, c) == "medium");
        CHECK(effort_wire_for(Effort::None, c)   == "");
        // The picker can cycle effort on these models.
        CHECK(cycle_effort(Effort::None, +1, c) != Effort::None);
    }

    // Non-reasoning / native-reasoning lines that share a prefix must NOT light
    // up: grok-code-fast (coder), deepseek-chat, gemini-2.5-pro (no thinking
    // suffix), and — crucially — magistral-*, which reasons NATIVELY and
    // REJECTS reasoning_effort (422). codestral is a code model, no reasoning.
    for (const char* id : {"grok-code-fast-1", "magistral-medium-latest",
                           "magistral-small-latest", "codestral-latest",
                           "deepseek-chat", "gemini-2.5-pro"}) {
        const auto c = ModelCapabilities::from_id(id);
        CHECK(!c.reasoning_compat);
        CHECK(!c.supports_effort());
        CHECK(effort_wire_for(Effort::High, c) == "");
    }

    // Claude / GPT are family-gated — the compat flag must stay OFF for them
    // even though they DO support effort through their own ladder.
    for (const char* id : {"claude-opus-4-5", "gpt-5.6-sol"}) {
        const auto c = ModelCapabilities::from_id(id);
        CHECK(!c.reasoning_compat);
        CHECK(c.supports_effort());   // via family, not the compat flag
    }
}

TEST_CASE("per-model reasoning override registry") {
    using agentty::ModelCapabilities;
    using agentty::resolved_caps;
    using agentty::set_reasoning_override;
    using agentty::clear_reasoning_override;
    using agentty::clear_reasoning_overrides;
    using agentty::reasoning_override_for;

    clear_reasoning_overrides();

    // No override → resolved_caps matches pure inference.
    //   codestral-latest: a code model, NOT a reasoner → no compat.
    //   mistral-small-latest: Mistral Small 4, adjustable reasoning → compat.
    CHECK(!resolved_caps("codestral-latest").reasoning_compat);
    CHECK(resolved_caps("mistral-small-latest").reasoning_compat);
    CHECK(reasoning_override_for("codestral-latest") == -1);

    // Force ON a model the catalog does NOT recognize as a reasoner.
    set_reasoning_override("codestral-latest", true);
    CHECK(reasoning_override_for("codestral-latest") == 1);
    {
        const auto c = resolved_caps("codestral-latest");
        CHECK(c.reasoning_compat);
        CHECK(c.supports_effort());
        CHECK(!c.supports_effort_max());   // still a 3-level enum
    }

    // Force OFF a model inference WOULD light up (Mistral Small 4).
    set_reasoning_override("mistral-small-latest", false);
    CHECK(reasoning_override_for("mistral-small-latest") == 0);
    {
        const auto c = resolved_caps("mistral-small-latest");
        CHECK(!c.reasoning_compat);
        CHECK(!c.supports_effort());
    }

    // An override must NOT leak onto the family-gated Claude/GPT lane.
    set_reasoning_override("claude-opus-4-5", false);
    {
        const auto c = resolved_caps("claude-opus-4-5");
        CHECK(!c.reasoning_compat);
        CHECK(c.supports_effort());   // family ladder wins; override ignored
    }

    // Clearing one restores inference for that id only.
    clear_reasoning_override("mistral-small-latest");
    CHECK(reasoning_override_for("mistral-small-latest") == -1);
    CHECK(resolved_caps("mistral-small-latest").reasoning_compat);
    // The other override still stands (codestral forced on).
    CHECK(resolved_caps("codestral-latest").reasoning_compat);

    clear_reasoning_overrides();   // leave global state clean for other tests
    CHECK(reasoning_override_for("codestral-latest") == -1);
}

TEST_CASE("capability tiers") {
    using T = ModelCapabilities::Tier;
    auto tier = [](std::string_view id) { return ModelCapabilities::tier_for(id); };
    // Anthropic lane ordering matches the vendor's own naming.
    CHECK(tier("claude-haiku-4-5")        == T::Cheap);
    CHECK(tier("claude-3-5-haiku-20241022") == T::Cheap);
    CHECK(tier("claude-sonnet-4-5-20250101") == T::Mid);
    CHECK(tier("claude-opus-4-5")         == T::Flagship);
    // OpenAI: mini/nano demote to the cheap lane; full gpt-5.x is flagship/mid.
    CHECK(tier("gpt-5-mini")   == T::Cheap);
    CHECK(tier("gpt-4o-mini")  == T::Cheap);
    CHECK(tier("gpt-5-nano")   == T::Cheap);
    CHECK(tier("gpt-5.6")      == T::Flagship);
    // Weak local models never rank above Weak even with a cheap-ish id.
    CHECK(tier("qwen2.5-coder:7b") == T::Weak);
    // Ordering is a real total order.
    CHECK(T::Weak < T::Cheap);
    CHECK(T::Cheap < T::Mid);
    CHECK(T::Mid < T::Flagship);
}

static ModelInfo mi(std::string_view id) {
    ModelInfo m;
    m.id = ModelId{std::string{id}};
    return m;
}

TEST_CASE("router hardening nonchat and oseries") {
    using T = ModelCapabilities::Tier;
    // Non-chat assets a raw OpenAI /v1/models dump lists must NEVER be picked.
    for (const char* asset : {"text-embedding-3-small", "text-embedding-3-large",
                              "dall-e-3", "whisper-1", "tts-1", "tts-1-hd",
                              "omni-moderation-latest", "gpt-4o-transcribe",
                              "gpt-4o-realtime-preview", "gpt-image-1"}) {
        CHECK(!is_dispatchable_model(asset));
    }
    // Real chat models stay dispatchable.
    for (const char* chat : {"gpt-5", "gpt-5-mini", "claude-haiku-4-5",
                             "claude-opus-4-5", "o1", "o3-mini", "gpt-4o"}) {
        CHECK(is_dispatchable_model(chat));
    }
    // o-series: base o1/o3 are pricey reasoning models (Flagship, never picked
    // as "cheap"); the -mini variants are genuinely cheap.
    CHECK(ModelCapabilities::tier_for("o1")      == T::Flagship);
    CHECK(ModelCapabilities::tier_for("o3")      == T::Flagship);
    CHECK(ModelCapabilities::tier_for("o1-mini") == T::Cheap);
    CHECK(ModelCapabilities::tier_for("o3-mini") == T::Cheap);

    // End-to-end: a realistic raw OpenAI listing. The router must skip every
    // asset + o1 and land on the genuinely-cheap gpt-5-mini, not an embedding.
    std::vector<ModelInfo> raw = {
        mi("gpt-5"), mi("gpt-5-mini"), mi("o1"),
        mi("text-embedding-3-small"), mi("dall-e-3"), mi("whisper-1"),
        mi("gpt-4o-realtime-preview")};
    for (auto& m : raw)
        if (m.id.value.rfind("gpt-5", 0) == 0) m.context_window = 272000;
    const std::string picked = cheapest_capable_model("gpt-5", raw);
    CHECK(picked == "gpt-5-mini");
    CHECK(is_dispatchable_model(picked));

    // If the ONLY cheaper candidates are non-chat assets, keep the parent
    // (no regression, never route to an embedding model).
    std::vector<ModelInfo> assets_only = {
        mi("gpt-5"), mi("text-embedding-3-large"), mi("dall-e-3")};
    CHECK(cheapest_capable_model("gpt-5", assets_only) == "gpt-5");
}

TEST_CASE("cheapest capable router") {
    // Typical Anthropic pool: parent Opus, cheaper Sonnet + Haiku available.
    std::vector<ModelInfo> pool = {
        mi("claude-opus-4-5"), mi("claude-sonnet-4-5"), mi("claude-haiku-4-5")};
    // Read-only role floor defaults to Cheap → picks the CHEAPEST capable one.
    CHECK(cheapest_capable_model("claude-opus-4-5", pool) == "claude-haiku-4-5");
    // Parent already cheap → nothing strictly cheaper → keep the parent.
    CHECK(cheapest_capable_model("claude-haiku-4-5", pool) == "claude-haiku-4-5");
    // Single-model provider → no change, no regression.
    std::vector<ModelInfo> solo = {mi("claude-opus-4-5")};
    CHECK(cheapest_capable_model("claude-opus-4-5", solo) == "claude-opus-4-5");
    // Empty pool → keep parent.
    CHECK(cheapest_capable_model("claude-opus-4-5", {}) == "claude-opus-4-5");
    // A weak local model in the pool is never chosen.
    std::vector<ModelInfo> mixed = {
        mi("claude-opus-4-5"), mi("qwen2.5-coder:7b"), mi("claude-sonnet-4-5")};
    CHECK(cheapest_capable_model("claude-opus-4-5", mixed) == "claude-sonnet-4-5");
    // A probe that reported no tool support disqualifies a candidate.
    std::vector<ModelInfo> notools = {mi("claude-opus-4-5"), mi("claude-haiku-4-5")};
    notools[1].supports_tools = false;
    CHECK(cheapest_capable_model("claude-opus-4-5", notools) == "claude-opus-4-5");

    // REGRESSION: the routed id must NEVER carry the `[1m]`/`[2m]` extended-
    // context picker marker. That marker makes the transport send the
    // `context-1m` beta, which 400s the whole subagent request on a
    // subscription that isn't entitled to the long-context beta
    // ("long context beta is not yet available for this subscription"). A
    // subagent never needs a 1M window, so the router strips it — both when a
    // cheaper candidate is chosen and when the `[1m]` parent is kept as-is.
    std::vector<ModelInfo> pool_1m = {
        mi("claude-opus-4-5"), mi("claude-sonnet-4-5"), mi("claude-haiku-4-5")};
    CHECK(cheapest_capable_model("claude-opus-4-5[1m]", pool_1m) == "claude-haiku-4-5");
    // Parent-kept path (nothing strictly cheaper) still drops the marker.
    std::vector<ModelInfo> solo_1m = {mi("claude-opus-4-5[1m]")};
    CHECK(cheapest_capable_model("claude-opus-4-5[1m]", solo_1m) == "claude-opus-4-5");
    CHECK(cheapest_capable_model("claude-opus-4-5[1m]", {}) == "claude-opus-4-5");
}

// A subagent must NEVER carry the picker-only `[1m]`/`[2m]` extended-context
// marker: it would make the transport send the entitlement-gated context-1m
// beta, which 400s ("long context beta is not yet available") and kills the
// whole fan-out. run_one_completion derives req.model as
// `wire_model_id(<cheapest-or-parent>)`; this locks that the marker is gone in
// every branch (cheaper-found, kept-parent, write-role, single-model).
TEST_CASE("subagent model never 1m") {
    using agentty::wire_model_id;
    auto has_1m = [](std::string_view s) {
        return s.find("[1m]") != std::string_view::npos
            || s.find("[2m]") != std::string_view::npos;
    };

    // Parent on a 1M variant, cheaper NON-1M model available (read-only role):
    // router picks the cheaper clean id, wire_model_id is a further no-op.
    std::vector<ModelInfo> pool = {
        mi("claude-opus-4-5"), mi("claude-sonnet-4-5"), mi("claude-haiku-4-5")};
    auto ro = wire_model_id(cheapest_capable_model("claude-opus-4-5[1m]", pool));
    CHECK(!has_1m(ro));
    CHECK(ro == "claude-haiku-4-5");

    // Single-model provider on a 1M variant: router KEEPS the parent (with the
    // marker) — wire_model_id is what strips it. This is the exact case that
    // used to 400.
    std::vector<ModelInfo> solo = {mi("claude-opus-4-5")};
    auto kept = wire_model_id(cheapest_capable_model("claude-opus-4-5[1m]", solo));
    CHECK(!has_1m(kept));
    CHECK(kept == "claude-opus-4-5");

    // Write-role path (req.model = cfg.model, no router): still stripped.
    CHECK(!has_1m(wire_model_id(std::string{"claude-sonnet-4-5[1m]"})));
    CHECK(wire_model_id(std::string{"claude-sonnet-4-5[1m]"}) == "claude-sonnet-4-5");

    // A plain (non-1M) parent is unaffected end-to-end.
    CHECK(wire_model_id(cheapest_capable_model("claude-sonnet-4-5", pool))
          == "claude-haiku-4-5");
}

TEST_CASE("context window detection") {
    using agentty::ModelCapabilities;
    auto win = [](std::string_view id) {
        return ModelCapabilities::from_id(id).context_window();
    };
    auto suffix = [](std::string_view id) {
        return ModelCapabilities::from_id(id).supports_1m_suffix();
    };

    // supports_1m_suffix(): mirrors Claude Code's catalog `supports_1m_suffix`
    // flag — the Sonnet-4 line, Opus-4 line, and Haiku 4.5 may be offered a
    // `[1m]` variant; gen<=3 models may not.
    CHECK(suffix("claude-sonnet-4-5"));
    CHECK(suffix("claude-sonnet-4-5-20250101"));
    CHECK(suffix("claude-sonnet-4"));
    CHECK(suffix("claude-opus-4-5"));
    CHECK(suffix("claude-haiku-4-5"));
    CHECK(!suffix("claude-3-5-sonnet-20241022"));   // gen 3, no 1M variant
    CHECK(!suffix("claude-3-opus"));
    // Flagship next-gen lane (Fable/Mythos 5+).
    CHECK(suffix("claude-fable-5"));
    CHECK(suffix("claude-mythos-5"));

    // context_window(): base 200k for every known Claude model; 1M ONLY when
    // the explicit `[1m]` variant is picked (matches Claude Code, which keeps
    // the base id at 200k and binds 1M to the `<id>[1m]` picker entry).
    CHECK(win("claude-sonnet-4-5") == 200000);        // base id → 200k
    CHECK(win("claude-sonnet-4-5[1m]") == 1000000);   // [1m] variant → 1M
    CHECK(win("claude-opus-4-5") == 200000);
    CHECK(win("claude-opus-4-5[1m]") == 1000000);
    CHECK(win("claude-haiku-4-5") == 200000);
    CHECK(win("claude-haiku-4-5[1m]") == 1000000);
    // Unknown / local families report 0 so the caller prefers a probed window.
    CHECK(win("qwen2.5-coder:7b") == 0);
    CHECK(win("some-random-model") == 0);

    // COMPACTION INVARIANT: the compaction path strips the `[1m]` marker
    // (wire_model_id) so the wire window is the BASE, then trims the payload
    // to that base — NOT to 65% of 1M, which would overflow the 200K request.
    // context_max_for_model composes these; assert the pure equivalent here:
    // the window of the STRIPPED id is always the 200K base, never 1M.
    {
        using agentty::wire_model_id;
        CHECK(win(wire_model_id("claude-opus-4-5[1m]")) == 200000
              && "compaction runs on the base window, not the parent's 1M");
        CHECK(win(wire_model_id("claude-sonnet-4-5[1m]")) == 200000);
        // A non-1M parent is unchanged.
        CHECK(win(wire_model_id("claude-opus-4-5")) == 200000);
    }
}

TEST_CASE("wire model id strip") {
    using agentty::wire_model_id;
    // The `[1m]`/`[2m]` picker marker must NEVER reach the wire (the API 404s
    // on `claude-sonnet-5[1m]`). wire_model_id strips it, mirroring Claude
    // Code's Yu().
    CHECK(wire_model_id("claude-sonnet-5[1m]") == "claude-sonnet-5");
    CHECK(wire_model_id("claude-opus-4-8[1m]") == "claude-opus-4-8");
    CHECK(wire_model_id("claude-sonnet-4-5[2m]") == "claude-sonnet-4-5");
    // A bare id (no marker) is returned unchanged.
    CHECK(wire_model_id("claude-opus-4-8") == "claude-opus-4-8");
    CHECK(wire_model_id("gpt-5.6-sol") == "gpt-5.6-sol");
    CHECK(wire_model_id("qwen2.5-coder:7b") == "qwen2.5-coder:7b");
    // Marker mid-string (defensive) is still removed.
    CHECK(wire_model_id("claude-opus-4-8[1m]-preview") == "claude-opus-4-8-preview");
    CHECK(wire_model_id("") == "");
}

TEST_CASE("model picker ordering") {
    using agentty::ModelInfo;
    using agentty::ModelId;
    using agentty::model_picker_less;

    auto mi = [](std::string_view id) {
        return ModelInfo{ModelId{std::string{id}}, std::string{id}, "anthropic"};
    };

    // Regression: Fable/Mythos are the NEWEST flagship-tier lane and must
    // never sink below Opus/Sonnet/Haiku just because a fixed family-name
    // bucket order happened to list them last. Feed the exact jumble Anthropic
    // returns (arbitrary /v1/models order) and sort with the shared
    // comparator.
    std::vector<ModelInfo> v = {
        mi("claude-opus-4-8"),   mi("claude-opus-4-7"),
        mi("claude-sonnet-4-6"), mi("claude-opus-4-6"),
        mi("claude-opus-4-5"),   mi("claude-haiku-4-5"),
        mi("claude-sonnet-4-5"), mi("claude-fable-5"),
    };
    std::stable_sort(v.begin(), v.end(), model_picker_less);

    // Fable 5 (flagship tier, generation 5) must lead every Opus (flagship
    // tier, generation 4.x) — newest flagship wins, not last-in-bucket.
    CHECK(v.front().id.value == "claude-fable-5");

    // Every flagship-tier model (Opus + Fable) sorts before every Mid-tier
    // model (Sonnet), which sorts before every Cheap-tier model (Haiku).
    auto flagship_end = std::find_if(v.begin(), v.end(), [](const ModelInfo& m) {
        return ModelCapabilities::from_id(m.id.value).tier()
            != ModelCapabilities::Tier::Flagship;
    });
    auto sonnet_end = std::find_if(flagship_end, v.end(), [](const ModelInfo& m) {
        return ModelCapabilities::from_id(m.id.value).tier()
            != ModelCapabilities::Tier::Mid;
    });
    CHECK(std::distance(v.begin(), flagship_end) == 5);   // 4 Opus + 1 Fable
    CHECK(std::distance(flagship_end, sonnet_end) == 2);  // 2 Sonnet
    CHECK(sonnet_end == v.end() - 1);                     // 1 Haiku, trailing

    // Within the flagship tier, newest generation leads (Fable 5 > Opus 4.8
    // > Opus 4.7 > Opus 4.6 > Opus 4.5).
    std::vector<std::string> flagship_order;
    for (auto it = v.begin(); it != flagship_end; ++it)
        flagship_order.push_back(it->id.value);
    CHECK((flagship_order == std::vector<std::string>{
        "claude-fable-5", "claude-opus-4-8", "claude-opus-4-7",
        "claude-opus-4-6", "claude-opus-4-5"}));
}
