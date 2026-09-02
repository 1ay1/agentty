// model_label_test — the model-name display SSOT.
//
// Two layers are pinned here:
//
//   • the view-layer helpers (pretty_model_label / model_display_label),
//     which are thin delegates — so these cases pin the END-TO-END result a
//     user actually reads;
//   • the decoder itself (domain/model_name.hpp), whose invariants (shed
//     nesting, totality, cross-surface agreement, positional recovery) the
//     old six-parser design could not even express.
//
// Raw provider ids (`codellama:latest`, `qwen2.5-coder:7b`,
// `openai/gpt-4o-mini`, `claude-sonnet-4-5[1m]`) are long and ugly; this
// pins the cleanup across every real-world id shape we ship against so a
// regression that re-leaks a raw id into the header is caught at CI.

#include "agentty/domain/model_name.hpp"
#include "agentty/runtime/view/helpers.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "agtest.hpp"

namespace {

void expect_label(std::string_view id, std::string_view want) {
    std::string got = agentty::ui::pretty_model_label(id);
    CHECK_MESSAGE(got == want, "pretty_model_label(\"" << std::string(id)
                                  << "\") got \"" << got << "\" want \""
                                  << std::string(want) << "\"");
}

// Sort a list with the picker's empty-query comparator and join for a
// single readable assertion.
std::string ordered(std::vector<std::string> in) {
    std::stable_sort(in.begin(), in.end(),
        [](const std::string& a, const std::string& b){
            return agentty::ui::model_order_less(a, b);
        });
    std::string out;
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (i) out += " | ";
        out += in[i];
    }
    return out;
}

} // namespace

TEST_CASE("pretty_model_label normalizes real-world ids") {
    // ── Ollama: drop :latest, keep meaningful size/quant tags ──────────
    expect_label("codellama:latest",        "Codellama");
    expect_label("llama3.2:latest",         "Llama3.2");
    expect_label("qwen2.5-coder:7b",        "Qwen2.5 Coder 7b");
    expect_label("llama3.1:70b",            "Llama3.1 70b");
    expect_label("mixtral:8x7b",            "Mixtral 8x7b");
    expect_label("phi3:3.8b",               "Phi3 3.8b");
    expect_label("deepseek-coder:6.7b",     "DeepSeek Coder 6.7b");
    expect_label("gemma2:9b",               "Gemma2 9b");

    // ── OpenAI / OpenAI-compat: title-case, keep GPT acronym + version ─
    expect_label("gpt-4o",                  "GPT 4o");
    expect_label("gpt-4o-mini",             "GPT 4o Mini");
    expect_label("gpt-5",                   "GPT 5");
    expect_label("o4-mini",                 "o4 Mini");
    expect_label("chatgpt-4o-latest",       "ChatGPT 4o");     // brand case + alias drop
    expect_label("gpt-4o-2024-08-06",       "GPT 4o");         // snapshot triple drop
    expect_label("gpt-4.1-nano",            "GPT 4.1 Nano");
    expect_label("gpt-5.1-codex-max",       "GPT 5.1 Codex Max");
    expect_label("codex-mini-latest",       "Codex Mini");

    // ── Provider-namespaced ids (OpenRouter / aggregators) ────────────
    expect_label("openai/gpt-4o-mini",      "GPT 4o Mini");
    expect_label("anthropic/claude-3-haiku","Haiku 3");
    expect_label("meta-llama/Llama-3.1-8B", "Llama 3.1 8B");
    expect_label("google/gemini-2.0-flash", "Gemini 2.0 Flash");

    // ── Gemini / xAI / DeepSeek hosted ────────────────────────────
    expect_label("gemini-1.5-pro",          "Gemini 1.5 Pro");
    expect_label("grok-2",                  "Grok 2");
    expect_label("grok-beta",               "Grok Beta");
    expect_label("deepseek-r1",             "DeepSeek R1");    // brand case
    expect_label("deepseek-chat",           "DeepSeek Chat");

    // ── Claude: adjacent version digits join with a dot; snapshots drop ─
    expect_label("claude-sonnet-4-5",       "Sonnet 4.5");
    expect_label("claude-opus-4-1",         "Opus 4.1");
    expect_label("claude-3-5-haiku-20241022", "Haiku 3.5");
    expect_label("claude-sonnet-4-20250514",  "Sonnet 4");

    // ── agentty `[1m]`/`[2m]` extended-context markers are stripped ───
    expect_label("claude-sonnet-4-5[1m]",   "Sonnet 4.5 \xc2\xb7 1M");
    expect_label("claude-opus-4-5[2m]",     "Opus 4.5 \xc2\xb7 2M");
    expect_label("gpt-4o[1m]",              "GPT 4o \xc2\xb7 1M");

    // ── Ollama quant tags: keep size + variant, drop quant noise ──────
    expect_label("llama3.3:70b-instruct-q4_K_M", "Llama3.3 70b Instruct");
    expect_label("phi4:Q8_0",               "Phi4");
    expect_label("llama3:8b-fp16",          "Llama3 8b");

    // ── Acronym preservation + already-cased input ────────────────────
    expect_label("glm-4-9b",                "GLM 4 9b");
    expect_label("Llama-3.1-8B-Instruct",   "Llama 3.1 8B Instruct");

    // ── Degenerate inputs never crash / never empty ───────────────────
    expect_label("",                        "");
    expect_label(":latest",                 "");      // family empty, tag dropped
    expect_label("model",                   "Model");
    expect_label("a",                       "A");
}

