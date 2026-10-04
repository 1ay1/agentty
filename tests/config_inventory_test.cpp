// config_inventory_test — the inventory is the ONLY declaration.
//
// THE RULE
// ========
// Every scope::Layout and dirs::Spec in agentty is declared in
// include/agentty/config/inventory.hpp, and every consumer reads it from
// there. No file builds its own.
//
// This matters because `agentty config` exists to tell a user where their
// config comes from. A tool that describes the program is worthless if it
// can drift from the program — a WRONG explanation is worse than none,
// because now you believe it. The only way to make drift impossible is to
// have one declaration, so the command and the feature cannot disagree.
//
// It is the same lesson as the dialect fix: that bug was two decision sites
// for one decision (plan() emitted six sources, a downstream filter read
// three), and the fix was to delete the second site, not document it.
//
// So it is checked mechanically, against the source, on every build.

#include "agtest.hpp"

#include "agentty/config/inventory.hpp"
#include "agentty/util/user_root.hpp"

#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace cfg = agentty::config;

namespace {

// The ONE file allowed to declare a Layout or a Spec.
const std::vector<std::string> kAllowed = {
    "include/agentty/config/inventory.hpp",
};

// Tests legitimately fabricate Layouts to exercise the pure algebra with a
// synthetic Env — that is the whole point of scope's purity contract, and
// those are not real storage locations.
[[nodiscard]] bool is_test(const std::string& rel) {
    return rel.rfind("tests/", 0) == 0;
}

[[nodiscard]] bool allowed(const std::string& rel) {
    if (is_test(rel)) return true;
    for (const auto& a : kAllowed)
        if (rel == a) return true;
    return false;
}

[[nodiscard]] fs::path repo_root() {
    // Walk up from the cwd to the checkout, same marker rule dirs uses.
    std::error_code ec;
    for (fs::path p = fs::current_path(ec); !p.empty(); p = p.parent_path()) {
        if (fs::exists(p / "cmake" / "AgenttySources.cmake", ec)) return p;
        if (p == p.parent_path()) break;
    }
    return {};
}

}  // namespace

TEST_CASE("every Layout and Spec is declared in the inventory") {
    const fs::path root = repo_root();
    REQUIRE(!root.empty(), "found the checkout root");

    // A brace-init of either type anywhere outside the inventory is a second
    // declaration. `= config::kFoo` aliases are fine — they name the one
    // declaration rather than copying it — so the pattern requires a brace.
    const std::regex decl(R"((scope::Layout|dirs::Spec)\s+\w+\s*\{)");

    std::vector<std::string> offenders;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(root, ec)) {
        if (!e.is_regular_file(ec)) continue;
        const std::string ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".hpp") continue;
        const std::string rel = fs::relative(e.path(), root, ec).string();
        if (rel.rfind("build", 0) == 0 || rel.rfind("third_party", 0) == 0)
            continue;
        if (allowed(rel)) continue;

        std::ifstream in(e.path());
        std::string line;
        int n = 0;
        while (std::getline(in, line)) {
            ++n;
            if (std::regex_search(line, decl))
                offenders.push_back(rel + ":" + std::to_string(n));
        }
    }

    for (const auto& o : offenders)
        MESSAGE("declares its own Layout/Spec: " << o);
    CHECK(offenders.empty(),
          "every Layout/Spec is declared in config/inventory.hpp");
}

// The inventory must agree with the accessors that shipped first. If
// kThreadsSpec resolved somewhere other than user_threads_dir(), `agentty
// config threads` would confidently print the wrong path — the exact
// failure this whole design is built to prevent.
TEST_CASE("write specs resolve where the real accessors do") {
    struct Pair { const char* name; fs::path actual; };
    const Pair pairs[] = {
        {"threads", agentty::util::user_threads_dir()},
        {"cache",   agentty::util::user_cache_dir()},
        {"logs",    agentty::util::user_logs_dir()},
    };
    for (const auto& p : pairs) {
        const cfg::Entry* e = cfg::find(p.name);
        REQUIRE(e != nullptr, "concern is in the inventory");
        REQUIRE(e->write != nullptr, "concern declares a write spec");
        const cfg::Report r = cfg::describe(*e);
        REQUIRE(r.write.has_value(), "report carries the write row");
        CHECK(r.write->error.empty(), "spec resolves");
        CHECK(r.write->path == p.actual.string(),
              "inventory path == the accessor's path");
    }
}

