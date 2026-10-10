// Search services: the plan agentty hands mcp-cpp, the key store behind the
// paid services, and the Web Search rows that choose them.
//
// What this pins, and why each is worth pinning:
//   * the registry's list of service ids IS mcp-cpp's catalogue. Two lists of
//     the same thing drift; this fails the build when they do.
//   * a plan is built from settings alone (no IO), so every rule about it —
//     order, duplicates, unknown ids, missing keys — is a plain assertion.
//   * a key never reaches settings.json or the Model. It is stored sealed and
//     is shown only as "stored".
//   * dials and keys appear under the slot that uses them, and a free service
//     adds no key row.

#include "agtest.hpp"

#include "agentty/auth/keystore.hpp"
#include "agentty/io/persistence.hpp"
#include "agentty/runtime/panel/web_search_form.hpp"
#include "agentty/runtime/settings_registry.hpp"
#include "agentty/tool/web_search_plan.hpp"
#include "agentty/tool/web_search_policy.hpp"
#include "agentty/tool/web_search_secret.hpp"

#include <mcp/tools/web_search.hpp>

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using namespace agentty;
namespace sc  = agentty::web_search_cfg;
namespace reg = agentty::settings::registry;
namespace wtf = agentty::web_search_form;
namespace sp  = agentty::tools::web_search_plan;
namespace ss  = agentty::tools::web_search_secret;
namespace fs  = std::filesystem;