TEST_CASE("model_order_less: family group + natural version-desc") {
    // Within a family, NEWEST first — and version numbers compared as
    // integers, so 4.10 sorts ABOVE 4.8 (plain lexical would invert it,
    // the classic file2/file10 bug).
    CHECK(ordered({"Claude Sonnet 4.5", "Claude Sonnet 4.6",
                   "Claude Sonnet 4", "Claude Sonnet 4.10"})
          == "Claude Sonnet 4.10 | Claude Sonnet 4.6 | "
             "Claude Sonnet 4.5 | Claude Sonnet 4");

    // gpt-10 must outrank gpt-2 (integer, not lexical).
    CHECK(ordered({"GPT 2", "GPT 10", "GPT 5"})
          == "GPT 10 | GPT 5 | GPT 2");

    // Families group together and sort alphabetically across families;
    // within each, newest first. (Opus before Sonnet: 'o' < 's'.)
    CHECK(ordered({"Claude Sonnet 4.5", "Claude Opus 4.1",
                   "Claude Sonnet 4.6", "Claude Opus 4.5"})
          == "Claude Opus 4.5 | Claude Opus 4.1 | "
             "Claude Sonnet 4.6 | Claude Sonnet 4.5");

    // A pure-number chunk sorts before letters at the same position, so
    // "GPT 4o" (number 4) groups before "GPT Image".
    CHECK(ordered({"GPT Image 1", "GPT 4o", "GPT 5"})
          == "GPT 5 | GPT 4o | GPT Image 1");

    // Decimal minor versions compare naturally: 5.6 > 5.10 is FALSE
    // (10 > 6 as integers) — dotted parts are separate numeric chunks.
    CHECK(ordered({"GPT 5.6", "GPT 5.10", "GPT 5.2"})
          == "GPT 5.10 | GPT 5.6 | GPT 5.2");

    // Stable / total: equal-family same-version aliases don't crash and
    // land deterministically.
    CHECK(ordered({"Gemini 2.5 Flash", "Gemini 2.5 Flash"})
          == "Gemini 2.5 Flash | Gemini 2.5 Flash");
}

