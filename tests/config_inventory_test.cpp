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
#include <map>
#include <regex>
#include <string>
#include <vector>

#include <unistd.h>   // geteuid — root can read anything, so skip there

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

TEST_CASE("anchors are derived, and honest") {
    // The inventory must not claim a variable moves more than it does.
    // $AGENTTY_RAG_DIR relocates the retrieval indexes and the feedback TSV
    // and NOTHING else -- memory.jsonl stays at <project>/.agentty -- so the
    // project root has no anchor, and saying otherwise would be exactly the
    // confident wrong answer this file exists to prevent.
    CHECK(cfg::anchor_env(agentty::dirs::Root::User) == "AGENTTY_HOME",
          "the user root has an anchor");
    CHECK(cfg::anchor_env(agentty::dirs::Root::Project).empty(),
          "the project root does NOT (that gap is the point)");

    for (const cfg::Entry& e : cfg::inventory()) {
        if (!e.write) continue;
        CHECK(!cfg::moves_whole_root(*e.write),
              "no Spec's own variable relocates a whole root");
    }

    // A concrete proof of the asymmetry: rag and memory share the project
    // root, and only one of them follows $AGENTTY_RAG_DIR.
    const cfg::Entry* rag = cfg::find("rag");
    const cfg::Entry* mem = cfg::find("memory");
    REQUIRE(rag != nullptr, "rag is in the inventory");
    REQUIRE(mem != nullptr, "memory is in the inventory");
    REQUIRE(rag->write != nullptr, "rag writes");
    CHECK(rag->write->env == "AGENTTY_RAG_DIR", "rag follows the variable");
    CHECK(mem->write == nullptr,
          "memory has no write spec -- it resolves through scope, so the"
          " variable cannot reach it");
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

TEST_CASE("sizes are measured once, never double-counted") {
    // The size column is what #58 actually needed: "store all
    // non-configuration data on a different path" is a decision nobody can
    // make without seeing which categories are big. Settings are KB and
    // threads can be GB, and until this existed the only way to find out was
    // `du`.
    //
    // The trap is specs that SHARE a resolved directory -- rag and feedback
    // both land on <project>/.agentty. If each measured the tree, the column
    // would sum to double the real disk, and a total that overstates by 2x is
    // worse than no total at all.
    std::map<std::string, int> measured_per_path;
    for (const cfg::Entry& e : cfg::inventory()) {
        if (!e.write) continue;
        const cfg::Report r = cfg::describe(e);
        REQUIRE(r.write.has_value(), "a write spec yields a write row");
        if (!r.write->error.empty()) continue;

        // measured and shared_dir are mutually exclusive by construction:
        // either this row owns the number or it defers to a sibling.
        CHECK(!(r.write->measured && r.write->shared_dir),
              "a row either owns its size or defers, never both");
        if (r.write->measured) ++measured_per_path[r.write->path];
        if (r.write->shared_dir)
            CHECK(r.write->bytes == 0,
                  "a deferring row reports no bytes of its own");
    }
    for (const auto& [path, n] : measured_per_path)
        CHECK(n == 1, "each directory is measured by exactly one concern");
}

TEST_CASE("a concern that writes nothing claims no disk") {
    // Read-only concerns (mcp, skills, memory...) resolve through scope, not
    // dirs. Reporting a size for them would invent a number.
    for (const cfg::Entry& e : cfg::inventory()) {
        if (e.write) continue;
        CHECK(!cfg::describe(e).write.has_value(),
              "no write spec, no write row, no size");
    }
}

TEST_CASE("an unreadable subtree is flagged, not silently dropped") {
    // skip_permission_denied makes a chmod-000 directory vanish SILENTLY --
    // no error_code, nothing inside it ever appears. A 2 MB subdir reported
    // as "0 B" with no hint anything was missed, which is the same confident
    // lie the rest of this file exists to prevent. Partial is fine; silent
    // is not.
    //
    // Root can read anything, so this would pass vacuously there.
    if (::geteuid() == 0) return;

    const fs::path base = fs::temp_directory_path()
                        / "agentty_cfg_partial";
    fs::remove_all(base);
    const fs::path home = base / "home";
    const fs::path locked = home / "threads" / "locked";
    fs::create_directories(locked);
    { std::ofstream f(locked / "big.bin"); f << std::string(200000, 'x'); }

    const std::string prev = std::getenv("AGENTTY_HOME")
                                 ? std::getenv("AGENTTY_HOME") : "";
    const bool had = std::getenv("AGENTTY_HOME") != nullptr;
    ::setenv("AGENTTY_HOME", home.string().c_str(), 1);

    const cfg::Entry* threads = cfg::find("threads");
    REQUIRE(threads != nullptr, "threads is in the inventory");

    // Readable: the bytes are counted and nothing is flagged.
    {
        const cfg::Report r = cfg::describe(*threads);
        REQUIRE(r.write.has_value(), "write row");
        CHECK(r.write->bytes >= 200000, "the file is counted");
        CHECK(!r.write->partial, "nothing was unreadable");
    }

    // Unreadable: the count drops to 0, and that MUST be announced.
    fs::permissions(locked, fs::perms::none);
    {
        const cfg::Report r = cfg::describe(*threads);
        REQUIRE(r.write.has_value(), "write row");
        CHECK(r.write->partial,
              "an unreadable subtree is reported as partial");
    }
    fs::permissions(locked, fs::perms::owner_all);

    if (had) ::setenv("AGENTTY_HOME", prev.c_str(), 1);
    else     ::unsetenv("AGENTTY_HOME");
    fs::remove_all(base);
}

TEST_CASE("a symlink loop in a measured tree terminates") {
    // Sizes must never follow symlinks: a link into someone else's tree
    // would report their disk as ours, and a cycle would hang the command.
    const fs::path base = fs::temp_directory_path() / "agentty_cfg_loop";
    fs::remove_all(base);
    const fs::path home = base / "home";
    fs::create_directories(home / "threads");
    { std::ofstream f(home / "threads" / "a.txt"); f << "hello"; }
    std::error_code ec;
    fs::create_directory_symlink("..", home / "threads" / "self", ec);

    const std::string prev = std::getenv("AGENTTY_HOME")
                                 ? std::getenv("AGENTTY_HOME") : "";
    const bool had = std::getenv("AGENTTY_HOME") != nullptr;
    ::setenv("AGENTTY_HOME", home.string().c_str(), 1);

    const cfg::Entry* threads = cfg::find("threads");
    REQUIRE(threads != nullptr, "threads is in the inventory");
    const cfg::Report r = cfg::describe(*threads);   // must return at all
    REQUIRE(r.write.has_value(), "write row");
    CHECK(r.write->bytes == 5, "counts the file once, does not follow the loop");

    if (had) ::setenv("AGENTTY_HOME", prev.c_str(), 1);
    else     ::unsetenv("AGENTTY_HOME");
    fs::remove_all(base);
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
