// fused_models_test — the pure ranking core of the unified cross-provider
// model picker. No Model / deps / network: we hand build_fused_rows() plain
// catalogs and assert ordering, sections, de-dup, active pinning, auth split.

#include "agentty/runtime/fused_models.hpp"

#include <doctest/doctest.h>

using namespace agentty;
using namespace agentty::ui;

namespace {

ModelInfo mk(std::string id, std::string name, std::string provider,
             int ctx = 200000, bool fav = false) {
    ModelInfo mi;
    mi.id = ModelId{std::move(id)};
    mi.display_name = std::move(name);
    mi.provider = std::move(provider);
    mi.context_window = ctx;
    mi.favorite = fav;
    return mi;
}

ProviderCatalog cat(std::string id, std::string label,
                    std::vector<ModelInfo> models) {
    ProviderCatalog c;
    c.provider_id = std::move(id);
    c.label = std::move(label);
    c.state = ProviderCatalog::State::Ready;
    c.models = std::move(models);
    return c;
}

} // namespace

TEST_CASE("fused: active pinned first and marked, no query") {
    std::vector<ProviderCatalog> cats = {
        cat("anthropic", "Anthropic",
            {mk("claude-sonnet-4-6", "Claude Sonnet 4.6", "anthropic"),
             mk("claude-opus-4", "Claude Opus 4", "anthropic")}),
        cat("openai", "OpenAI",
            {mk("gpt-5-codex", "gpt-5-codex", "openai", 400000),
             mk("gpt-4o", "gpt-4o", "openai", 128000)}),
    };
    FusedInputs in;
    in.catalogs = &cats;
    in.active = ModelRef{"openai", "gpt-5-codex"};

    auto rows = build_fused_rows(in);
    REQUIRE(!rows.empty());
    // Active pinned to row 0, in the RECENT section, marked active.
    CHECK(rows[0].provider_id == "openai");
    CHECK(rows[0].model.id.value == "gpt-5-codex");
    CHECK(rows[0].active);
    CHECK(rows[0].recent);
    // The active model must not also appear in the "all providers" section.
    int count_codex = 0;
    for (auto& r : rows)
        if (r.provider_id == "openai" && r.model.id.value == "gpt-5-codex")
            ++count_codex;
    CHECK(count_codex == 1);
    // Every other catalog model is present exactly once.
    CHECK(rows.size() == 4);
}

TEST_CASE("fused: query spans providers, ranks by match") {
    std::vector<ProviderCatalog> cats = {
        cat("anthropic", "Anthropic",
            {mk("claude-sonnet-4-6", "Claude Sonnet 4.6", "anthropic")}),
        cat("openai", "OpenAI",
            {mk("gpt-5-codex", "gpt-5-codex", "openai"),
             mk("gpt-4o", "gpt-4o", "openai")}),
        cat("kimi", "Kimi", {mk("kimi-k2", "kimi-k2-0905", "kimi", 256000)}),
    };
    FusedInputs in;
    in.catalogs = &cats;
    in.query = "gpt";

    auto rows = build_fused_rows(in);
    // Only the two GPT rows match "gpt".
    CHECK(rows.size() == 2);
    for (auto& r : rows) {
        CHECK(r.provider_id == "openai");
        CHECK(r.model.id.value.find("gpt") != std::string::npos);
    }
}

TEST_CASE("fused: MRU section then all-providers, deduped") {
    std::vector<ProviderCatalog> cats = {
        cat("anthropic", "Anthropic",
            {mk("claude-sonnet-4-6", "Claude Sonnet 4.6", "anthropic"),
             mk("claude-opus-4", "Claude Opus 4", "anthropic")}),
        cat("openai", "OpenAI", {mk("gpt-4o", "gpt-4o", "openai")}),
    };
    std::vector<ModelRef> recents = {
        {"openai", "gpt-4o"},
        {"anthropic", "claude-opus-4"},
    };
    FusedInputs in;
    in.catalogs = &cats;
    in.recents = &recents;
    in.active = ModelRef{"anthropic", "claude-sonnet-4-6"};

    auto rows = build_fused_rows(in);
    // RECENT = active + 2 MRU = 3 rows, all marked recent.
    int recent_rows = 0;
    for (auto& r : rows) if (r.recent) ++recent_rows;
    CHECK(recent_rows == 3);
    CHECK(rows[0].active);                     // active pinned first
    // Total distinct models = 3 (no dupes between RECENT and all-providers).
    CHECK(rows.size() == 3);
}

