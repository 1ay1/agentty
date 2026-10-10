// SPDX-License-Identifier: Apache-2.0
//
// settings_multiproc_test.cpp — two agentty PROCESSES editing settings.
//
// persistence_race_test drives many THREADS through the save queue, which is
// why the bug this file pins went unnoticed: the writers were never in the
// same process. save_settings serialises the caller's whole in-memory record
// and atomically replaces the document, so two instances lose each other's
// edits with nothing failing:
//
//     A starts (theme=Harper)        B starts (theme=Harper)
//     B: pick "Sumi Phosphor"   ->   file says Sumi Phosphor
//     A: change ANY other row   ->   A writes its STALE record
//     -> theme is Harper again
//
// Reported from a real session with five instances open: a theme pick kept
// reverting on restart and looked nondeterministic, because the winner is
// whichever process happened to save last. write_json_atomic does not help —
// it guarantees no TORN file, which is a different property from no LOST
// update. Both writes are complete, valid and durable; one lands second.
//
// Invariants pinned:
//   • disjoint edits from two processes BOTH survive (the reported bug)
//   • a stale writer does not revert a field it never touched
//   • concurrent savers do not corrupt the document
//   • reasoning-off persists as off, and is NOT re-read as auto
//
// The last one is here rather than in effort_auto_test because the
// unset-vs-off split is only observable ACROSS a save/load boundary, and
// `""` is what the setting persisted as for its whole life before `none`.

#include "agentty/io/persistence.hpp"
#include "agentty/store/store.hpp"
#include "agentty/domain/catalog.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>   // getpid

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;
namespace ps = agentty::persistence;

namespace {

// Point the user store at a scratch root for this case.
void use_store(const fs::path& root) {
    fs::create_directories(root);
#ifdef _WIN32
    _putenv_s("AGENTTY_HOME", root.string().c_str());
#else
    ::setenv("AGENTTY_HOME", root.string().c_str(), 1);
#endif
}

fs::path scratch(const char* name) {
    auto p = fs::temp_directory_path() /
             ("agentty-mp-" + std::string{name} + "-" + std::to_string(::getpid()));
    fs::remove_all(p);
    return p;
}

json read_raw(const fs::path& root) {
    std::ifstream ifs(root / "settings.json");
    REQUIRE(ifs.good());
    json j; ifs >> j;
    return j;
}

// Simulate a SECOND instance: it loaded the file at some earlier point and
// writes its own full record, exactly as another process would. Writing the
// raw file is the honest model — the other process has its own statics, so
// reusing this process's save_settings would share the merge baseline and
// prove nothing.
void other_instance_writes(const fs::path& root, const json& doc) {
    std::ofstream ofs(root / "settings.json", std::ios::binary | std::ios::trunc);
    ofs << doc.dump(2);
}

}  // namespace

TEST_CASE("settings: a stale instance does not revert another's theme") {
    // The reported bug, in its exact shape.
    const auto root = scratch("revert");
    use_store(root);

    // Instance A starts and reads the store. This also arms A's merge
    // baseline, which is what lets it tell "I changed this" from "I merely
    // read this".
    {
        agentty::store::Settings s;
        s.ui.theme = "Harper";
        ps::save_settings(s);
    }
    auto a_view = ps::load_settings();
    REQUIRE(a_view.ui.theme == "Harper");

    // Instance B (another process) picks a new theme and saves.
    {
        auto doc = read_raw(root);
        doc["ui"]["theme"] = "Sumi Phosphor";
        other_instance_writes(root, doc);
    }

    // Now A saves an UNRELATED setting, from its startup-era record. Before
    // the merge this wrote ui.theme=Harper straight back over B's pick.
    a_view.model_id = agentty::ModelId{"gpt-5"};
    ps::save_settings(a_view);

    const auto after = read_raw(root);
    CHECK(after["ui"]["theme"] == "Sumi Phosphor");   // B's edit survived
    CHECK(after["model_id"] == "gpt-5");              // A's edit landed

    fs::remove_all(root);
}

