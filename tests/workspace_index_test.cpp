// SPDX-License-Identifier: Apache-2.0
//
// workspace_index_test.cpp — locks the composer @/# picker logic: the
// fuzzy file filter, frecency ranking (recently-referenced files float to
// the top), and the non-blocking ready-probe contract. The disk walk
// itself isn't unit-tested (it needs a real tree); we test the pure
// filter + frecency layer that runs on every keystroke.

#include "agentty/workspace/files.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <thread>
#include <vector>

using agentty::filter_files;
using agentty::note_file_referenced;

TEST_CASE("filter_files: fuzzy subsequence match") {
    std::vector<std::string> files = {
        "src/runtime/app/update/composer.cpp",
        "src/workspace/files.cpp",
        "include/agentty/workspace/files.hpp",
        "README.md",
    };

    // Empty query returns everything.
    CHECK(filter_files(files, "").size() == files.size());

    // "files" matches both files.cpp and files.hpp, not composer/README.
    auto m = filter_files(files, "files");
    CHECK(m.size() == 2);
    for (auto i : m) CHECK(files[i].find("files.") != std::string::npos);

    // Subsequence across separators: "wsfiles" hits workspace/files.
    auto m2 = filter_files(files, "wsfiles");
    CHECK(!m2.empty());
    CHECK(files[m2[0]].find("workspace/files") != std::string::npos);

    // No match → empty.
    CHECK(filter_files(files, "zzzznotathing").empty());
}

TEST_CASE("filter_files: frecency floats referenced files to the top") {
    std::vector<std::string> files = {
        "a/alpha.cpp",
        "b/beta.cpp",
        "c/gamma.cpp",
    };

    // No frecency yet: empty-query order is the input (alphabetic) order.
    {
        auto m = filter_files(files, "");
        REQUIRE(m.size() == 3);
        CHECK(files[m[0]] == "a/alpha.cpp");
    }

    // Reference gamma, then beta. beta is most-recent → ranks first,
    // gamma second, alpha (never referenced) last.
    note_file_referenced("c/gamma.cpp");
    note_file_referenced("b/beta.cpp");
    {
        auto m = filter_files(files, "");
        REQUIRE(m.size() == 3);
        CHECK(files[m[0]] == "b/beta.cpp");    // most recent
        CHECK(files[m[1]] == "c/gamma.cpp");   // next
        CHECK(files[m[2]] == "a/alpha.cpp");   // never referenced
    }

    // Re-referencing gamma moves it back to the front (dedupe + hoist).
    note_file_referenced("c/gamma.cpp");
    {
        auto m = filter_files(files, "");
        REQUIRE(m.size() == 3);
        CHECK(files[m[0]] == "c/gamma.cpp");
    }

    // Frecency also breaks ties within a fuzzy query: both *.cpp match
    // ".cpp"; the recently-referenced one should rank ahead.
    {
        auto m = filter_files(files, "cpp");
        REQUIRE(m.size() == 3);
        // gamma was referenced last → first among the .cpp matches.
        CHECK(files[m[0]] == "c/gamma.cpp");
    }
}

#include "agentty/tool/util/fs_helpers.hpp"
#include "agentty/workspace/symbols.hpp"
#include "agentty/tool/util/subprocess.hpp"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

// The load-bearing "smart" behaviour: a git-DIRTY file outranks everything
// else in a blank `@`, even a frecency entry — because the file you're
// editing right now is almost always the one you want. Builds a real tiny
// git repo so build_git_signals() (git status --porcelain) has something
// to read.
TEST_CASE("filter_files: git-dirty files lead the working set") {
    namespace fs = std::filesystem;
    auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path repo = fs::temp_directory_path() /
                    ("agentty_gitrank_" + std::to_string(nonce));
    fs::create_directories(repo);
    auto sh = [&](const std::string& c) {
        (void)agentty::tools::util::run_command_s(
            "cd " + repo.string() + " && " + c, 8192, std::chrono::seconds{15});
    };
    auto write = [&](const std::string& rel, const std::string& body) {
        std::ofstream f(repo / rel); f << body;
    };

    sh("git init -q && git config user.email t@t && git config user.name t");
    write("clean_a.cpp", "int a() { return 1; }\n");
    write("clean_b.cpp", "int b() { return 2; }\n");
    write("hot.cpp",     "int hot() { return 3; }\n");
    sh("git add -A && git commit -qm init");
    // Now dirty ONE file after the commit.
    write("hot.cpp", "int hot() { return 3; } // edited\n");

    // Point project_root at the fixture and rebuild the git signal map
    // synchronously (refresh_git_signals reads git status against the
    // current project root — deterministic, no prewarm-ordering race).
    auto prev = fs::current_path();
    fs::current_path(repo);
    agentty::tools::util::set_workspace_root(repo);
    agentty::refresh_git_signals();

    std::vector<std::string> files = {
        "clean_a.cpp", "clean_b.cpp", "hot.cpp",
    };
    auto m = filter_files(files, "");
    REQUIRE(m.size() == 3);
    // hot.cpp is the only modified file → it must lead, ahead of the
    // alphabetically-earlier clean_a/clean_b.
    CHECK(files[m[0]] == "hot.cpp");
    CHECK(agentty::file_git_tag("hot.cpp") == agentty::GitTag::Modified);

    fs::current_path(prev);
    std::error_code ec;
    fs::remove_all(repo, ec);
}

TEST_CASE("prewarm walk bails promptly when cancelled") {
    namespace fs = std::filesystem;
    // A directory with enough files that an uncancelled full scan is
    // measurable, so a prompt return proves the cancel actually short-circuits.
    auto root = fs::temp_directory_path() /
        ("agentty_prewarm_cancel_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    for (int i = 0; i < 3000; ++i) {
        std::ofstream(root / ("f" + std::to_string(i) + ".cpp"))
            << "int f" << i << "(){return " << i << ";}\n";
    }
    auto prev = fs::current_path();
    fs::current_path(root);
    agentty::tools::util::set_workspace_root(root);

    // Request cancel BEFORE kicking the walk: the loop's first cancel check
    // fires and it bails almost immediately. join must not scan all 3000 files.
    agentty::request_prewarm_cancel();
    CHECK(agentty::prewarm_cancelled());

    const auto t0 = std::chrono::steady_clock::now();
    agentty::prewarm_workspace_files();
    agentty::prewarm_workspace_symbols();
    agentty::join_workspace_prewarm();
    agentty::join_workspace_symbols_prewarm();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 1000);   // generous; a full 3000-file scan is far slower

    fs::current_path(prev);
    std::error_code ec;
    fs::remove_all(root, ec);
}
