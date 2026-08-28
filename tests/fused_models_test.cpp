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

TEST_CASE("fused: un-authed providers become sign-in offers at the end") {
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

    auto rows = build_fused_rows(in);
    REQUIRE(rows.size() == 3);
    // Authed model rows first, offers last.
    CHECK(rows[0].authed);
    CHECK(rows[1].is_signin_offer());
    CHECK(rows[2].is_signin_offer());
    CHECK(rows[1].provider_id == "groq");
    // A sign-in offer carries no model id.
    CHECK(rows[1].model.id.value.empty());
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