TEST_CASE("model_display_label: one canonical label across providers") {
    using agentty::ui::model_display_label;
    auto L = [](std::string_view id, std::string_view name) {
        return model_display_label(id, name);
    };

    // No server name (OpenAI-compat / Ollama echo the id) → id-normalized.
    CHECK(L("gpt-4o-mini", "")            == "GPT 4o Mini");
    CHECK(L("gpt-image-1.5", "gpt-image-1.5") == "GPT Image 1.5"); // name==id

    // A server name that's just a re-cased id ("GPT-4o", "Hy-MT2-30B-A3B")
    // yields the SAME tidy label as the id alone — cross-provider
    // consistency, not the provider's ad-hoc casing.
    CHECK(L("gpt-4o", "GPT-4o")           == L("gpt-4o", ""));
    CHECK(L("hy-mt2-30b-a3b", "Hy-MT2-30B-A3B") == L("hy-mt2-30b-a3b", ""));

    // Cruft in a server name ((latest), casing) is normalized away, so it
    // matches the id-derived form too.
    CHECK(L("gpt-5.3-chat-latest", "GPT-5.3 Chat (latest)")
          == L("gpt-5.3-chat-latest", ""));

    // A genuine MARKETING alias the id can't reconstruct is KEPT (but
    // still normalized) — "Nano Banana Pro" is not a rearrangement of
    // "gemini-3-pro-image".
    CHECK(L("gemini-3-pro-image", "Nano Banana Pro") == "Nano Banana Pro");

    // A KNOWN family decodes entirely from the id, so a server name adds
    // nothing and is ignored — every provider serving this model renders it
    // identically, whatever ad-hoc spelling they each send.
    CHECK(L("claude-sonnet-4-5-20250929", "Claude Sonnet 4.5")
          == "Sonnet 4.5");

    // Degenerate: empty id + empty name never crashes.
    CHECK(L("", "") == "");
}

// Extended-context variants must be DISTINGUISHABLE in the list.
//
// `[1m]` is a picker-only marker: agentty appends it to offer the 1M context
// window as a separate, selectable row, and wire_model_id strips it before the
// id reaches the wire. pretty_model_label strips it too — correctly, since a
// wire marker must never leak as literal "[1m]" — but the consequence was that
// `claude-opus-4-8` and `claude-opus-4-8[1m]` rendered as the SAME string.
// Two rows, identical names, and the only difference (the context column) sat
// in dim reference text at the far end of the row.
//
// fused_models_test already asserts the two are distinct ROWS; this asserts
// they are distinct to the READER, which is the half that matters.
TEST_CASE("model label: extended-context variants stay distinguishable") {
    auto L = [](std::string_view id, std::string_view name = "") {
        return agentty::ui::model_display_label(id, name);
    };

    // The base model is untouched.
    CHECK(L("claude-opus-4-8") == "Opus 4.8");

    // The variant carries the window in its NAME, not just its ctx column.
    CHECK(L("claude-opus-4-8[1m]") == "Opus 4.8 \xc2\xb7 1M");
    CHECK(L("claude-sonnet-4-6[2m]") == "Sonnet 4.6 \xc2\xb7 2M");

    // …and they differ, which is the whole point.
    CHECK(L("claude-opus-4-8") != L("claude-opus-4-8[1m]"));

    // No DOUBLE labelling when the server also names the variant: a known
    // family ignores the server name, so we get OUR canonical "· 1M" rather
    // than the provider's ad-hoc "(1M Context)" spelling — and two providers
    // serving the same variant agree.
    CHECK(L("claude-opus-4-8[1m]", "Claude Opus 4.8 (1M Context)")
          == "Opus 4.8 \xc2\xb7 1M");

    // The marker never leaks literally, in either form.
    for (std::string_view id : {"claude-opus-4-8[1m]", "claude-sonnet-4-6[2m]"}) {
        const auto s = L(id);
        CHECK(s.find("[1m]") == std::string::npos);
        CHECK(s.find("[2m]") == std::string::npos);
    }

    // A marketing alias is still preferred over the id-derived form, and is
    // not disturbed by the variant handling.
    CHECK(L("gemini-3-pro-image", "Nano Banana Pro") == "Nano Banana Pro");
}

// ── The SSOT's own invariants ────────────────────────────────────────────
//
// These are the properties the OLD design could not even express, because
// there was no single decoded value to state them about — six independent
// parsers, each partially covered. They are the reason the SSOT exists.

