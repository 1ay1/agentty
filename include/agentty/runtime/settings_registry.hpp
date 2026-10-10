#pragma once
// agentty::settings::registry — every user-facing setting, as one table.
//
// THE PROBLEM. agentty grew ~35 genuine policy knobs that existed only as
// environment variables. Each one was declared in four places — a field on the
// config struct, a line in `from_env()`, a line in the settings load, a line in
// the settings save — and surfaced in none. They were undiscoverable (you had
// to read the source to learn they existed), session-only (export it or lose
// it), and unvalidated (`env_float("...", 0.65f)` happily accepts 900).
//
// THE FIX. A setting is a ROW. The table below is the single source of truth,
// and everything else is DERIVED from it by walking:
//
//   * env reading      — read `.env`, clamp to `.range`
//   * settings.json    — load/save keyed on `.id`
//   * the settings UI  — SettingDef -> form::Field
//   * validation       — `.range` enforced in exactly one place
//   * `agentty config` — list/get/set, free
//
// Adding a knob is adding a row, not four edits across three files. Same
// promise as `kProviders`: "adding a provider is a table row".
//
// ── The binding is part of the row ───────────────────────────────────────
// `slot` is a pointer-to-member into store::RagConfig, so the row knows how to
// READ and WRITE its own value. Without it, every walker would need its own
// switch on `id` — which is precisely the duplication this table exists to
// remove. Pointer-to-member is constexpr-constructible, so the whole table
// stays a compile-time constant.
//
// ── Tier is an editorial judgement, and it matters ───────────────────────
// Not everything that CAN be exposed SHOULD be. `autocut_sensitivity` is an
// engine-tuning constant a user has no way to evaluate; making it prominent
// would be making a confusing thing discoverable rather than useful. Advanced
// rows are hidden behind a keypress. The bar for Basic: would a reasonable
// user reasonably choose either way, and can they tell which they prefer?

#include <array>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <variant>

#include "agentty/domain/web_search_config.hpp"  // web_search.* rows: bounds + struct
#include "agentty/domain/smart_tuning.hpp"   // shipped defaults + ranges
#include "agentty/runtime/panel/form.hpp"           // Builder (add_rows)
#include "agentty/store/store.hpp"