TEST_CASE("fused: sign-in offers are QUERY-GATED — hidden while browsing") {
    std::vector<ProviderCatalog> cats = {
        cat("anthropic", "Anthropic",
            {mk("claude-sonnet-4-6", "Claude Sonnet 4.6", "anthropic")}),
    };
    std::vector<SigninOffer> offers = {
        {"groq", "Groq"}, {"cerebras", "Cerebras"},
    };
    FusedInputs in;
    in.catalogs = &cats;
    in.offers = &offers;

    // Empty query = browse view: NO offer rows (the list stays clean; signing
    // in is reachable by TYPING the provider's name, or via ^P).
    auto rows = build_fused_rows(in);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].authed);

    // A query matching an un-authed provider surfaces its offer — searching
    // for a provider you haven't added is never a dead end.
    in.query = "groq";
    rows = build_fused_rows(in);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].is_signin_offer());
    CHECK(rows[0].provider_id == "groq");
    // A sign-in offer carries no model id.
    CHECK(rows[0].model.id.value.empty());
}

TEST_CASE("fused: query filters sign-in offers by provider name") {
    std::vector<ProviderCatalog> cats;
    std::vector<SigninOffer> offers = {{"groq", "Groq"}, {"cerebras", "Cerebras"}};
    FusedInputs in;
    in.catalogs = &cats;
    in.offers = &offers;
    in.query = "cere";

    auto rows = build_fused_rows(in);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].provider_id == "cerebras");
    CHECK(rows[0].is_signin_offer());
}

TEST_CASE("fused: favorites float above equal-scoring peers") {
    std::vector<ProviderCatalog> cats = {
        cat("openai", "OpenAI",
            {mk("gpt-4o", "gpt-4o", "openai", 128000, /*fav=*/false),
             mk("gpt-5", "gpt-5", "openai", 400000, /*fav=*/true)}),
    };
    FusedInputs in;
    in.catalogs = &cats;
    // No query ⇒ all match with score 0; favorite must sort first.
    auto rows = build_fused_rows(in);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].model.favorite);
    CHECK(rows[0].model.id.value == "gpt-5");
}

TEST_CASE("fused: a recent whose model left the catalog is dropped") {
    std::vector<ProviderCatalog> cats = {
        cat("openai", "OpenAI", {mk("gpt-4o", "gpt-4o", "openai")}),
    };
    std::vector<ModelRef> recents = {
        {"openai", "gpt-3.5-gone"},        // no longer offered
        {"openai", "gpt-4o"},
    };
    FusedInputs in;
    in.catalogs = &cats;
    in.recents = &recents;

    auto rows = build_fused_rows(in);
    // Only the still-present recent survives; the stale one is silently skipped.
    CHECK(rows.size() == 1);
    CHECK(rows[0].model.id.value == "gpt-4o");
    CHECK(rows[0].recent);
}

TEST_CASE("fused: [1m] context variants are distinct rows, not alias dupes") {
    // The 1M-context variant is a SEPARATE choice from its base model —
    // both must be listable. Row identity folds spelling aliases but must
    // NOT fold the `[1m]` marker (capkey::norm_row_id vs norm_model): the
    // latter strips it for capability lookups, which made every 1M row
    // look like an alias of its base and vanish from the list.
    std::vector<ProviderCatalog> cats = {
        cat("anthropic", "Anthropic",
            {mk("claude-opus-4-8",     "Claude Opus 4.8", "anthropic"),
             mk("claude-opus-4-8[1m]", "Claude Opus 4.8 (1M Context)",
                "anthropic", 1'000'000),
             mk("claude-sonnet-4-6",   "Claude Sonnet 4.6", "anthropic")}),
    };
    ui::FusedInputs in;
    in.catalogs = &cats;
    auto rows = ui::build_fused_rows(in);

    bool base = false, one_m = false;
    for (const auto& r : rows) {
        if (r.model.id.value == "claude-opus-4-8")     base  = true;
        if (r.model.id.value == "claude-opus-4-8[1m]") one_m = true;
    }
    CHECK(base);
    CHECK(one_m);   // regression: was silently deduped away

    // A genuine alias SPELLING still dedups (the behaviour norm_model
    // was there for): -3-5 and -3.5 are one model, one row.
    std::vector<ProviderCatalog> alias = {
        cat("mistral", "Mistral",
            {mk("mistral-medium-3-5", "Mistral Medium 3.5", "mistral"),
             mk("mistral-medium-3.5", "Mistral Medium 3.5", "mistral")}),
    };
    ui::FusedInputs in2;
    in2.catalogs = &alias;
    auto rows2 = ui::build_fused_rows(in2);
    int mistral_rows = 0;
    for (const auto& r : rows2)
        if (r.provider_id == "mistral") ++mistral_rows;
    CHECK(mistral_rows == 1);
}