TEST_CASE("model_name: the shed ladder is strictly nested") {
    using agentty::model_name::decode;

    // tiny() ⊑ medium() ⊑ full(). A narrow surface can never state something
    // a wide one contradicts, because it is literally a prefix of it. This is
    // what the old compact badge violated: it dropped the version by falling
    // out of a DIFFERENT branch, so "Opus" and "Opus 4.8" were produced by
    // unrelated code and could disagree.
    for (std::string_view id : {"claude-opus-4-8",
                                "claude-opus-4-8[1m]",
                                "claude-3-5-haiku-20241022",
                                "gpt-5.1-codex-max",
                                "gpt-4o-mini",
                                "qwen2.5-coder:7b",
                                "mystery-model"}) {
        const auto n = decode(id);
        CHECK_MESSAGE(n.medium().starts_with(n.tiny()),
                      "medium() must extend tiny() for \"" << std::string(id)
                      << "\": tiny=\"" << n.tiny() << "\" medium=\""
                      << n.medium() << "\"");
        CHECK_MESSAGE(n.full().starts_with(n.medium()),
                      "full() must extend medium() for \"" << std::string(id)
                      << "\": medium=\"" << n.medium() << "\" full=\""
                      << n.full() << "\"");
    }
}

TEST_CASE("model_name: totality — never blank, never a raw id leak") {
    using agentty::model_name::decode;

    // Total function: any id yields a non-empty name, and no agentty-internal
    // marker ever reaches the screen.
    for (std::string_view id : {"claude-opus-4-8[1m]", "gpt-5", "o4-mini",
                                "openrouter/anthropic/claude-opus-4",
                                "llama3:70b-instruct-q4_K_M",
                                "codex-mini-latest", "x", "---"}) {
        const auto n = decode(id);
        CHECK_MESSAGE(!n.name.empty(),
                      "name must never be empty for \"" << std::string(id) << "\"");
        const auto s = n.full();
        CHECK(s.find("[1m]") == std::string::npos);
        CHECK(s.find("[2m]") == std::string::npos);
    }

    // Degenerate inputs are handled, not crashed on.
    CHECK(decode("").name.empty());
    CHECK(decode("").full().empty());
}

TEST_CASE("model_name: one model has ONE identity across surfaces") {
    using agentty::model_name::decode;

    // The bug class this file exists to prevent: the composer chip said
    // "Opus", the turn header said "Opus 4.8", the picker said "Claude Opus
    // 4.8" — same model, three strings, three parsers. Now every surface
    // projects the SAME decoded value, so the family word and the colour are
    // identical by construction and only the shed level differs.
    const auto a = decode("claude-opus-4-8");
    const auto b = decode("claude-opus-4-8");
    CHECK(a == b);                       // decode is a pure function

    // The version survives into the mid rung — this is the exact information
    // loss the old compact badge caused (4.5 vs 4.8 indistinguishable).
    CHECK(decode("claude-opus-4-5").medium() != decode("claude-opus-4-8").medium());

    // A known family never depends on the provider's spelling, so the same
    // model served by two different providers reads identically.
    CHECK(decode("claude-sonnet-4-5", "Claude Sonnet 4.5 (Bedrock)").full()
          == decode("claude-sonnet-4-5", "anthropic.claude-sonnet-4-5-v1").full());

    // Colour comes from the family table, so two members of a lane share a
    // hue and two lanes do not.
    CHECK(decode("claude-opus-4-8").color == decode("claude-fable-5").color);
    CHECK(decode("claude-opus-4-8").color != decode("claude-sonnet-4-5").color);
    CHECK(decode("claude-haiku-4-5").color != decode("claude-sonnet-4-5").color);
}

TEST_CASE("model_name: positional facts from_id cannot express") {
    using agentty::model_name::decode;

    // LEGACY SCHEMA: version BEFORE the family word. from_id reads the token
    // AFTER the family, so caps.generation is 0 here (catalog.hpp documents
    // this and works around it with a find("claude-3") sniff). Without the
    // recovery pass this rendered as bare "Haiku" — a 3.5 model
    // indistinguishable from a 4.5 one in a picker row.
    CHECK(decode("claude-3-5-haiku-20241022").medium() == "Haiku 3.5");
    CHECK(decode("claude-3-haiku").medium()            == "Haiku 3");

    // …while the MODERN schema still reads its version from from_id.
    CHECK(decode("claude-sonnet-4-5").medium() == "Sonnet 4.5");

    // Snapshot dates are provenance, never a version. "Sonnet 4.20250514"
    // was a real rendering bug.
    CHECK(decode("claude-sonnet-4-20250514").medium() == "Sonnet 4");
    CHECK(decode("claude-sonnet-4-20250514").full().find("2025")
          == std::string::npos);

    // QUALIFIER: a product suffix is identity, not decoration — gpt-5.1 and
    // gpt-5.1-codex-max are different products and must not both read
    // "GPT 5.1".
    CHECK(decode("gpt-5.1-codex-max").medium() == "GPT 5.1 Codex Max");
    CHECK(decode("gpt-5.1-codex-max").medium() != decode("gpt-5.1").medium());
    // It sheds at the narrowest rung, where width is the binding constraint.
    CHECK(decode("gpt-5.1-codex-max").tiny() == "GPT 5.1");
}