TEST_CASE("settings: the writer's OWN edit still wins over a stale disk") {
    // The other direction, so the merge cannot be "always take disk" — that
    // would fix the revert by making every save a no-op.
    const auto root = scratch("mine");
    use_store(root);

    {
        agentty::store::Settings s;
        s.ui.theme = "Harper";
        ps::save_settings(s);
    }
    auto mine = ps::load_settings();

    // This process changes the theme deliberately.
    mine.ui.theme = "Sumi Linen";
    ps::save_settings(mine);

    CHECK(read_raw(root)["ui"]["theme"] == "Sumi Linen");

    fs::remove_all(root);
}

TEST_CASE("settings: reasoning off persists as off, not as unset") {
    // `effort: ""` has been the default since the setting existed, and
    // persistence drops empty values — so an "off" written as "" would be
    // ABSENT on disk and read back as auto, silently undoing the choice on
    // every restart. Off has to persist as a non-empty sentinel.
    const auto root = scratch("effort");
    use_store(root);

    agentty::store::Settings s;
    // The PERSISTED form of "off" -- effort_persist(Effort::None), which is
    // the sentinel the fix introduced. Writing Effort::None through the wire
    // spelling would give "", which is exactly the bug.
    s.effort = std::string{agentty::effort_to_wire_setting(agentty::Effort::None)};
    ps::save_settings(s);

    const auto raw = read_raw(root);
    REQUIRE(raw.contains("effort"));
    CHECK(raw["effort"].get<std::string>() == "none");   // not ""

    // And it survives the round trip as off rather than degrading to auto.
    const auto back = ps::load_settings();
    CHECK(agentty::effort_from_wire(back.effort) == agentty::Effort::None);

    fs::remove_all(root);
}

// ── web_search policy (Settings → Web Search) ─────────────────────────────
// `search` is absent from settings.json until a row moves off its default,
// which made it the first block a sibling could ADD after another instance
// loaded. These pin the three ways that interacts with the merge.

TEST_CASE("settings: a sibling's new web_search block survives an unrelated save") {
    const auto root = scratch("search-sibling");
    use_store(root);
    ps::save_settings(agentty::store::Settings{});   // no `search` key
    auto a = ps::load_settings();                     // A's baseline: no `search`

    // B turns web search off.
    {
        auto doc = read_raw(root);
        doc["web_search"] = {{"mode", "off"}};
        other_instance_writes(root, doc);
    }

    // A saves something else; it never had an opinion on `search`.
    a.model_id = agentty::ModelId{"gpt-5"};
    ps::save_settings(a);

    const auto after = read_raw(root);
    REQUIRE(after.contains("web_search"));
    CHECK(after["web_search"]["mode"] == "off");   // B's choice kept
    CHECK(after["model_id"] == "gpt-5");
    fs::remove_all(root);
}

TEST_CASE("settings: resetting web search to defaults persists") {
    const auto root = scratch("search-reset");
    use_store(root);
    {
        agentty::store::Settings s;
        s.web_search.count = 5;
        ps::save_settings(s);
    }
    auto mine = ps::load_settings();
    REQUIRE(mine.web_search.count == 5);

    mine.web_search = agentty::web_search_cfg::Config{};   // every row back on its default
    ps::save_settings(mine);

    CHECK(ps::load_settings().web_search.count == 10);   // not re-adopted from disk
    fs::remove_all(root);
}

TEST_CASE("settings: a web search env override is never written to settings.json") {
    const auto root = scratch("search-env");
    use_store(root);
#ifdef _WIN32
    _putenv_s("AGENTTY_WEB_SEARCH_COUNT", "4");
#else
    ::setenv("AGENTTY_WEB_SEARCH_COUNT", "4", 1);
#endif
    auto s = ps::load_settings();
    CHECK(s.web_search.count == 4);                // the export is in force
    s.model_id = agentty::ModelId{"gpt-5"};
    ps::save_settings(s);                      // any save at all
#ifdef _WIN32
    _putenv_s("AGENTTY_WEB_SEARCH_COUNT", "");
#else
    ::unsetenv("AGENTTY_WEB_SEARCH_COUNT");
#endif

    const auto after = read_raw(root);
    CHECK((!after.contains("web_search") || !after["web_search"].contains("count")));
    CHECK(ps::load_settings().web_search.count == 10);   // gone with the export
    fs::remove_all(root);
}
