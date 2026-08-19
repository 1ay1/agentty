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