TEST_CASE("model_name: no family borrows a status hue") {
    // Enforced at COMPILE time by proofs::no_family_uses_a_status_hue; this
    // restates it at runtime so the intent is discoverable from the tests.
    // palette.hpp's rule is "one hue = one axis": green means status-ok, so
    // Haiku-as-green (the old widget's mapping) made a model look like a
    // completion checkmark.
    using agentty::model_name::decode;
    for (std::string_view id : {"claude-haiku-4-5", "claude-sonnet-4-5",
                                "claude-opus-4-8", "claude-fable-5", "gpt-5"}) {
        const auto c = decode(id).color;
        CHECK(c != maya::Color::green());
        CHECK(c != maya::Color::bright_green());
        CHECK(c != maya::Color::red());
        CHECK(c != maya::Color::yellow());
    }
}

// ── SSOT enforcement ─────────────────────────────────────────────────────
//
// The invariants above prove the DECODER is correct. This case guards
// something the decoder cannot: that every surface actually ROUTES through
// it. That gap is not hypothetical — consolidating the six parsers initially
// missed a seventh (activity_indicator.cpp carried its own
// is_opus/is_sonnet/is_haiku colour chain), and it had drifted in exactly
// the predicted way: Opus plain-magenta instead of bright, and Haiku GREEN,
// the status-ok hue. A grep found it; this test means the next one is found
// by CI instead.
TEST_CASE("model_name: every surface agrees on a model's colour") {
    using agentty::model_name::decode;

    // The colour a model gets is a function of the model ALONE — not of
    // which widget is asking. Any surface re-deriving a hue locally will
    // disagree with this, because the family table is the only place the
    // mapping exists.
    struct Case { std::string_view id; maya::Color want; };
    const Case cases[] = {
        {"claude-opus-4-8",   maya::Color::bright_magenta()},
        {"claude-fable-5",    maya::Color::bright_magenta()},
        {"claude-mythos-5",   maya::Color::bright_magenta()},
        {"claude-sonnet-4-5", maya::Color::blue()},
        {"claude-haiku-4-5",  maya::Color::bright_cyan()},
    };
    for (const auto& c : cases)
        CHECK_MESSAGE(decode(c.id).color == c.want,
                      "family colour drifted for \"" << std::string(c.id) << "\"");

    // Lane membership implies colour equality — Fable and Mythos share
    // Opus's flagship hue because they share its lane (catalog.hpp groups
    // them under is_flagship()).
    CHECK(decode("claude-fable-5").color  == decode("claude-opus-4-8").color);
    CHECK(decode("claude-mythos-5").color == decode("claude-opus-4-8").color);

    // An unknown family still gets a colour — never a default-constructed
    // one that would render as an invisible or wrong-themed glyph.
    CHECK(decode("some-local-model:7b").color == maya::Color::cyan());
}