namespace agentty::settings::registry {

// Where a row appears in the settings UI.
enum class Group : std::uint8_t {
    Sources,      // which corpora are indexed
    Pipeline,     // retrieval stages
    Fusion,       // how lexical + dense scores combine
    Proactive,    // pre-turn injection
    Infra,        // persistence, tracing, feedback
    Routing,      // Smart Mode's numeric policy
    WebSearch,    // web_search: availability, result count, exclusions
    WebSearchServices, // web_search: which services answer, in what order
};

[[nodiscard]] constexpr std::string_view label_of(Group g) noexcept {
    switch (g) {
        case Group::Sources:   return "Sources";
        case Group::Pipeline:  return "Pipeline";
        case Group::Fusion:    return "Fusion";
        case Group::Proactive: return "Proactive";
        case Group::Infra:     return "Infrastructure";
        case Group::Routing:   return "Routing";
        case Group::WebSearch: return "Search";
        case Group::WebSearchServices: return "Services";
    }
    return "";
}

// Basic rows are always shown; Advanced hide behind a keypress. See the
// header comment — this is a judgement about whether a user can act on the
// knob, not about how obscure it is.
enum class Tier : std::uint8_t { Basic, Advanced };

// What kind of control the row is. Deliberately NOT a variant of values: the
// value lives in the config struct and is reached through `slot`, so a row
// carries only its TYPE and its bounds.
enum class Type : std::uint8_t { Bool, Real, Int, Enum, Text };

// WHICH config struct a row binds to.
//
// The table started life RAG-only and every accessor took a RagConfig. Smart
// Mode's numeric policy has the same problem RAG's did — knobs that existed
// only as environment variables, so undiscoverable, session-only and
// unvalidated — and deserves the same solution.
//
// Both owners are DOMAIN types that store::Settings composes, so a row binds
// to the thing the runtime actually reads. That is what makes a UI edit take
// effect: the pane writes the same struct the router holds, with no shape to
// convert between and no fan-out step to forget.
//
// Search is the third, for the same reason: web_search's policy is a domain
// type (web_search_cfg::Config) the tool layer reads as-is. Its rows are the only
// ones the Web Search pane renders, and the only ones of Type::Text — an
// exclusion list is free text, not a number or an enum.
enum class Owner : std::uint8_t { Rag, Smart, WebSearch };

// Pointer-to-member into the persisted config. The row's read/write binding.
// One alternative per (type, owner) pair; `Owner` says which half is live.
//
// The member-pointer TYPE is always spelled through the alias below, never
// inline: MSVC cannot parse a namespace-qualified pointer-to-member written
// directly as a template argument — `std::variant<bool store::RagConfig::*>`
// dies with C3083/C2059 at the `*` (it only knows `<tag>::*` in declarator
// context, which is why the `&store::RagConfig::field` initializers further
// down compile everywhere). Routing every use through a named alias turns
// each into a plain type argument: identical type, parseable by all three
// compilers.
template <class C, class M>
using MemberPtr = M C::*;

using Slot = std::variant<
    MemberPtr<store::RagConfig, bool>,
    MemberPtr<store::RagConfig, int>,
    MemberPtr<store::RagConfig, float>,
    MemberPtr<store::RagConfig, double>,
    MemberPtr<store::RagConfig, std::string>,
    MemberPtr<smart::RoleConfig, bool>,
    MemberPtr<smart::RoleConfig, int>,
    MemberPtr<smart::RoleConfig, float>,
    MemberPtr<smart::RoleConfig, double>,
    MemberPtr<smart::RoleConfig, std::string>,
    MemberPtr<web_search_cfg::Config, bool>,
    MemberPtr<web_search_cfg::Config, int>,
    MemberPtr<web_search_cfg::Config, std::string>>;

// The struct a member pointer points into: `bool web_search_cfg::Config::*` →
// web_search_cfg::Config. owner() is derived from this.
template <class M> struct member_class;
template <class C, class T> struct member_class<T C::*> { using type = C; };
template <class M>
using member_class_t = typename member_class<M>::type;

struct SettingDef {
    std::string_view id;      // settings.json key AND form field id ("rag.mmr")
    std::string_view env;     // legacy env name; "" = no env alias
    std::string_view label;   // UI
    std::string_view help;    // one-line description
    Group            group;
    Tier             tier;
    Type             type;
    Slot             slot;

    // Bounds for Real/Int. Ignored otherwise. ENFORCED — a value outside the
    // range is clamped at every entry point (env, json, UI), so the config
    // can never hold one.
    double           min = 0.0;
    double           max = 1.0;
    double           step = 0.05;   // Real: slider granularity

    // Enum options, pipe-separated ("convex|rrf"). Parsed once into the
    // form's Choice row; the stored value is the option string itself.
    std::string_view options;

