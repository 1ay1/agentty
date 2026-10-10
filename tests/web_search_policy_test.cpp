// web_search policy (Settings → Web Search): the request rewrite agentty
// applies at the dispatch seam, and the registry rows that persist it.
//
// The rewrite is the whole of what the setting DOES at call time, so it is
// pinned here directly rather than through a live search: the three modes
// (auto leaves the count to the model, on applies the user's default and
// ceiling, off is refused), count resolution in on, and the
// -site: exclusions (documented by DuckDuckGo, observed on Brave; services with a
// native domain filter get the list as a field instead - see domain/web_search_config.hpp).

#include "agtest.hpp"

#include "agentty/domain/web_search_config.hpp"
#include "agentty/runtime/settings_registry.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace sc  = agentty::web_search_cfg;
namespace reg = agentty::settings::registry;
using nlohmann::json;

TEST_CASE("web search policy: exclusions normalise what a user pastes") {
    // Address-bar URLs, a habit-typed operator, case and www. all reduce to
    // the bare host the `site:` operator wants.
    const auto v = sc::exclude_list(
        "https://www.Pinterest.com/pin/123, -site:w3schools.com;"
        "  medium.com:443  site:EXAMPLE.org/");
    CHECK((v == std::vector<std::string>{"pinterest.com", "w3schools.com",
                                        "medium.com", "example.org"}));
}

TEST_CASE("web search policy: junk entries are dropped, duplicates collapse") {
    // No dot, a bare word, a leading hyphen label and a non-ASCII host would
    // each become an operator no engine reads the way the user meant.
    const auto v = sc::exclude_list(
        "localhost foo -bad.com pinterest.com PINTEREST.COM www.pinterest.com "
        "b\xc3\xbc" "cher.de xn--bcher-kva.de");
    CHECK((v == std::vector<std::string>{"pinterest.com", "xn--bcher-kva.de"}));
    CHECK(sc::exclude_list("").empty());
    CHECK(sc::exclude_list(" , ;\t").empty());
}

TEST_CASE("web search policy: count — default, model's request, clamped") {
    sc::Config c;                 // count 10, max 20
    CHECK(sc::effective_count(c, json::object()) == 10);
    CHECK((sc::effective_count(c, json{{"count", 5}}) == 5));
    CHECK((sc::effective_count(c, json{{"count", 500}}) == 20));   // ceiling
    CHECK((sc::effective_count(c, json{{"count", 0}}) == 1));      // floor
    CHECK((sc::effective_count(c, json{{"count", -3}}) == 1));
    // Models send numbers every which way; take them, or fall back.
    CHECK((sc::effective_count(c, json{{"count", "7"}}) == 7));
    CHECK((sc::effective_count(c, json{{"count", 7.9}}) == 7));
    CHECK((sc::effective_count(c, json{{"count", "lots"}}) == 10));
    CHECK((sc::effective_count(c, json{{"count", nullptr}}) == 10));
    // Never a crash, never a silent prefix parse.
    CHECK((sc::effective_count(c, json{{"count", true}}) == 10));
    CHECK((sc::effective_count(c, json{{"count", "7abc"}}) == 10));
    CHECK((sc::effective_count(c, json{{"count", " 7 "}}) == 7));
    CHECK((sc::effective_count(c, json{{"count", 18446744073709551615ULL}}) == 20));
    CHECK((sc::effective_count(c, json{{"count", 1e300}}) == 20));

    // The user's default above their own ceiling resolves to the ceiling,
    // never past it.
    c.count = 15; c.max_count = 5;
    CHECK(sc::effective_count(c, json::object()) == 5);
}

TEST_CASE("web search policy: exclusions append once, model's own kept") {
    const std::vector<std::string> sites{"pinterest.com", "medium.com"};
    CHECK(sc::apply_exclusions("rust async", sites)
          == "rust async -site:pinterest.com -site:medium.com");
    // Already excluded by the model (any case): not repeated.
    CHECK(sc::apply_exclusions("rust -SITE:Pinterest.com", sites)
          == "rust -SITE:Pinterest.com -site:medium.com");
    // A query with a trailing space gets no double space.
    CHECK(sc::apply_exclusions("rust ", sites)
          == "rust -site:pinterest.com -site:medium.com");
    CHECK((sc::apply_exclusions("q", {}) == "q"));
}

TEST_CASE("web search policy: rewrite_args owns count + query, nothing else") {
    sc::Config c;
    c.mode_text = "on";   // the count is applied only in On
    c.count = 6;
    c.exclude_sites = "pinterest.com";
    const json in{{"query", "css grid"}, {"display_description", "why"},
                  {"future_arg", 1}};
    const json out = sc::rewrite_args(c, in);
    CHECK(out["query"] == "css grid -site:pinterest.com");
    CHECK(out["count"] == 6);
    CHECK(out["display_description"] == "why");
    CHECK(out["future_arg"] == 1);

    // No query, wrong type: left for mcp-cpp to reject with its own error.
    CHECK((sc::rewrite_args(c, json{{"query", 3}})["query"] == 3));
    CHECK(sc::rewrite_args(c, json::array()).is_array());

    // Defaults are Auto: the count is the model's, so nothing is written for
    // it; the query still gets exclusions in the wire shape.
    const json plain = sc::rewrite_args(sc::Config{}, json{{"query", "x"}});
    CHECK(plain["query"] == "x");
    CHECK(!plain.contains("count"));

    // On applies the user's default when the model is silent.
    sc::Config on; on.mode_text = "on";
    CHECK(sc::rewrite_args(on, json{{"query", "x"}})["count"] == 10);
}