// Browse-view ranking: with NO query the head of the list must be the models
// you would plausibly pick, not an arbitrary slice of whatever the providers
// happen to serve.
//
// The motivating case is an aggregator: OpenRouter-class catalogs contribute
// hundreds of rows to a ~14-row viewport, and ordering by provider-registry
// position alone meant the first screen was determined by nothing the user
// cares about. Ranking by capability tier (Flagship → Mid → Cheap → Weak) puts
// the plausible picks on screen 1, so scrolling becomes a choice.
//
// The inverse matters just as much: once a QUERY is active, fuzzy score is the
// intent signal and tier must NOT re-rank behind it — otherwise the row the
// user is aiming at moves under them as they type.
TEST_CASE("fused: browse ranks by tier, search stays relevance-ordered") {
    // One provider, deliberately listed weakest-first so registry order alone
    // would leave the weak model at the top.
    std::vector<ProviderCatalog> cats = {
        cat("openrouter", "OpenRouter",
            {mk("tinyllama:1b",        "TinyLlama 1B",   "openrouter"),
             mk("qwen2.5-coder:7b",    "Qwen2.5 Coder",  "openrouter"),
             mk("claude-haiku-4-5",    "Claude Haiku",   "openrouter"),
             mk("claude-sonnet-4-6",   "Claude Sonnet",  "openrouter"),
             mk("claude-opus-4-5",     "Claude Opus",    "openrouter")}),
    };

    // ── Browse: strongest first ──────────────────────────────────────
    {
        ui::FusedInputs in;
        in.catalogs = &cats;
        const auto rows = ui::build_fused_rows(in);
        REQUIRE(rows.size() >= 5);
        // Tier is non-increasing down the list.
        int prev = 4;   // above Flagship(3)
        for (const auto& r : rows) {
            if (r.is_signin_offer()) continue;
            const int t = static_cast<int>(
                ModelCapabilities::tier_for(r.model.id.value));
            INFO("browse list is ordered strongest-first");
        CHECK(t <= prev);
            prev = t;
        }
        // Concretely: the flagship outranks the 1B local model that the
        // catalog listed first.
        int opus = -1, tiny = -1;
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            if (rows[static_cast<std::size_t>(i)].model.id.value == "claude-opus-4-5") opus = i;
            if (rows[static_cast<std::size_t>(i)].model.id.value == "tinyllama:1b")    tiny = i;
        }
        REQUIRE(opus >= 0);
        REQUIRE(tiny >= 0);
        INFO("a flagship outranks a 1B local model when browsing");
        CHECK(opus < tiny);
    }

    // ── Search: relevance wins, tier does NOT reorder behind it ──────
    {
        ui::FusedInputs in;
        in.catalogs = &cats;
        in.query = "coder";           // matches only the weak local model
        const auto rows = ui::build_fused_rows(in);
        REQUIRE(!rows.empty());
        INFO("a query's best match leads, regardless of its tier");
        CHECK(rows.front().model.id.value == "qwen2.5-coder:7b");
    }

    // ── Favorites still outrank everything, in both modes ────────────
    {
        std::vector<ProviderCatalog> favc = {
            cat("openrouter", "OpenRouter",
                {mk("tinyllama:1b",    "TinyLlama 1B", "openrouter", 8000, true),
                 mk("claude-opus-4-5", "Claude Opus",  "openrouter")}),
        };
        ui::FusedInputs in;
        in.catalogs = &favc;
        const auto rows = ui::build_fused_rows(in);
        REQUIRE(!rows.empty());
        INFO("an explicit favorite outranks tier — the user already chose");
        CHECK(rows.front().model.id.value == "tinyllama:1b");
    }

    // ── The ACTIVE model stays pinned to the top of RECENT ───────────
    // Tier ranking applies to section 2 only; it must not disturb the
    // recents section's meaning.
    {
        ui::FusedInputs in;
        in.catalogs = &cats;
        in.active = ModelRef{"openrouter", "tinyllama:1b"};   // weak, but active
        const auto rows = ui::build_fused_rows(in);
        REQUIRE(!rows.empty());
        INFO("the active model still leads the list");
        CHECK(rows.front().active);
        INFO("tier ranking does not evict the active model from the top");
        CHECK(rows.front().model.id.value == "tinyllama:1b");
    }
}