    // Which config struct `slot` points into. DERIVED from the slot rather
    // than stored alongside it: a row that declared its owner separately
    // could declare it wrongly, and the walkers would then skip a row that
    // was reachable or visit one that was not.
    //
    // Keyed on the member pointer's CLASS, not on each (class, type) pair:
    // with three owners the old "Rag if one of these five, else Smart" chain
    // would have filed every search row under Smart, silently.
    [[nodiscard]] constexpr Owner owner() const noexcept {
        return std::visit([]<class M>(M) constexpr {
            using C = member_class_t<M>;
            if constexpr (std::is_same_v<C, store::RagConfig>)
                return Owner::Rag;
            else if constexpr (std::is_same_v<C, smart::RoleConfig>)
                return Owner::Smart;
            else {
                static_assert(std::is_same_v<C, web_search_cfg::Config>,
                              "a Slot alternative names a config struct "
                              "owner() does not know");
                return Owner::WebSearch;
            }
        }, slot);
    }
};

// ── The table ────────────────────────────────────────────────────────────
// Ordered by Group, then by how likely a user is to want it. Reading this
// top-to-bottom should read like the settings screen, because it IS the
// settings screen.
// The size is DEDUCED, not declared. A hand-maintained count is one more
// thing to keep in step with the rows themselves — exactly the duplication
// this table exists to remove — and getting it wrong is a wall of
// "too many initializers" rather than a useful message.
inline constexpr std::array kSettings = std::to_array<SettingDef>({
    // ── Sources ─────────────────────────────────────────────────────────
    {"rag.skills", "AGENTTY_RAG_SKILLS", "Index skills",
     "let retrieval search your installed skills",
     Group::Sources, Tier::Basic, Type::Bool, &store::RagConfig::skills},
    {"rag.memory", "AGENTTY_RAG_MEMORY", "Index memory",
     "let retrieval search remembered facts",
     Group::Sources, Tier::Basic, Type::Bool, &store::RagConfig::memory},
    {"rag.mcp_resources", "AGENTTY_RAG_MCP", "Index MCP resources",
     "also index resources exposed by MCP servers",
     Group::Sources, Tier::Basic, Type::Bool, &store::RagConfig::mcp_resources},

    // ── Pipeline ────────────────────────────────────────────────────────
    {"rag.contextual", "AGENTTY_RAG_CONTEXTUAL", "Contextual chunks",
     "give each chunk a short situating preamble at index time",
     Group::Pipeline, Tier::Basic, Type::Bool, &store::RagConfig::contextual},
    {"rag.mmr", "AGENTTY_RAG_MMR", "Diversity (MMR)",
     "spread results across the corpus instead of one dense cluster",
     Group::Pipeline, Tier::Basic, Type::Bool, &store::RagConfig::mmr},
    {"rag.mmr_lambda", "AGENTTY_RAG_MMR_LAMBDA", "Diversity balance",
     "1.0 = pure relevance, 0.0 = pure diversity",
     Group::Pipeline, Tier::Advanced, Type::Real, &store::RagConfig::mmr_lambda,
     0.0, 1.0, 0.05},
    {"rag.dedup", "AGENTTY_RAG_DEDUP", "Fold duplicates",
     "collapse near-identical passages so the window isn't spent twice",
     Group::Pipeline, Tier::Basic, Type::Bool, &store::RagConfig::dedup},
    {"rag.dedup_threshold", "AGENTTY_RAG_DEDUP_THRESHOLD", "Duplicate threshold",
     "similarity above which two passages count as the same",
     Group::Pipeline, Tier::Advanced, Type::Real, &store::RagConfig::dedup_threshold,
     0.5, 1.0, 0.01},
    {"rag.autocut", "AGENTTY_RAG_AUTOCUT", "Trim weak tail",
     "drop the low-relevance tail at the score knee",
     Group::Pipeline, Tier::Basic, Type::Bool, &store::RagConfig::autocut},
    {"rag.autocut_sensitivity", "AGENTTY_RAG_AUTOCUT_SENS", "Trim aggressiveness",
     "lower cuts more; higher keeps more of the tail",
     Group::Pipeline, Tier::Advanced, Type::Real, &store::RagConfig::autocut_sensitivity,
     0.5, 5.0, 0.1},
    {"rag.stitch", "AGENTTY_RAG_STITCH", "Stitch neighbours",
     "join adjacent chunks so a passage isn't cut mid-thought",
     Group::Pipeline, Tier::Basic, Type::Bool, &store::RagConfig::stitch},

    // Power modes: each costs latency or model round-trips, so each is off by
    // default and says what it costs.
    {"rag.expand", "AGENTTY_RAG_EXPAND", "Multi-query",
     "ask the model for query variants — better recall, one extra round trip",
     Group::Pipeline, Tier::Basic, Type::Bool, &store::RagConfig::expand},
    {"rag.hyde", "AGENTTY_RAG_HYDE", "HyDE",
     "search with a hypothetical answer — helps vague questions, costs a round trip",
     Group::Pipeline, Tier::Basic, Type::Bool, &store::RagConfig::hyde},
    {"rag.prf", "AGENTTY_RAG_PRF", "Feedback expansion",
     "re-query using the top hits' terms; can drift off-topic",
     Group::Pipeline, Tier::Advanced, Type::Bool, &store::RagConfig::prf},
    {"rag.corrective", "AGENTTY_RAG_CORRECT", "Corrective grading",
     "grade and re-retrieve weak results; lexical proxy can reject good matches",
     Group::Pipeline, Tier::Advanced, Type::Bool, &store::RagConfig::corrective},
    {"rag.graph", "AGENTTY_RAG_GRAPH", "Graph expansion",
     "follow entity links between chunks; quadratic build on large corpora",
     Group::Pipeline, Tier::Advanced, Type::Bool, &store::RagConfig::graph},

    // ── Fusion ──────────────────────────────────────────────────────────
    {"rag.fusion", "AGENTTY_RAG_FUSION", "Fusion",
     "how keyword and semantic scores combine",
     Group::Fusion, Tier::Basic, Type::Enum, &store::RagConfig::fusion,
     0.0, 0.0, 0.0, "convex|rrf"},
    {"rag.adaptive_fusion", "AGENTTY_RAG_ADAPTIVE", "Adaptive weighting",
     "lean on whichever retriever is more confident per query (convex only)",
     Group::Fusion, Tier::Basic, Type::Bool, &store::RagConfig::adaptive_fusion},
    {"rag.dense_weight", "AGENTTY_RAG_DENSE_WEIGHT", "Semantic weight",
     "weight of embedding search in RRF mode",
     Group::Fusion, Tier::Advanced, Type::Real, &store::RagConfig::dense_weight,
     0.0, 3.0, 0.1},
    {"rag.bm25_weight", "AGENTTY_RAG_BM25_WEIGHT", "Keyword weight",
     "weight of keyword search in RRF mode",
     Group::Fusion, Tier::Advanced, Type::Real, &store::RagConfig::bm25_weight,
     0.0, 3.0, 0.1},

    // ── Infrastructure ──────────────────────────────────────────────────
    {"rag.persist", "AGENTTY_RAG_PERSIST", "Cache index",
     "keep the built index under .agentty/ so restarts are warm",
     Group::Infra, Tier::Basic, Type::Bool, &store::RagConfig::persist},
    {"rag.trace", "AGENTTY_RAG_TRACE", "Trace stages",
     "fold per-stage timings into the retrieval label",
     Group::Infra, Tier::Advanced, Type::Bool, &store::RagConfig::trace},

    // ── Routing (Smart Mode) ─────────────────────────────────────
    // Smart Mode's FEATURE surface (the master switch and the three model
    // slots) is the Ctrl+S overlay — those are choices about WHICH MODEL, and
    // belong next to a model picker. These three are its numeric POLICY, and
    // they are here for the same reason the RAG tuning rows are: they existed
    // only as environment variables, which made them undiscoverable,
    // session-only and unvalidated.
    //
    // All Advanced. "At what feature score does a turn become Complex" is not
    // a question a user can answer cold — but it IS the spend dial, so it has
    // to be reachable rather than a source-code constant.
    {"smart.complex_threshold", "AGENTTY_SMART_COMPLEX_THRESHOLD",
     "Complexity cut",
     "feature score at which a turn routes as Complex; lower spends more",
     Group::Routing, Tier::Advanced, Type::Int,
     &smart::RoleConfig::complex_threshold,
     smart::tuning::kComplexMin, smart::tuning::kComplexMax, 1},
    {"smart.deep_margin", "AGENTTY_SMART_DEEP_MARGIN", "Deep-band margin",
     "how far into a tier a turn must sit to earn the extra effort step",
     Group::Routing, Tier::Advanced, Type::Int,
     &smart::RoleConfig::deep_margin,
     smart::tuning::kDeepMarginMin, smart::tuning::kDeepMarginMax, 1},
    {"smart.bias_clamp", "AGENTTY_SMART_BIAS_CLAMP", "Self-correction cap",
     "how far this session's own corrections may drift effort from baseline",
     Group::Routing, Tier::Advanced, Type::Int,
     &smart::RoleConfig::bias_clamp,
     smart::tuning::kBiasClampMin, smart::tuning::kBiasClampMax, 1},

    // The two rows that decide whether Smart Mode saves you money at all.
    //
    // Basic tier, unlike the three above: "should a one-word turn run on my
    // most expensive model" IS a question a user can answer cold, and it is
    // the difference between Smart Mode working and Smart Mode being
    // decoration. Before these existed the main turn was pinned to Strategic
    // in the dispatch path with no way to change it — a user's 577-turn
    // trace had 142 trivial/simple turns all billed at flagship rate.
    {"smart.route_main_turn", "AGENTTY_SMART_ROUTE_MAIN", "Route the main turn",
     "let a turn's complexity choose its model, not just its effort",
     Group::Routing, Tier::Basic, Type::Bool,
     &smart::RoleConfig::route_main_turn},
    {"smart.main_turn_floor", "AGENTTY_SMART_MAIN_FLOOR", "Cheapest main-turn role",
     "how far down a simple turn may be routed; strategic = never route down",
     Group::Routing, Tier::Basic, Type::Enum,
     &smart::RoleConfig::main_turn_floor,
     0.0, 0.0, 0.0, "utility|implementation|strategic"},

    // ── web_search (Settings → Web Search) ─────────────────────────────
    // Applied at the two seams agentty owns — wire advertisement and the
    // dispatch closure — never inside mcp-cpp, which implements the tool.
    // See domain/web_search_config.hpp for what that does and does not reach.
    {"web_search.mode", "AGENTTY_WEB_SEARCH", "Web search",
     "auto: the model decides the count, as before \xc2\xb7 on: your counts apply \xc2\xb7 off: no web search",
     Group::WebSearch, Tier::Basic, Type::Enum, &web_search_cfg::Config::mode_text,
     0.0, 0.0, 0.0, "auto|on|off"},
    {"web_search.count", "AGENTTY_WEB_SEARCH_COUNT", "Results per search",
     "how many results a search returns when the model does not say",
     Group::WebSearch, Tier::Basic, Type::Int, &web_search_cfg::Config::count,
     static_cast<double>(web_search_cfg::kCountMin),
     static_cast<double>(web_search_cfg::kCountMax), 1.0},
    {"web_search.max_count", "AGENTTY_WEB_SEARCH_MAX", "Most results allowed",
     "ceiling on what the model may ask for in one search",
     Group::WebSearch, Tier::Basic, Type::Int, &web_search_cfg::Config::max_count,
     static_cast<double>(web_search_cfg::kMaxCountMin),
     static_cast<double>(web_search_cfg::kMaxCountMax), 1.0},
    {"web_search.exclude_sites", "AGENTTY_WEB_SEARCH_EXCLUDE", "Never return",
     "domains to leave out, separated by spaces or commas: pinterest.com w3schools.com",
     Group::WebSearch, Tier::Basic, Type::Text, &web_search_cfg::Config::exclude_sites},

    // Which services answer, first to succeed wins. The options are the ids of
    // mcp-cpp's catalogue (mcp/tools/web_search.hpp); the pane shows its labels.
    // tests/web_search_services_test.cpp pins this list to the catalogue.
    {"web_search.primary", "AGENTTY_WEB_SEARCH_PRIMARY", "Primary",
     "asked first. Free needs no account; API key needs one, stored encrypted",
     Group::WebSearchServices, Tier::Basic, Type::Enum, &web_search_cfg::Config::primary,
     0.0, 0.0, 0.0, web_search_cfg::kServiceIds},
    {"web_search.secondary", "AGENTTY_WEB_SEARCH_SECONDARY", "Secondary",
     "asked when the primary fails or is blocked",
     Group::WebSearchServices, Tier::Basic, Type::Enum, &web_search_cfg::Config::secondary,
     0.0, 0.0, 0.0, web_search_cfg::kOptionalServiceIds},
    {"web_search.fallback", "AGENTTY_WEB_SEARCH_FALLBACK", "Fallback",
     "asked last, when both of the above fail",
     Group::WebSearchServices, Tier::Basic, Type::Enum, &web_search_cfg::Config::fallback,
     0.0, 0.0, 0.0, web_search_cfg::kOptionalServiceIds},
    {"web_search.language", "AGENTTY_WEB_SEARCH_LANGUAGE", "Language",
     "result language for the services that take one; auto leaves it to them",
     Group::WebSearchServices, Tier::Basic, Type::Enum, &web_search_cfg::Config::language,
     0.0, 0.0, 0.0, web_search_cfg::kLanguages},
});

inline constexpr int kCount = static_cast<int>(kSettings.size());

// ── Lookup ───────────────────────────────────────────────────────────────
[[nodiscard]] constexpr const SettingDef* find(std::string_view id) noexcept {
    for (const auto& s : kSettings)
        if (s.id == id) return &s;
    return nullptr;
}

// ── Compile-time proofs ──────────────────────────────────────────────────
// A reader should not have to trust that the table is well-formed; the
// invariants are checked where they are written.
namespace proofs {

consteval bool ids_unique() {
    for (std::size_t i = 0; i < kSettings.size(); ++i)
        for (std::size_t j = i + 1; j < kSettings.size(); ++j)
            if (kSettings[i].id == kSettings[j].id) return false;
    return true;
}

consteval bool envs_unique() {
    for (std::size_t i = 0; i < kSettings.size(); ++i) {
        if (kSettings[i].env.empty()) continue;
        for (std::size_t j = i + 1; j < kSettings.size(); ++j)
            if (kSettings[i].env == kSettings[j].env) return false;
    }
    return true;
}

consteval bool ranges_sane() {
    for (const auto& s : kSettings) {
        if (s.type != Type::Real && s.type != Type::Int) continue;
        if (!(s.min < s.max)) return false;
        if (s.step <= 0.0) return false;
    }
    return true;
}

// An Enum row without options is a dropdown with nothing in it.
consteval bool enums_have_options() {
    for (const auto& s : kSettings)
        if (s.type == Type::Enum && s.options.empty()) return false;
    return true;
}

// The slot's type must match the declared Type, or a walker will write
// through the wrong member and silently corrupt an unrelated setting. Checked
// across BOTH owners, so a Settings-owned row is held to the same standard as
// a RagConfig-owned one.
consteval bool slots_match_types() {
    for (const auto& s : kSettings) {
        switch (s.type) {
            case Type::Bool:
                if (!std::holds_alternative<MemberPtr<store::RagConfig, bool>>(s.slot)
                 && !std::holds_alternative<MemberPtr<smart::RoleConfig, bool>>(s.slot)
                 && !std::holds_alternative<MemberPtr<web_search_cfg::Config, bool>>(s.slot))
                    return false;
                break;
            case Type::Real:
                if (!std::holds_alternative<MemberPtr<store::RagConfig, float>>(s.slot)
                 && !std::holds_alternative<MemberPtr<store::RagConfig, double>>(s.slot)
                 && !std::holds_alternative<MemberPtr<smart::RoleConfig, float>>(s.slot)
                 && !std::holds_alternative<MemberPtr<smart::RoleConfig, double>>(s.slot))
                    return false;
                break;
            case Type::Int:
                if (!std::holds_alternative<MemberPtr<store::RagConfig, int>>(s.slot)
                 && !std::holds_alternative<MemberPtr<smart::RoleConfig, int>>(s.slot)
                 && !std::holds_alternative<MemberPtr<web_search_cfg::Config, int>>(s.slot))
                    return false;
                break;
            case Type::Enum:
                if (!std::holds_alternative<MemberPtr<store::RagConfig, std::string>>(s.slot)
                 && !std::holds_alternative<MemberPtr<smart::RoleConfig, std::string>>(s.slot)
                 && !std::holds_alternative<MemberPtr<web_search_cfg::Config, std::string>>(s.slot))
                    return false;
                break;
            // Free text binds a string like Enum does, but carries no options
            // to validate against — so it is kept to the one owner that needs
            // it rather than offered to every string slot.
            case Type::Text:
                if (!std::holds_alternative<MemberPtr<web_search_cfg::Config, std::string>>(s.slot))
                    return false;
                if (!s.options.empty()) return false;
                break;
        }
    }
    return true;
}

// Every id is namespaced, so a settings.json key cannot collide with an
// unrelated top-level field — and the PREFIX must agree with the owner the
// slot points into. That second half is what keeps "which struct holds this"
// answerable by reading the id: a row called "rag.…" that writes into
// store::Settings would be a lie the walkers could not catch, since each one
// only ever sees rows it can reach.
consteval bool ids_namespaced() {
    for (const auto& s : kSettings) {
        const bool rag    = s.id.starts_with("rag.");
        const bool smart  = s.id.starts_with("smart.");
        const bool web    = s.id.starts_with("web_search.");
        if (!rag && !smart && !web) return false;
        if (rag    && s.owner() != Owner::Rag)      return false;
        if (smart  && s.owner() != Owner::Smart)    return false;
        if (web    && s.owner() != Owner::WebSearch) return false;
    }
    return true;
}

// The registry and smart::tuning both name the shipped defaults and ranges.
// tuning is the source; these rows mirror it. Pin them together so a change
// to one is a compile error rather than a silent disagreement between what
// the UI clamps to and what a bare env read clamps to.
consteval bool smart_rows_match_tuning() {
    for (const auto& s : kSettings) {
        if (s.id == "smart.complex_threshold")
            if (s.min != smart::tuning::kComplexMin
             || s.max != smart::tuning::kComplexMax) return false;
        if (s.id == "smart.deep_margin")
            if (s.min != smart::tuning::kDeepMarginMin
             || s.max != smart::tuning::kDeepMarginMax) return false;
        if (s.id == "smart.bias_clamp")
            if (s.min != smart::tuning::kBiasClampMin
             || s.max != smart::tuning::kBiasClampMax) return false;
    }
    return true;
}

// The search rows' bounds are web_search_cfg's constants, and the tool layer
// clamps to the same constants at dispatch. Pin the two together so a row
// cannot advertise a range the dispatch seam then disagrees with.
consteval bool web_search_rows_match_config() {
    for (const auto& s : kSettings) {
        if (s.id == "web_search.count")
            if (s.min != web_search_cfg::kCountMin
             || s.max != web_search_cfg::kCountMax) return false;
        if (s.id == "web_search.max_count")
            if (s.min != web_search_cfg::kMaxCountMin
             || s.max != web_search_cfg::kMaxCountMax) return false;
    }
    return true;
}

static_assert(ids_unique(),        "duplicate setting id");
static_assert(envs_unique(),       "duplicate env var name");
static_assert(ranges_sane(),       "a numeric row has an empty or inverted range");
static_assert(enums_have_options(),"an Enum row has no options");
static_assert(slots_match_types(), "a row's slot type disagrees with its Type");
static_assert(ids_namespaced(),    "setting ids must be namespaced, and the "
                                  "prefix must match the slot's owner");
static_assert(smart_rows_match_tuning(),
              "a smart.* row's range disagrees with smart::tuning");
static_assert(web_search_rows_match_config(),
              "a web_search.* row's range disagrees with web_search_cfg");

} // namespace proofs

// ── Derived operations (settings_registry.cpp) ───────────────────────────
// Each walks the table. None of them names an individual setting.
//
// Every accessor is overloaded per owner. A row is only reachable through the
// struct it binds to, so passing the wrong one is a compile error rather than
// a silent no-op — and each walker naturally SKIPS rows it cannot reach.

// Apply environment overrides on top of `c`. Clamps to each row's range.
void apply_env(store::RagConfig& c);
void apply_env(smart::RoleConfig& c);
void apply_env(web_search_cfg::Config& c);

// Read/write a row's value as a string — the shape `agentty config get/set`
// and the JSON walkers both want. Returns false for an unknown id or a value
// the row cannot hold.
[[nodiscard]] std::string get(const store::RagConfig& c, const SettingDef& d);
[[nodiscard]] std::string get(const smart::RoleConfig& c, const SettingDef& d);
[[nodiscard]] std::string get(const web_search_cfg::Config& c, const SettingDef& d);
[[nodiscard]] bool        set(store::RagConfig& c, const SettingDef& d,
                              std::string_view value);
[[nodiscard]] bool        set(smart::RoleConfig& c, const SettingDef& d,
                              std::string_view value);
[[nodiscard]] bool        set(web_search_cfg::Config& c, const SettingDef& d,
                              std::string_view value);

// True when the row still holds its shipped default — used to keep
// settings.json clean (only non-default rows are written).
[[nodiscard]] bool is_default(const store::RagConfig& c, const SettingDef& d);
[[nodiscard]] bool is_default(const smart::RoleConfig& c, const SettingDef& d);
[[nodiscard]] bool is_default(const web_search_cfg::Config& c, const SettingDef& d);

// The env var that is OVERRIDING this row right now, or "" when none is set.
// A row under an override renders LOCKED and names the variable, instead of
// looking editable and silently losing the edit on the next read.
[[nodiscard]] std::string env_override(const SettingDef& d);

// Restore a row to its shipped default.
void reset(store::RagConfig& c, const SettingDef& d);
void reset(smart::RoleConfig& c, const SettingDef& d);
void reset(web_search_cfg::Config& c, const SettingDef& d);

// One row: label, help, control kind, range, env lock and provenance, all from
// the table. Split out of add_rows for a pane that must put its own rows
// BETWEEN registry rows (Web Search hangs each service's dials under its slot).
template <class C>
inline void add_row(form::Builder& b, const C& cfg, const SettingDef& d) {
    {
        const std::string id{d.id};
        const std::string label{d.label};
        const std::string help{d.help};

        switch (d.type) {
            case Type::Bool:
                b.toggle(id, label, get(cfg, d) == "true", help);
                break;
            case Type::Real: {
                double v = d.min;
                try { v = std::stod(get(cfg, d)); } catch (...) {}
                b.slider(id, label, v, d.min, d.max, d.step, help);
                break;
            }
            case Type::Int: {
                long long v = 0;
                try { v = std::stoll(get(cfg, d)); } catch (...) {}
                b.number(id, label, v, static_cast<std::int64_t>(d.min),
                         static_cast<std::int64_t>(d.max), help);
                break;
            }
            case Type::Enum: {
                std::vector<std::string> opts;
                std::string_view rest = d.options;
                while (!rest.empty()) {
                    const auto bar = rest.find('|');
                    opts.emplace_back(rest.substr(0, bar));
                    if (bar == std::string_view::npos) break;
                    rest.remove_prefix(bar + 1);
                }
                b.choice(id, label, opts, {}, get(cfg, d), help);
                break;
            }
            case Type::Text:
                // Free text: the value is the user's own string, verbatim.
                // Normalising happens where it is USED (web_search_cfg), so the
                // row shows exactly what was typed.
                b.text(id, label, get(cfg, d), help);
                break;
        }

        // An env var overriding this row makes it READ-ONLY and names the
        // variable. The row would otherwise look editable and silently lose
        // the edit on the next read — layered config's worst failure, and the
        // reason `origin` exists at all.
        if (const std::string env = env_override(d); !env.empty()) {
            b.origin("env: " + env);
            b.lock("env: " + env);
        }
        // Provenance: a row still on its shipped value says so, which is the
        // difference between "I never touched this" and "I set it to that".
        else if (is_default(cfg, d)) b.origin("default");
    }
}

// ── Rows ───────────────────────────────────────────────────
//
// Project this table's rows onto a form. THE way a pane renders settings —
// label, help, control kind, range, group header, env lock and provenance all
// come from the row, so a pane contributes only WHICH rows it owns.
//
// This lives here rather than in one pane's .cpp because both panes need it.
// It was private to the RAG pane, so the Smart Mode pane hand-rolled its three
// rows instead: labels, help strings and ranges retyped from the table they
// were already declared in, and the env-lock/provenance handling quietly
// reimplemented and subtly different. Two panes rendering settings two ways is
// exactly the duplication the table exists to remove.
//
// `first` and `last_group` thread across calls so group headers stay correct
// when a pane walks more than one owner.
// Every row `owner` has, in table order, under a header per group.
template <class C>
inline void add_rows(form::Builder& b, const C& cfg, Owner owner, bool advanced,
                     bool& first, Group& last_group) {
    for (const auto& d : kSettings) {
        if (d.owner() != owner) continue;

        // Advanced rows are hidden, not disabled: a knob whose effect a user
        // cannot judge is noise on the main screen, but it still has to be
        // reachable (`a`) rather than env-only.
        if (d.tier == Tier::Advanced && !advanced) continue;

        // A group header separates sections. It is a real field kind, not a
        // locked text row: faking it leaked a placeholder into the value
        // column and let the cursor land on a label that does nothing.
        if (first || d.group != last_group) {
            b.header(std::string{label_of(d.group)});
            last_group = d.group;
            first = false;
        }
        add_row(b, cfg, d);
    }
}

} // namespace agentty::settings::registry