TEST_CASE("the read ladder is exactly what plan emits") {
    // No filtering happens between plan() and the report — that invariant is
    // what makes the printed ladder the real one. Native concerns get two
    // sources, portable ones six (project×3 ▷ user×3).
    const cfg::Entry* mcp = cfg::find("mcp");
    REQUIRE(mcp != nullptr, "mcp is in the inventory");
    const cfg::Report rm = cfg::describe(*mcp);
    for (const auto& row : rm.reads)
        CHECK(row.dialect == agentty::scope::Dialect::Agentty,
              "mcp reads only the native dialect");
    // And it reports the two it skips, derived from the same field.
    CHECK(rm.skipped.size() == 2, "mcp names .agents and .claude as not read");

    const cfg::Entry* sk = cfg::find("skills");
    REQUIRE(sk != nullptr, "skills is in the inventory");
    CHECK(cfg::describe(*sk).skipped.empty(),
          "a portable concern skips nothing");
}

TEST_CASE("anchors are derived, not tagged") {
    // is_anchor() is a function of the Root, so adding a user-root category
    // cannot accidentally advertise itself as a second anchor.
    for (const cfg::Entry& e : cfg::inventory()) {
        if (!e.write) continue;
        const bool anchor = cfg::is_anchor(*e.write);
        CHECK(anchor == (e.write->root == agentty::dirs::Root::Project),
              "only the project root's variable is an anchor");
        if (!anchor)
            CHECK(cfg::anchor_env(e.write->root) == "AGENTTY_HOME",
                  "user-root leaves point at the one user anchor");
    }
}

TEST_CASE("only executable concerns report trust") {
    // The column exists because MCP's gate is now scope::trust_of's — the
    // report calls the same function the spawn path calls. Nothing else in
    // the inventory executes, so nothing else may claim a trust state;
    // printing "trusted" next to a skills directory would be meaningless
    // ceremony that erodes the word where it matters.
    for (const cfg::Entry& e : cfg::inventory()) {
        if (!e.read) continue;
        const cfg::Report r = cfg::describe(e);
        for (const auto& row : r.reads) {
            if (cfg::executable(e.name)) {
                // Present executable config always gets a verdict; a missing
                // file has no bytes to bind an approval to.
                CHECK(row.exists == !row.trust.empty(),
                      "executable: a verdict exactly when the file exists");
            } else {
                CHECK(row.trust.empty(),
                      "non-executable concerns claim no trust state");
            }
        }
    }
    CHECK(cfg::executable("mcp"), "mcp spawns processes");
    CHECK(!cfg::executable("skills"), "skills are data");
    CHECK(!cfg::executable("memory"), "memory is data");
}

TEST_CASE("user and explicit sources are trusted by placement") {
    // scope::trust_of's rule, surfaced: a human put ~/.agentty/mcp.json
    // there, so it needs no vouching. Only Project/Local rode in on a clone.
    const cfg::Entry* mcp = cfg::find("mcp");
    REQUIRE(mcp != nullptr, "mcp is in the inventory");
    for (const auto& row : cfg::describe(*mcp).reads) {
        if (!row.exists) continue;
        if (row.locus == agentty::scope::Locus::User
            || row.locus == agentty::scope::Locus::Explicit)
            CHECK(row.trust == "trusted",
                  "a human placed it, so it is trusted");
    }
}

TEST_CASE("every concern has a name and a description") {
    // The table is the first thing a confused user sees; a blank cell there
    // is a worse bug than a missing feature.
    for (const cfg::Entry& e : cfg::inventory()) {
        CHECK(!e.name.empty(), "concern has a name");
        CHECK(!e.what.empty(), "concern has a description");
        CHECK((e.read != nullptr || e.write != nullptr),
              "concern does at least one of read/write");
        CHECK(cfg::find(e.name) == &e, "find() returns the same row");
    }
    CHECK(cfg::find("nope") == nullptr, "unknown concern is not found");
}