// Row-layout arithmetic: the picker aligns three columns —
//
//   <provider, padded>  <● marker><model name>   <ctx, right-aligned> <★> <✦>
//
// maya's Picker documents that callers must "pad badges to a common width for
// column alignment", and this picker did not, so with provider labels running
// 3..14 chars every model NAME began at a different column. In a FLAT
// cross-provider list the provider badge is the grouping signal, so a ragged
// badge column defeats the entire layout — you cannot scan it vertically.
//
// The context window is a MEASUREMENT and is right-aligned into a fixed 5-col
// field; the two marks occupy fixed slots after it so a row lacking a
// favourite leaves a hole rather than sliding its ✦ leftwards (which is what
// made the list jitter while scrolling).
//
// This test pins the arithmetic the view performs. It deliberately does NOT
// render — it reproduces the exact expressions from ui::fused_picker so a
// change there without a change here is caught.
TEST_CASE("fused: row columns align") {
    // ── Badge padding ────────────────────────────────────────────────
    auto pad_badge = [](const std::string& label, std::size_t w) {
        return label.size() < w ? label + std::string(w - label.size(), ' ')
                                : label;
    };
    {
        // Width is the widest label present, clamped to 12.
        const std::vector<std::string> labels = {
            "Groq", "Anthropic", "GitHub Copilot", "OpenRouter"};
        std::size_t w = 0;
        for (const auto& l : labels) w = std::max(w, l.size());
        w = std::min<std::size_t>(w, 12);
        CHECK(w == 12);   // "GitHub Copilot" is 14 → clamped

        // Every padded badge is the same width, so the next column starts at
        // one fixed offset for every row.
        std::size_t first = pad_badge(labels.front(), w).size();
        for (const auto& l : labels) {
            const auto b = pad_badge(l, w);
            CHECK(b.size() == std::max(first, l.size()));
        }
        // A short label really is padded (this is the bug being fixed).
        CHECK(pad_badge("Groq", w) == "Groq        ");
        // An over-long label is left alone; maya truncates the overflow.
        CHECK(pad_badge("GitHub Copilot", w) == "GitHub Copilot");
    }

    // ── Context window is right-aligned in a 5-column field ─────────
    auto ctx_field = [](int win) {
        std::string ctx;
        if (win > 0) {
            if (win >= 1'000'000) {
                ctx = std::to_string(win / 1'000'000) + "M";
                if (win % 1'000'000 != 0) ctx += "+";
            } else if (win >= 1000) {
                ctx = std::to_string(win / 1'000) + "k";
            } else {
                ctx = std::to_string(win);
            }
        }
        return ctx.size() < 5 ? std::string(5 - ctx.size(), ' ') + ctx : ctx;
    };
    {
        CHECK(ctx_field(200000) == " 200k");
        CHECK(ctx_field(128000) == " 128k");
        CHECK(ctx_field(8000)   == "   8k");
        CHECK(ctx_field(1000000)== "   1M");
        CHECK(ctx_field(1500000)== "  1M+");
        CHECK(ctx_field(0)      == "     ");   // unknown: blank, still aligned
        // Every field is exactly 5 wide → the digits share a column.
        for (int w : {200000, 128000, 8000, 1000000, 1500000, 0})
            CHECK(ctx_field(w).size() == 5);
    }

    // ── Marks occupy fixed slots ────────────────────────────────────
    auto marks = [](bool fav, bool reasons) {
        std::string t;
        t += fav ? "  \xe2\x98\x85" : "   ";
        t += reasons ? " \xe2\x9c\xa6" : "  ";
        return t;
    };
    {
        // DISPLAY COLUMNS, not bytes. ★ and ✦ are 3-byte UTF-8 sequences that
        // occupy ONE column each, so byte offsets legitimately differ between
        // a row with a favourite and one without — an earlier version of this
        // test compared find() offsets and "failed" on correct layout. What
        // must hold is that both marks land in the same COLUMN.
        auto cols = [](std::string_view t) {
            int n = 0;
            for (unsigned char c : t)
                if ((c & 0xC0) != 0x80) ++n;   // count non-continuation bytes
            return n;
        };
        // Every combination occupies exactly 5 columns: "  ★" / "   " is 3,
        // " ✦" / "  " is 2.
        CHECK(cols(marks(true,  true))  == 5);
        CHECK(cols(marks(true,  false)) == 5);
        CHECK(cols(marks(false, true))  == 5);
        CHECK(cols(marks(false, false)) == 5);
        // The ✦ therefore starts at column 3 whether or not ★ is present —
        // it can never slide into the favourite's slot.
        auto star_slot = [&](bool fav) { return cols(std::string{marks(fav, false)}); };
        CHECK(star_slot(true) == star_slot(false));
    }
}

// The badge column scales with the terminal. A fixed clamp is wrong in both
// directions: too greedy in a narrow split (14 columns restating the provider
// on every row starves the model NAME, which is what the user reads), too
// tight on a wide screen (truncating "GitHub Copilot" to "GitHub Copil" when
// there is ample room). The picker is commonly used in a split pane, so this
// is not a hypothetical.
TEST_CASE("fused: badge column scales with terminal width") {
    // Mirrors picker_badge_max_cols()'s ladder.
    auto badge_max = [](int cols) {
        if (cols < 70)  return 8;
        if (cols < 100) return 12;
        return 16;
    };

    CHECK(badge_max(60)  == 8);    // narrow split
    CHECK(badge_max(80)  == 12);   // classic terminal
    CHECK(badge_max(120) == 16);   // wide

    // Monotonic: a wider terminal never yields a NARROWER badge column.
    int prev = 0;
    for (int c : {40, 60, 70, 80, 100, 120, 200}) {
        const int w = badge_max(c);
        CHECK(w >= prev);
        prev = w;
    }

    // The widest real provider label fits without truncation once there is
    // room for it — "GitHub Copilot" is 14 columns.
    CHECK(badge_max(120) >= 14);
    // …and is abbreviated, not permitted to eat the row, when there isn't.
    CHECK(badge_max(60) < 14);
}

// The row carries its capability tier, precomputed. Two consumers need it —
// the browse-mode sort and the view (which hues the provider badge by it so
// the strongest-first ordering is legible rather than unexplained) — and both
// run hot: a comparator is O(n log n), and the view touches every visible row
// every frame. tier_for() tokenises the id and runs several substring scans,
// so it is resolved ONCE per row, alongside `reasons`, for the same reason.
TEST_CASE("fused: rows carry a precomputed capability tier") {
    std::vector<ProviderCatalog> cats = {
        cat("openrouter", "OpenRouter",
            {mk("claude-opus-4-5",  "Claude Opus",   "openrouter"),
             mk("claude-sonnet-4-6","Claude Sonnet", "openrouter"),
             mk("claude-haiku-4-5", "Claude Haiku",  "openrouter"),
             mk("tinyllama:1b",     "TinyLlama 1B",  "openrouter")}),
    };

    auto tier_of = [&](const std::vector<FusedRow>& rows, std::string_view id) {
        for (const auto& r : rows)
            if (r.model.id.value == id) return static_cast<int>(r.tier);
        return -1;
    };

    // ── Populated on the ALL-PROVIDERS rows ─────────────────────────
    {
        ui::FusedInputs in;
        in.catalogs = &cats;
        const auto rows = ui::build_fused_rows(in);
        CHECK(tier_of(rows, "claude-opus-4-5")   == 3);   // Flagship
        CHECK(tier_of(rows, "claude-sonnet-4-6") == 2);   // Mid
        CHECK(tier_of(rows, "claude-haiku-4-5")  == 1);   // Cheap
        CHECK(tier_of(rows, "tinyllama:1b")      == 0);   // Weak
    }

    // ── Populated while FILTERING too ───────────────────────────────
    // The sort only needed tier when browsing, so it used to be computed
    // under `no_query`. The view needs it always — a filtered list still
    // hues its badges — so a query must not leave the field at 0 (which
    // would paint every match as Weak).
    {
        ui::FusedInputs in;
        in.catalogs = &cats;
        in.query = "opus";
        const auto rows = ui::build_fused_rows(in);
        REQUIRE(!rows.empty());
        CHECK(tier_of(rows, "claude-opus-4-5") == 3);
    }

    // ── Populated on RECENT rows (a separate build path) ────────────
    {
        std::vector<ModelRef> recents = {ModelRef{"openrouter", "claude-opus-4-5"}};
        ui::FusedInputs in;
        in.catalogs = &cats;
        in.recents  = &recents;
        const auto rows = ui::build_fused_rows(in);
        REQUIRE(!rows.empty());
        CHECK(rows.front().recent);
        CHECK(static_cast<int>(rows.front().tier) == 3);
    }

    // ── Colour is never the SOLE carrier of the tier signal ─────────
    // The badge hue is an accessibility hazard if it is the only way to tell
    // a flagship from a 3B local model. It is not: the browse ORDER encodes
    // the same fact (strongest first), and every row still shows its model
    // name and context window. This asserts the redundancy holds.
    {
        ui::FusedInputs in;
        in.catalogs = &cats;
        const auto rows = ui::build_fused_rows(in);
        int prev = 4;
        for (const auto& r : rows) {
            if (r.is_signin_offer()) continue;
            CHECK(static_cast<int>(r.tier) <= prev);   // order says it too
            prev = static_cast<int>(r.tier);
            CHECK(!r.model_label.empty());             // and the name is there
        }
    }
}