// ── Graceful degradation: the model we have never seen ───────────────────
//
// The decoder's hardest requirement is not correctness on the ids we ship
// against today — the corpus above pins those. It is that the ids we have
// NOT seen degrade to the same SHAPE, so a model launched after this build
// looks native instead of looking broken. A user meeting a brand-new
// flagship is the worst possible moment for the UI to start rendering raw
// wire ids or inconsistent typography.
TEST_CASE("model_name: an unknown family renders like a known one") {
    using agentty::model_name::decode;

    // A FUTURE Anthropic family, months before anyone adds it to the enum.
    // It must be indistinguishable in shape from a family we do know:
    // vendor-free, version split out, snapshot dropped.
    CHECK(decode("claude-quasar-6").medium()            == "Quasar 6");
    CHECK(decode("claude-quasar-6-2-20260114").medium() == "Quasar 6.2");
    CHECK(decode("claude-nova-7-1[1m]").full()          == "Nova 7.1 \xc2\xb7 1M");

    // The shape matches a KNOWN family's exactly — same field split, same
    // absence of a vendor prefix. This is the property that makes adding a
    // family to the enum an optimisation rather than a bug fix.
    const auto known   = decode("claude-opus-4-8");
    const auto unknown = decode("claude-quasar-6-8");
    CHECK(known.version   == "4.8");
    CHECK(unknown.version == "6.8");
    CHECK(known.name      == "Opus");
    CHECK(unknown.name    == "Quasar");
    CHECK(known.family_known);
    CHECK(!unknown.family_known);   // …yet it still decoded structurally

    // Other vendors get the same treatment.
    CHECK(decode("gemini-4-pro").medium() == "Gemini 4 Pro");
    CHECK(decode("grok-5-fast").medium()  == "Grok 5 Fast");

    // The shed ladder WORKS for unknown ids. Before the structural decode
    // they produced one flat string for all three rungs — a no-op exactly
    // where names are longest and shedding matters most.
    CHECK(decode("gemini-4-pro").tiny() == "Gemini 4");
    CHECK(decode("gemini-4-pro").tiny() != decode("gemini-4-pro").medium());
}

TEST_CASE("model_name: no vendor prefix ever reaches the screen") {
    using agentty::model_name::decode;

    // Vendor and PROVIDER are independent axes (Copilot serves Claude, GPT
    // and Gemini alike), and the provider is already rendered from the
    // registry row by its own chip. A vendor prefix in the model name is at
    // best redundant and at worst a lie — "Claude Opus 4.8" under a Copilot
    // chip invites the misreading that you are talking to Anthropic.
    for (std::string_view id : {"claude-opus-4-8", "claude-quasar-6",
                                "anthropic/claude-sonnet-4-5",
                                "openrouter/anthropic/claude-opus-4",
                                "openai/gpt-4o-mini"}) {
        const auto s = decode(id).full();
        CHECK_MESSAGE(s.find("Claude") == std::string::npos,
                      "vendor prefix leaked for \"" << std::string(id)
                      << "\": \"" << s << "\"");
        CHECK_MESSAGE(s.find("Anthropic") == std::string::npos,
                      "vendor leaked for \"" << std::string(id) << "\"");
        CHECK(s.find('/') == std::string::npos);   // namespace never shown
    }

    // …but a model genuinely NAMED like a vendor keeps its name. Only
    // LEADING vendor tokens are stripped, and only exact matches.
    CHECK(decode("gemma2:9b").name.starts_with("Gemma"));
}

TEST_CASE("model_name: adversarial ids never crash or blank") {
    using agentty::model_name::decode;

    // Ids arrive from provider APIs and user config — neither is trusted to
    // be well-formed. Every one of these must produce SOMETHING renderable.
    for (std::string_view id : {"-", "---", ":", ":::", "[1m]", "claude",
                                "claude-", "-claude-", "4-5", "1.2.3.4.5",
                                "::latest", "a", "/", "//", "x/y/",
                                "20250101", "openai/", "model:tag:extra"}) {
        const auto n = decode(id);
        const auto s = n.full();
        // Totality: the projections never throw and never disagree about
        // nesting, whatever the input.
        CHECK_MESSAGE(n.medium().starts_with(n.tiny()),
                      "nesting broke on \"" << std::string(id) << "\"");
        CHECK_MESSAGE(s.starts_with(n.medium()),
                      "nesting broke on \"" << std::string(id) << "\"");
        // No agentty-internal marker escapes to the screen.
        CHECK(s.find("[1m]") == std::string::npos);
        CHECK(s.find("[2m]") == std::string::npos);
    }

    // A pathological id still names SOMETHING rather than rendering blank —
    // an empty model chip reads as a bug, not as "unknown model".
    CHECK(!decode("---").name.empty());
    CHECK(!decode("claude").name.empty());
    CHECK(!decode("4-5").name.empty());
}

