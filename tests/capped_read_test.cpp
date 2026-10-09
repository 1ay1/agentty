// capped_read_test — the one capped-read primitive, and the rule that there
// is only one.
//
// THE RULE
// ========
// A file read with a size limit goes through util::capped_read. No file
// writes its own.
//
// This exists because the same twelve-line helper was written four times,
// and all four shipped the same bug: they returned "" for BOTH "no such
// file" and "file is over the cap", so every caller's `if (raw.empty())
// continue;` silently discarded the second case. What that cost:
//
//   skills    an oversized SKILL.md vanished with `0 warning(s)`
//   hooks     `agentty hooks` said "no hooks file" with the file right
//             there, and a blocking PreToolUse hook was inert
//   AGENTS.md cut at 64 KiB with no marker, so a rule near the bottom
//             never reached the model
//
// Three separate finds, three separate fixes, one missing type. jaal's first
// principle is "contracts are types" -- so the fix is a variant whose TooBig
// alternative cannot be reached through an empty string, and this test keeps
// the fourth copy from being written.

#include "agtest.hpp"

#include "agentty/util/capped_read.hpp"

#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <process.h>
#  define getpid _getpid
#else
#  include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace u  = agentty::util;

namespace {

[[nodiscard]] fs::path sandbox() {
    // Per process: ctest runs each case as its own process in parallel, and
    // a shared directory let one case's remove_all delete another's files.
    const auto d = fs::temp_directory_path()
        / ("agentty_capped_read_test." + std::to_string(::getpid()));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

void write_file(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << body;
}

[[nodiscard]] fs::path repo_root() {
    std::error_code ec;
    for (fs::path p = fs::current_path(ec); !p.empty(); p = p.parent_path()) {
        if (fs::exists(p / "cmake" / "AgenttySources.cmake", ec)) return p;
        if (p == p.parent_path()) break;
    }
    return {};
}

}  // namespace

TEST_CASE("capped_read: the four outcomes are distinct") {
    const auto dir = sandbox();

    // Content.
    write_file(dir / "ok.txt", "hello");
    const auto ok = u::capped_read(dir / "ok.txt", 1024);
    CHECK(std::holds_alternative<u::Content>(ok));
    CHECK(u::bytes_or_empty(ok) == "hello");

    // Absent -- and a MISSING file is Absent, not an error.
    CHECK(std::holds_alternative<u::Absent>(
        u::capped_read(dir / "nope.txt", 1024)));

    // TooBig carries the real size, so a caller can say how far over.
    write_file(dir / "big.txt", std::string(2048, 'x'));
    const auto big = u::capped_read(dir / "big.txt", 1024);
    REQUIRE(std::holds_alternative<u::TooBig>(big));
    CHECK(std::get<u::TooBig>(big).size == 2048);
    CHECK(u::oversize_bytes(big) == 2048);

    // THE bug, as an assertion: TooBig and Absent must not look alike.
    // Every one of the four shipped helpers failed exactly here.
    CHECK(u::capped_read(dir / "big.txt", 1024).index()
          != u::capped_read(dir / "nope.txt", 1024).index());

    fs::remove_all(dir);
}

TEST_CASE("capped_read: a directory is Absent, never a crash") {
    // std::ifstream OPENS a directory successfully on Linux and throws out
    // of the first read. That aborted agentty once, from `mkdir
    // .agentty/mcp.json`, with a message naming neither agentty nor the
    // path. The is_regular_file check in front is what makes it impossible.
    const auto dir = sandbox();
    fs::create_directories(dir / "shaped-like-a-file.json");
    CHECK(std::holds_alternative<u::Absent>(
        u::capped_read(dir / "shaped-like-a-file.json", 1024)));
    fs::remove_all(dir);
}

TEST_CASE("capped_read: an empty file is Content, not Absent") {
    // "The user wrote an empty hooks.json" and "there is no hooks.json" are
    // different facts. A caller may treat them alike -- explicitly.
    const auto dir = sandbox();
    write_file(dir / "empty.txt", "");
    const auto got = u::capped_read(dir / "empty.txt", 1024);
    CHECK(std::holds_alternative<u::Content>(got));
    CHECK(u::bytes_or_empty(got).empty());
    fs::remove_all(dir);
}

TEST_CASE("capped_read: the note explains only what needs explaining") {
    const auto dir = sandbox();
    write_file(dir / "big.txt", std::string(70u * 1024u, 'x'));
    write_file(dir / "ok.txt", "small");

    const auto note = u::capped_note(u::capped_read(dir / "big.txt", 64u * 1024u),
                                     64u * 1024u);
    CHECK(note.find("70 KB") != std::string::npos);
    CHECK(note.find("64 KB") != std::string::npos);
    CHECK(note.find("NOT loaded") != std::string::npos);

    // Content and Absent say nothing -- neither needs it.
    CHECK(u::capped_note(u::capped_read(dir / "ok.txt", 1024), 1024).empty());
    CHECK(u::capped_note(u::capped_read(dir / "nope", 1024), 1024).empty());
    fs::remove_all(dir);
}

TEST_CASE("no file writes its own capped reader") {
    // The discipline half. Four copies of this helper existed and all four
    // were wrong the same way; catching the fifth at review is exactly what
    // did not happen the first four times.
    const fs::path root = repo_root();
    REQUIRE(!root.empty(), "found the checkout root");

    // A local read helper that takes a cap. Matches the shape every copy
    // had: `read_capped(path, cap)` / `read_all(path, cap)`.
    const std::regex decl(
        R"((read_capped|read_all)\s*\([^)]*\b(cap|kMax\w*Bytes)\b)");

    std::vector<std::string> offenders;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(root / "src", ec)) {
        if (!e.is_regular_file(ec)) continue;
        if (e.path().extension() != ".cpp") continue;
        const std::string rel = fs::relative(e.path(), root, ec).string();

        std::ifstream in(e.path());
        std::string line;
        int n = 0;
        while (std::getline(in, line)) {
            ++n;
            // Only DEFINITIONS, not calls into the shared one.
            if (line.find("util::capped_read") != std::string::npos) continue;
            if (line.find("//") == 0) continue;
            if (std::regex_search(line, decl)
                && line.find("[[nodiscard]]") != std::string::npos)
                offenders.push_back(rel + ":" + std::to_string(n));
        }
    }

    for (const auto& o : offenders)
        MESSAGE("defines its own capped reader: " << o);
    CHECK(offenders.empty(),
          "capped reads go through util::capped_read");
}