TEST_CASE("web search policy: auto passes the model's count through untouched") {
    sc::Config c;                       // auto by default
    CHECK(sc::mode(c) == sc::Mode::Auto);
    // The model asks for 50; Auto neither clamps it nor fills a default in.
    CHECK(sc::rewrite_args(c, json{{"query", "x"}, {"count", 50}})["count"] == 50);
    // Exclusions still apply in Auto.
    c.exclude_sites = "pinterest.com";
    CHECK(sc::rewrite_args(c, json{{"query", "x"}})["query"] == "x -site:pinterest.com");
    // A typo or hand edit reads as Auto, never as On or Off.
    sc::Config typo; typo.mode_text = "ON ";
    CHECK(sc::mode(typo) == sc::Mode::Auto);
}

TEST_CASE("web search policy: every web_search.* row round-trips through the registry") {
    // The same contract smart_settings_roundtrip pins for Smart Mode: a row
    // the pane can show must be writable back, whatever its control type.
    int seen = 0;
    for (const auto& d : reg::kSettings) {
        if (d.owner() != reg::Owner::WebSearch) continue;
        ++seen;
        sc::Config c;
        switch (d.type) {
            case reg::Type::Bool:
                CHECK(reg::set(c, d, "false"));
                CHECK(reg::get(c, d) == "false");
                CHECK(!reg::is_default(c, d));
                CHECK(reg::set(c, d, "true"));
                CHECK(reg::get(c, d) == "true");
                break;
            case reg::Type::Int:
                CHECK(reg::set(c, d, "3"));
                CHECK(reg::get(c, d) == "3");
                // Clamped on the way in, from every source.
                CHECK(reg::set(c, d, "100000"));
                CHECK(reg::get(c, d) == std::to_string(static_cast<int>(d.max)));
                break;
            case reg::Type::Text:
                CHECK(reg::set(c, d, "pinterest.com\nmedium.com"));
                // A pasted newline cannot reach settings.json.
                CHECK(reg::get(c, d) == "pinterest.commedium.com");
                CHECK(reg::set(c, d, ""));
                CHECK(reg::is_default(c, d));
                break;
            case reg::Type::Enum: {
                // Every declared option round-trips (web_search.mode, the three
                // service slots, language); case is folded to the canonical
                // word; anything else is refused and leaves the row unchanged.
                INFO(d.id);
                std::vector<std::string> opts;
                for (std::string_view rest = d.options; !rest.empty();) {
                    const auto bar = rest.find('|');
                    opts.emplace_back(rest.substr(0, bar));
                    if (bar == std::string_view::npos) break;
                    rest.remove_prefix(bar + 1);
                }
                REQUIRE(opts.size() >= 2);
                for (const auto& v : opts) {
                    CHECK(reg::set(c, d, v));
                    CHECK(reg::get(c, d) == v);
                }
                std::string upper = opts.back();
                for (char& ch : upper)
                    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                CHECK(reg::set(c, d, upper));
                CHECK(reg::get(c, d) == opts.back());
                CHECK_FALSE(reg::set(c, d, "sometimes"));
                CHECK(reg::get(c, d) == opts.back());
                break;
            }
            case reg::Type::Real:
                CHECK(false);   // no search row uses this today
                break;
        }
        reg::reset(c, d);
        CHECK(reg::is_default(c, d));
    }
    // mode, count, max_count, exclude_sites; primary, secondary, fallback,
    // language.
    CHECK(seen == 8);
}

TEST_CASE("web search policy: the row a user sees first is the master switch") {
    // The pane walks the table in order; the on/off row leads so it is the
    // one the cursor opens on.
    const reg::SettingDef* first = nullptr;
    for (const auto& d : reg::kSettings)
        if (d.owner() == reg::Owner::WebSearch) { first = &d; break; }
    REQUIRE(first != nullptr);
    CHECK((first->id == std::string_view{"web_search.mode"}));
    CHECK(first->type == reg::Type::Enum);
    // auto first: the dropdown opens on the default, and the order the pane
    // shows is the order the test helpers step through.
    CHECK(first->options == std::string_view{"auto|on|off"});
}

TEST_CASE("web search policy: AGENTTY_WEB_SEARCH accepts boolean spellings") {
    const auto with = [](const char* v) {
#ifdef _WIN32
        _putenv_s("AGENTTY_WEB_SEARCH", v);
#else
        ::setenv("AGENTTY_WEB_SEARCH", v, 1);
#endif
        sc::Config c;
        reg::apply_env(c);
#ifdef _WIN32
        _putenv_s("AGENTTY_WEB_SEARCH", "");
#else
        ::unsetenv("AGENTTY_WEB_SEARCH");
#endif
        return sc::mode(c);
    };
    // Old spellings: false is off; true was "available, model picks the
    // count" — Auto, never On (On would start clamping unasked).
    for (const char* v : {"0", "false", "no", "OFF"}) CHECK(with(v) == sc::Mode::Off);
    for (const char* v : {"1", "true", "yes"})        CHECK(with(v) == sc::Mode::Auto);
    // The mode words themselves.
    CHECK(with("on")   == sc::Mode::On);
    CHECK(with("On")   == sc::Mode::On);
    CHECK(with("auto") == sc::Mode::Auto);
    // Junk is ignored: the default stands.
    CHECK(with("maybe") == sc::Mode::Auto);
}