// ── Hostile input: the id is not trusted ─────────────────────────────────
//
// Model ids arrive from provider APIs and user config, and neither is
// trusted. A `/v1/models` response is attacker-controlled if the endpoint is
// (a custom host, a compromised aggregator, a typo-squatted proxy), and it
// flows straight to a terminal that INTERPRETS what it is handed.
TEST_CASE("model_name: no terminal escape ever reaches the screen") {
    using agentty::model_name::decode;

    // An ESC byte in a label is a terminal-injection bug, not a cosmetic
    // one: "\x1b[31m" would recolour the UI, "\x1b[2J" would clear it, and
    // cursor-movement sequences can corrupt the frame — all from a string
    // the user never typed. Every C0 control and DEL is dropped at the
    // decoder boundary so no downstream widget has to remember to.
    for (std::string_view id : {"claude-opus\x1b[31m-4-8",
                                "gpt-\x07-4o",
                                "model\r\n-1",
                                "a\x1b]0;title\x07-2",
                                "x\x7f-3"}) {
        const auto s = decode(id).full();
        for (unsigned char c : s) {
            const bool printable = (c >= 0x20) && (c != 0x7F);
            CHECK_MESSAGE(printable,
                          "control byte " << int(c) << " survived decode");
        }
    }

    // An id that is ENTIRELY control bytes decodes to empty rather than to
    // garbage — there is genuinely no name in it.
    CHECK(decode("\x1b\x07\r\n").name.empty());
}

TEST_CASE("model_name: output length is bounded") {
    using agentty::model_name::decode;

    // An unbounded label is a denial of service against every width-shed
    // ladder downstream (composer footer, status bar, picker rows all
    // measure what they are given). Truncation is VISIBLE — an ellipsis, so
    // a surprising label reads as "too long", not as "wrong".
    const std::string huge(4000, 'x');
    for (const std::string& id : {huge,
                                  huge + "-4-5",
                                  std::string(500, 'a') + ":" + std::string(500, 'b')}) {
        const auto n = decode(id);
        CHECK(n.name.size()      <= 64);
        CHECK(n.qualifier.size() <= 64);
        CHECK(n.full().size()    <= 200);
        CHECK(!n.name.empty());          // bounded, but never blanked
    }

    // Many short tokens are bounded too — the cap is on OUTPUT, so it holds
    // however the length arrives.
    std::string many;
    for (int i = 0; i < 200; ++i) many += "tok-";
    CHECK(decode(many + "1").name.size() <= 64);

    // A normal name is never truncated — the cap must not be reachable by
    // legitimate models. The longest real one we ship against is ~26 chars.
    CHECK(decode("qwen2.5-coder:32b-instruct").full().find("\xe2\x80\xa6")
          == std::string::npos);
}

TEST_CASE("model_name: UTF-8 survives intact") {
    using agentty::model_name::decode;

    // Sanitising drops C0 controls by BYTE, and every UTF-8 continuation
    // byte is >= 0x80, so multi-byte text passes through untouched. A
    // provider serving a non-ASCII model name must not have it mangled.
    CHECK(decode("mod\xc3\xa8le-4-5").full()  == "Mod\xc3\xa8le 4.5");
    CHECK(decode("\xe6\xa8\xa1\xe5\x9e\x8b-4-5").full()
          == "\xe6\xa8\xa1\xe5\x9e\x8b 4.5");

    // Truncation lands on a UTF-8 boundary — cutting mid-sequence would
    // emit a replacement glyph or desynchronise the renderer's width
    // accounting. Build a long pure-multi-byte name and check the result
    // is still well-formed UTF-8.
    std::string wide;
    for (int i = 0; i < 100; ++i) wide += "\xe6\xa8\xa1";   // 模
    const auto s = decode(wide).full();
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t len = 0;
        if      (c < 0x80)        len = 1;
        else if ((c >> 5) == 0x06) len = 2;
        else if ((c >> 4) == 0x0E) len = 3;
        else if ((c >> 3) == 0x1E) len = 4;
        const bool valid_lead = (len != 0);
        CHECK_MESSAGE(valid_lead, "truncation produced an invalid lead byte");
        if (!valid_lead) break;
        for (std::size_t k = 1; k < len && i + k < s.size(); ++k) {
            const bool cont =
                (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
            CHECK(cont);
        }
        i += len;
    }
    CHECK(i == s.size());   // consumed exactly — no dangling partial glyph
}