namespace {

// A private AGENTTY_HOME with the OS keystore off, so the sealed FILE is what
// the assertions read and nothing leaves the temp dir.
//
// "Off" has a catch: auth::keystore::available() caches its answer for the
// life of the process, so if AGENTTY_USE_KEYSTORE was already set when an
// earlier test asked, setting it to 0 here changes nothing and a store()
// would land in the developer's REAL keyring. `real_keyring` reports that,
// and the tests that write a key skip rather than touch it.
struct TmpHome {
    fs::path    dir;
    std::string old_home, old_keystore, old_exa;
    bool        had_home = false, had_ks = false, had_exa = false;
    TmpHome() {
        dir = fs::temp_directory_path()
            / ("agentty_web_search_secret_test_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir / "home");
        if (const char* h = ::getenv("AGENTTY_HOME")) { had_home = true; old_home = h; }
        if (const char* k = ::getenv("AGENTTY_USE_KEYSTORE")) { had_ks = true; old_keystore = k; }
        if (const char* e = ::getenv("AGENTTY_WEB_SEARCH_KEY_EXA_API")) { had_exa = true; old_exa = e; }
        ::setenv("AGENTTY_HOME", (dir / "home").c_str(), 1);
        ::setenv("AGENTTY_USE_KEYSTORE", "0", 1);
        ::unsetenv("AGENTTY_WEB_SEARCH_KEY_EXA_API");
        real_keyring = agentty::auth::keystore::available();
    }
    bool real_keyring = false;
    ~TmpHome() {
        if (had_home) ::setenv("AGENTTY_HOME", old_home.c_str(), 1);
        else          ::unsetenv("AGENTTY_HOME");
        if (had_ks) ::setenv("AGENTTY_USE_KEYSTORE", old_keystore.c_str(), 1);
        else        ::unsetenv("AGENTTY_USE_KEYSTORE");
        if (had_exa) ::setenv("AGENTTY_WEB_SEARCH_KEY_EXA_API", old_exa.c_str(), 1);
        else         ::unsetenv("AGENTTY_WEB_SEARCH_KEY_EXA_API");
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    static std::string slurp(const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
    [[nodiscard]] std::string all_files_text() const {
        std::string out;
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(dir, ec))
            if (e.is_regular_file(ec)) out += slurp(e.path());
        return out;
    }
};

std::vector<std::string> split_bar(std::string_view s) {
    std::vector<std::string> out;
    while (!s.empty()) {
        const auto bar = s.find('|');
        out.emplace_back(s.substr(0, bar));
        if (bar == std::string_view::npos) break;
        s.remove_prefix(bar + 1);
    }
    return out;
}

std::vector<std::string> field_ids(const form::Form& f) {
    std::vector<std::string> ids;
    for (const auto& r : f.fields) ids.push_back(r.id);
    return ids;
}

bool has(const std::vector<std::string>& v, std::string_view x) {
    for (const auto& s : v) if (s == x) return true;
    return false;
}

const std::string kNoKey;
const sp::KeyLookup no_keys = [](std::string_view) { return std::string{}; };

} // namespace

// ── The two lists of services cannot disagree ────────────────────────────

TEST_CASE("web search services: the registry's ids are mcp-cpp's catalogue") {
    std::vector<std::string> cat;
    for (const auto& s : ::mcp::tools::web_search_services()) cat.push_back(s.id);

    // Primary: exactly the catalogue, in menu order.
    CHECK(split_bar(sc::kServiceIds) == cat);

    // Secondary and Fallback: "none", then exactly the same.
    auto opt = split_bar(sc::kOptionalServiceIds);
    REQUIRE(!opt.empty());
    CHECK(opt.front() == "none");
    opt.erase(opt.begin());
    CHECK(opt == cat);
}

TEST_CASE("web search services: the shipped defaults are Brave, DuckDuckGo, Tavily - all free") {
    sc::Config c;
    CHECK(c.primary == "brave-free");
    CHECK(c.secondary == "ddg-free");
    CHECK(c.fallback == "tavily-free");
    for (const auto* id : {&c.primary, &c.secondary, &c.fallback}) {
        const auto* svc = ::mcp::tools::find_web_search_service(*id);
        REQUIRE(svc != nullptr);
        CHECK(!svc->needs_key);
    }
    // And they equal mcp-cpp's own default plan, so the two never diverge.
    const auto dflt = ::mcp::tools::default_web_search_plan();
    REQUIRE(dflt.slots.size() == 3);
    CHECK(dflt.slots[0].service == c.primary);
    CHECK(dflt.slots[1].service == c.secondary);
    CHECK(dflt.slots[2].service == c.fallback);
}

TEST_CASE("web search services: the language list is valid ISO 639-1") {
    for (const auto& l : split_bar(sc::kLanguages)) {
        INFO(l);
        CHECK((l == "auto" || l.size() == 2));
    }
}

// ── build_plan ───────────────────────────────────────────────────────────

TEST_CASE("web search plan: default settings give the three free slots in order") {
    const auto p = sp::build_plan(sc::Config{}, no_keys);
    REQUIRE(p.slots.size() == 3);
    CHECK(p.slots[0].service == "brave-free");
    CHECK(p.slots[1].service == "ddg-free");
    CHECK(p.slots[2].service == "tavily-free");
    CHECK(p.language.empty());
}

TEST_CASE("web search plan: none, unknown ids and repeats are left out") {
    sc::Config c;
    c.primary = "exa-api"; c.secondary = "none"; c.fallback = "exa-api";
    auto p = sp::build_plan(c, [](std::string_view) { return std::string{"k-1234567890"}; });
    REQUIRE(p.slots.size() == 1);
    CHECK(p.slots[0].service == "exa-api");

    c.primary = "kagi-api";            // a service this build does not ship
    c.secondary = "tavily-free"; c.fallback = "none";
    p = sp::build_plan(c, no_keys);
    REQUIRE(p.slots.size() == 1);
    CHECK(p.slots[0].service == "tavily-free");

    // Nothing usable: the key-less default, rather than a tool that cannot answer.
    c.primary = "bogus"; c.secondary = "none"; c.fallback = "none";
    p = sp::build_plan(c, no_keys);
    CHECK(p.slots.size() == 3);
}

TEST_CASE("web search plan: keys go to keyed services only, and a missing key stays visible") {
    sc::Config c;
    c.primary = "serper-api"; c.secondary = "tavily-free"; c.fallback = "none";
    std::vector<std::string> asked;
    const auto p = sp::build_plan(c, [&](std::string_view s) {
        asked.emplace_back(s);
        return std::string{"secret-key-123"};
    });
    REQUIRE(p.slots.size() == 2);
    CHECK(p.slots[0].api_key == "secret-key-123");
    CHECK(p.slots[1].api_key.empty());           // a free service gets no key
    CHECK(asked == std::vector<std::string>{"serper-api"});   // and is never asked for one

    // No key stored: the slot STAYS, empty, so the report can say so.
    const auto q = sp::build_plan(c, no_keys);
    REQUIRE(q.slots.size() == 2);
    CHECK(q.slots[0].service == "serper-api");
    CHECK(q.slots[0].api_key.empty());
}

TEST_CASE("web search plan: dials follow their service, not their slot") {
    sc::Config c;
    c.primary = "firecrawl-free"; c.secondary = "tavily-free"; c.fallback = "none";
    c.dials["firecrawl-free.category"] = "github";
    c.dials["tavily-free.topic"] = "news";
    c.dials["exa-api.type"] = "fast";             // not in the plan
    auto p = sp::build_plan(c, no_keys);
    REQUIRE(p.slots.size() == 2);
    REQUIRE(p.slots[0].dials.size() == 1);
    CHECK(p.slots[0].dials[0].first == "category");
    CHECK(p.slots[0].dials[0].second == "github");
    REQUIRE(p.slots[1].dials.size() == 1);
    CHECK(p.slots[1].dials[0].first == "topic");

    // Swapping the order moves the service and its dials together.
    std::swap(c.primary, c.secondary);
    p = sp::build_plan(c, no_keys);
    CHECK(p.slots[0].service == "tavily-free");
    CHECK(p.slots[0].dials[0].second == "news");
}

TEST_CASE("web search plan: language 'auto' sends nothing, a code is passed on") {
    sc::Config c;
    CHECK(sp::build_plan(c, no_keys).language.empty());
    c.language = "de";
    CHECK(sp::build_plan(c, no_keys).language == "de");
    c.language = "garbage";
    CHECK(sp::build_plan(c, no_keys).language.empty());
    // Two bytes are not a language: only two lower-case letters are sent.
    for (const char* bad : {"A&", "EN", "\n\n", "e1"}) {
        c.language = bad;
        CHECK(sp::build_plan(c, no_keys).language.empty(), bad);
    }
}

TEST_CASE("web search settings: a dial entry must look like <service>.<dial>") {
    CHECK(sc::dial_entry_ok("brave-free.freshness", "pw"));
    CHECK(!sc::dial_entry_ok("nodot", "pw"));
    CHECK(!sc::dial_entry_ok(".freshness", "pw"));
    CHECK(!sc::dial_entry_ok("brave-free.", "pw"));
    CHECK(!sc::dial_entry_ok("brave-free.freshness", ""));
    CHECK(!sc::dial_entry_ok("brave-free.freshness", "p\nw"));
    CHECK(!sc::dial_entry_ok(std::string(65, 'a') + ".x", "pw"));
}

// ── The key store ────────────────────────────────────────────────────────

TEST_CASE("web search secret: a key round-trips, sealed, and is never in plaintext") {
    TmpHome home;
    if (home.real_keyring) { MESSAGE("skipped: OS keystore armed for this process"); return; }
    const std::string key = "exa-LIVE-0123456789abcdef";
    CHECK(!ss::has("exa-api"));
    CHECK(ss::origin("exa-api").empty());

    REQUIRE(ss::store("exa-api", key));
    CHECK(ss::load("exa-api") == key);
    CHECK(ss::has("exa-api"));
    CHECK(ss::origin("exa-api") == "stored");

    // Nothing on disk, anywhere under the home, contains the key.
    CHECK(home.all_files_text().find("0123456789abcdef") == std::string::npos);
    // ...and another service does not see it.
    CHECK(!ss::has("serper-api"));

    CHECK(ss::erase("exa-api"));
    CHECK(!ss::has("exa-api"));
}

TEST_CASE("web search secret: the environment beats the store and is named, not shown") {
    TmpHome home;
    if (home.real_keyring) { MESSAGE("skipped: OS keystore armed for this process"); return; }
    REQUIRE(ss::store("exa-api", "stored-key-0123456789"));
    ::setenv("AGENTTY_WEB_SEARCH_KEY_EXA_API", "env-key-9876543210", 1);
    CHECK(ss::load("exa-api") == "env-key-9876543210");
    CHECK(ss::origin("exa-api") == "env: AGENTTY_WEB_SEARCH_KEY_EXA_API");
    CHECK(ss::env_name("brave-api") == "AGENTTY_WEB_SEARCH_KEY_BRAVE_API");
    ::unsetenv("AGENTTY_WEB_SEARCH_KEY_EXA_API");
    CHECK(ss::load("exa-api") == "stored-key-0123456789");
}

TEST_CASE("web search secret: an unreadable key file is never overwritten") {
    TmpHome home;
    if (home.real_keyring) { MESSAGE("skipped: OS keystore armed for this process"); return; }
    REQUIRE(ss::store("exa-api", "exa-FIRST-0123456789"));
    // Find the sealed file and damage it, as a disk error or a machine-id
    // change would: it exists but can no longer be opened.
    fs::path file;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(home.dir, ec))
        if (e.path().filename() == "web_search_keys.json") file = e.path();
    REQUIRE(!file.empty());
    { std::ofstream out(file, std::ios::binary | std::ios::trunc); out << "not a sealed envelope"; }
    // Storing another service's key must not replace the file (which would
    // lose exa-api's key for good if the damage is recoverable).
    CHECK(!ss::store("serper-api", "serper-SECOND-0123456789"));
    CHECK(TmpHome::slurp(file) == "not a sealed envelope");
    CHECK(!ss::has("exa-api"));
}

TEST_CASE("web search secret: implausible keys are refused before they are stored") {
    TmpHome home;
    CHECK(!ss::plausible("short"));
    CHECK(!ss::plausible("has a space inside it"));
    CHECK(!ss::plausible("line\nbreak-0123456789"));
    CHECK(!ss::plausible(std::string(513, 'a')));
    CHECK(ss::plausible("tvly-0123456789abcdef"));
    CHECK(!ss::store("exa-api", "two words here"));
    CHECK(!ss::has("exa-api"));
}

TEST_CASE("web search secret: the live plan source reads policy and key per call") {
    TmpHome home;
    // The policy slot is process-wide and other tests install into it, so
    // install a known one here and put back what was there afterwards.
    const auto saved = agentty::tools::web_search_policy::current();
    struct Restore {
        sc::Config c;
        ~Restore() { agentty::tools::web_search_policy::install(c); }
    } restore{saved};

    auto source = sp::make_plan_source();
    REQUIRE(source != nullptr);

    sc::Config c;
    c.primary = "serper-api"; c.secondary = "tavily-free"; c.fallback = "none";
    agentty::tools::web_search_policy::install(c);
    ::setenv("AGENTTY_WEB_SEARCH_KEY_SERPER_API", "env-serper-0123456789", 1);
    auto p = source->plan();
    REQUIRE(p.slots.size() == 2);
    CHECK(p.slots[0].service == "serper-api");
    CHECK(p.slots[0].api_key == "env-serper-0123456789");

    // A change is seen by the NEXT call, with no new source.
    ::unsetenv("AGENTTY_WEB_SEARCH_KEY_SERPER_API");
    c.primary = "firecrawl-free";
    agentty::tools::web_search_policy::install(c);
    p = source->plan();
    REQUIRE(p.slots.size() == 2);
    CHECK(p.slots[0].service == "firecrawl-free");
    CHECK(p.slots[0].api_key.empty());
}

// ── The pane's rows ──────────────────────────────────────────────────────

TEST_CASE("web search form: free defaults add dial rows and no key row") {
    const auto f = wtf::build_form(sc::Config{});
    const auto ids = field_ids(f);
    // Slot rows exist, in order.
    auto at = [&](std::string_view id) {
        for (std::size_t i = 0; i < ids.size(); ++i) if (ids[i] == id) return static_cast<int>(i);
        return -1;
    };
    REQUIRE(at("web_search.primary") >= 0);
    CHECK(at("web_search.primary") < at("web_search.secondary"));
    CHECK(at("web_search.secondary") < at("web_search.fallback"));
    // Brave Free offers freshness, under Primary.
    CHECK(at("web_search.dial.brave-free.freshness") > at("web_search.primary"));
    CHECK(at("web_search.dial.brave-free.freshness") < at("web_search.secondary"));
    // DuckDuckGo Free offers two, under Secondary.
    CHECK(at("web_search.dial.ddg-free.region") > at("web_search.secondary"));
    CHECK(at("web_search.dial.ddg-free.safe") < at("web_search.fallback"));
    // Nothing free asks for a key.
    for (const auto& id : ids) CHECK(id.rfind("web_search.key.", 0) != 0);
}

TEST_CASE("web search form: a keyed service gets a Secret row that starts empty") {
    sc::Config c; c.primary = "exa-api";
    const auto f = wtf::build_form(c, [](std::string_view s) {
        return s == "exa-api" ? std::string{"stored"} : std::string{};
    });
    const auto* row = f.find("web_search.key.exa-api");
    REQUIRE(row != nullptr);
    const auto* sec = std::get_if<form::field::Secret>(&row->value);
    REQUIRE(sec != nullptr);
    CHECK(sec->value.empty());            // the pane is never given the key
    CHECK(row->origin == "stored");
    CHECK(wtf::key_row_service(row->id) == "exa-api");
    // A stored key can be removed.
    CHECK(f.find("web_search.keyclear.exa-api") != nullptr);

    // No key yet: "not set", and nothing to remove.
    const auto g = wtf::build_form(c);
    CHECK(g.find("web_search.key.exa-api")->origin == "not set");
    CHECK(g.find("web_search.keyclear.exa-api") == nullptr);
}

TEST_CASE("web search form: a key from the environment locks its row") {
    sc::Config c; c.primary = "exa-api";
    const auto f = wtf::build_form(c, [](std::string_view) {
        return std::string{"env: AGENTTY_WEB_SEARCH_KEY_EXA_API"};
    });
    const auto* row = f.find("web_search.key.exa-api");
    REQUIRE(row != nullptr);
    CHECK(row->locked);
    CHECK(f.find("web_search.keyclear.exa-api") == nullptr);    // not ours to delete
}

TEST_CASE("web search form: a service named twice shows its rows once") {
    sc::Config c; c.primary = "tavily-api"; c.secondary = "tavily-api"; c.fallback = "tavily-api";
    const auto ids = field_ids(wtf::build_form(c));
    int keys = 0;
    for (const auto& id : ids) if (id == "web_search.key.tavily-api") ++keys;
    CHECK(keys == 1);
}

TEST_CASE("web search form: slot rows read as service names") {
    const auto f = wtf::build_form(sc::Config{});
    const auto* ch = std::get_if<form::field::Choice>(&f.find("web_search.primary")->value);
    REQUIRE(ch != nullptr);
    CHECK(ch->label() == "Brave Free");
    CHECK(ch->id() == "brave-free");          // the stored value is still the id
    // Secondary/Fallback may be emptied; Primary may not.
    const auto* sec = std::get_if<form::field::Choice>(&f.find("web_search.secondary")->value);
    REQUIRE(sec != nullptr);
    bool none = false;
    for (const auto& i : sec->ids) none = none || i == "none";
    CHECK(none);
    for (const auto& i : ch->ids) CHECK(i != "none");
}

TEST_CASE("web search form: a dial round-trips through apply_form, and the default erases it") {
    sc::Config c;
    auto f = wtf::build_form(c);
    auto* row = f.find("web_search.dial.brave-free.freshness");
    REQUIRE(row != nullptr);
    auto* ch = std::get_if<form::field::Choice>(&row->value);
    REQUIRE(ch != nullptr);
    ch->select_id("pw");
    wtf::apply_form(f, c);
    CHECK(c.dials.at("brave-free.freshness") == "pw");

    // Rebuilt from the config, the row shows what was stored.
    auto g = wtf::build_form(c);
    CHECK(std::get<form::field::Choice>(g.find("web_search.dial.brave-free.freshness")->value).id() == "pw");

    // Back to "Any time": the entry is removed, not stored empty.
    std::get<form::field::Choice>(g.find("web_search.dial.brave-free.freshness")->value).select_id("");
    wtf::apply_form(g, c);
    CHECK(c.dials.find("brave-free.freshness") == c.dials.end());
}

TEST_CASE("web search form: choosing a slot's service changes the rows under it") {
    sc::Config c;
    c.primary = "exa-api";
    const auto ids = field_ids(wtf::build_form(c));
    CHECK(has(ids, "web_search.dial.exa-api.type"));
    CHECK(has(ids, "web_search.key.exa-api"));
    CHECK(!has(ids, "web_search.dial.brave-free.freshness"));
    // A stale dial for a service not in use is kept in the config, not shown.
    c.dials["brave-free.freshness"] = "pd";
    CHECK(!has(field_ids(wtf::build_form(c)), "web_search.dial.brave-free.freshness"));
    CHECK(c.dials.count("brave-free.freshness") == 1);
}

TEST_CASE("web search form: off locks every service row too") {
    sc::Config c; c.mode_text = "off"; c.primary = "exa-api";
    const auto f = wtf::build_form(c);
    for (const auto& r : f.fields) {
        if (r.id == "web_search.mode" || r.is_header()) { CHECK(!r.locked); continue; }
        INFO(r.id);
        CHECK(r.locked);
    }
}

// ── settings.json ────────────────────────────────────────────────────────

TEST_CASE("web search settings: dials and slots persist, and a key never does") {
    TmpHome home;
    if (home.real_keyring) { MESSAGE("skipped: OS keystore armed for this process"); return; }
    store::Settings s;
    s.web_search.primary = "exa-api";
    s.web_search.secondary = "none";
    s.web_search.language = "de";
    s.web_search.dials["exa-api.type"] = "fast";
    REQUIRE(ss::store("exa-api", "exa-LEAKCHECK-0123456789"));
    persistence::save_settings(s);

    const auto text = TmpHome::slurp(home.dir / "home" / "settings.json");
    CHECK(text.find("\"exa-api\"") != std::string::npos);
    CHECK(text.find("LEAKCHECK") == std::string::npos);
    CHECK(home.all_files_text().find("LEAKCHECK") == std::string::npos);

    const auto back = persistence::load_settings();
    CHECK(back.web_search.primary == "exa-api");
    CHECK(back.web_search.secondary == "none");
    CHECK(back.web_search.fallback == "tavily-free");        // untouched default
    CHECK(back.web_search.language == "de");
    CHECK(back.web_search.dials.at("exa-api.type") == "fast");
}

TEST_CASE("web search settings: a hand-edited bad value cannot reach a slot") {
    TmpHome home;
    {
        std::ofstream out(home.dir / "home" / "settings.json");
        out << R"({"web_search":{"primary":"not-a-service","secondary":"ddg-free",)"
               R"("language":"klingon","dials":{"brave-free.freshness":"pw",)"
               R"("x":7,"":"y"}}})";
    }
    const auto s = persistence::load_settings();
    // An enum row only accepts a declared option.
    CHECK(s.web_search.primary == "brave-free");
    CHECK(s.web_search.language == "auto");
    CHECK(s.web_search.secondary == "ddg-free");
    // Non-string and empty-keyed dial entries are dropped; the good one is kept.
    CHECK(s.web_search.dials.size() == 1);
    CHECK(s.web_search.dials.at("brave-free.freshness") == "pw");
}
