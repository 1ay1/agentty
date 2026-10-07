// edit_idempotent_guard_test — a mistyped old_text must not read as success.
//
// ── WHY ──────────────────────────────────────────────────────────────────
//
// `edit` has an "already applied" shortcut: if old_text is gone and new_text
// is present, a re-run of an edit that already landed reports success instead
// of a confusing no-match. That is worth having.
//
// The signal was too weak. It accepted new_text occurring ANYWHERE, with a
// floor of 8 bytes. So a MISTYPED old_text whose new_text happens to be a
// common line -- `return false;`, `    });`, `#include <string>` -- found its
// new_text elsewhere in the file and the tool answered "identical content
// (file unchanged on disk)". Nothing was written, the call succeeded, and the
// model carried on building on a change that never happened.
//
// That is the worst shape of bug this tool can have short of corruption: not
// a wrong edit, a believed one.
#include "agtest.hpp"

#include "agentty/tool/registry.hpp"
#include "agentty/tool/tool.hpp"
#include "agentty/tool/util/fs_helpers.hpp"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <string>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

// A file inside the workspace root, since the fs tools refuse anything else.
// NOT under .agentty/ -- the handoff gate treats that as host-trusted (hooks
// run from there) and denies the write, which is correct of it.
fs::path scratch(const char* tag) {
    auto dir = agentty::tools::util::workspace_root() / "build" / "test-scratch";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir / ("edit_guard_" + std::string{tag} + ".txt");
}

void put(const fs::path& p, std::string_view body) {
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    o.write(body.data(), static_cast<std::streamsize>(body.size()));
}

std::string get(const fs::path& p) {
    std::ifstream i(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(i)),
                       std::istreambuf_iterator<char>());
}

agentty::tools::ExecResult do_edit(const fs::path& p,
                                   std::string old_text, std::string new_text) {
    return agentty::tool::DynamicDispatch::execute(
        "edit", json{{"path", p.string()},
                     {"edits", json::array({json{{"old_text", std::move(old_text)},
                                                 {"new_text", std::move(new_text)}}})}});
}

} // namespace

TEST_CASE("edit guard: a mistyped old_text does not pass as already-applied") {
    // `    return false;` appears TWICE at the same indentation, so it is not
    // a uniquely-placed new_text. The model asks to replace a line that isn't
    // in the file. Before the fix, presence anywhere was enough: this
    // reported "already present, move on" and wrote nothing.
    const auto p = scratch("mistyped");
    put(p,
        "int a() {\n"
        "    return false;\n"
        "}\n"
        "int b() {\n"
        "    return false;\n"
        "}\n");
    const auto before = get(p);

    auto r = do_edit(p, "    int totally_absent_line = 7;\n",
                        "    return false;\n");

    CHECK_MESSAGE(!r.has_value(),
                  "old_text is absent and new_text is NOT uniquely placed, so "
                  "this has to be a no-match error, not a success");
    CHECK_MESSAGE(get(p) == before, "and nothing may be written either way");
    fs::remove(p);
}

TEST_CASE("edit guard: a genuine retry still reports already-applied") {
    // The case the shortcut exists for: the edit landed, the model retries
    // the same call. new_text is distinctive and present exactly once.
    const auto p = scratch("retry");
    put(p,
        "int a() {\n"
        "    return compute_the_answer(42);\n"
        "}\n");
    const auto before = get(p);

    auto r = do_edit(p, "    return compute(41);\n",
                        "    return compute_the_answer(42);\n");

    CHECK_MESSAGE(r.has_value(),
                  "old_text gone, new_text present exactly once -- a retry of "
                  "an edit that already landed");
    if (r) CHECK(r->text.find("No write needed") != std::string::npos);
    CHECK(get(p) == before);
    fs::remove(p);
}

TEST_CASE("edit guard: a normal edit still applies") {
    const auto p = scratch("normal");
    put(p, "int a() {\n    return 1;\n}\n");

    auto r = do_edit(p, "    return 1;\n", "    return 2;\n");

    REQUIRE(r.has_value());
    CHECK(get(p).find("return 2;") != std::string::npos);
    CHECK(get(p).find("return 1;") == std::string::npos);
    fs::remove(p);
}

TEST_CASE("edit guard: the no-op message does not tell the model to move on") {
    // It is a heuristic, so it must report what was OBSERVED and say how to
    // check. The old wording asserted "the desired state is already in place;
    // move on", which is exactly what a mistyped old_text should not be told.
    const auto p = scratch("message");
    put(p, "int a() {\n    return compute_the_answer(42);\n}\n");

    auto r = do_edit(p, "    return compute(41);\n",
                        "    return compute_the_answer(42);\n");
    REQUIRE(r.has_value());
    CHECK(r->text.find("read the file back") != std::string::npos);
    CHECK_MESSAGE(r->text.find("move on") == std::string::npos,
                  "never instruct the model to proceed on a guess");
    fs::remove(p);
}

// ── editing through a symlink ───────────────────────────────────────────
//
// The publish step is rename(), which replaces the NAME it is given. For a
// symlink that is the link itself, so an edit through one did three wrong
// things at once, silently:
//
//   * the symlink became a regular file (the link is destroyed),
//   * the real target kept its old contents (the edit went nowhere), and
//   * the tool reported success, with a diff of the change.
//
// Measured, not reasoned: I made a link, edited through it, and found the
// link gone and the target untouched. Dotfiles repos and symlinked generated
// files (compile_commands.json) hit this on the first edit.

TEST_CASE("edit through a symlink writes the target and keeps the link") {
    const auto dir = agentty::tools::util::workspace_root() / "build" / "test-scratch";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const auto target = dir / "symlink_target.txt";
    const auto link   = dir / "symlink_alias.txt";

    fs::remove(link, ec);
    fs::remove(target, ec);
    put(target, "alpha\nbeta\n");
    fs::create_symlink(target.filename(), link, ec);
    if (ec) return;   // no symlink support on this host (Windows w/o privilege)
    REQUIRE(fs::is_symlink(link, ec));

    auto r = agentty::tool::DynamicDispatch::execute(
        "edit", json{{"path", link.string()},
                     {"edits", json::array({json{{"old_text", "beta"},
                                                 {"new_text", "BETA"}}})}});
    REQUIRE(r.has_value());

    CHECK_MESSAGE(fs::is_symlink(link, ec),
                  "the link must survive the edit -- replacing it with a "
                  "regular file silently breaks whatever pointed at it");
    CHECK_MESSAGE(get(target).find("BETA") != std::string::npos,
                  "and the edit has to land in the TARGET, not in a new inode "
                  "nobody is looking at");
    CHECK(get(target).find("beta") == std::string::npos);

    fs::remove(link, ec);
    fs::remove(target, ec);
}

// ── the `line:` hint ────────────────────────────────────────────────────
//
// `line:` is documented as a DISAMBIGUATOR, and as a hard filter it would do
// harm: line numbers go stale constantly (an earlier edit grew the file), and
// refusing a correct edit over a stale number is worse than ignoring it.
//
// But it used to be discarded entirely whenever the DP produced exactly one
// candidate -- precisely the case with no other evidence. A lone fuzzy match
// 900 lines from where the caller said to look was applied silently.
//
// The rule: the hint binds only where we are ALREADY guessing.
//
//   exact match, any distance     -> apply (returns before the hint is read)
//   fuzzy, near the hint          -> apply
//   fuzzy, no hint given          -> apply (nothing was claimed)
//   fuzzy, far from a given hint  -> refuse, and name the line it found

namespace {

// A file with a distinctive line far from the top, plus filler.
std::string spread_file(const char* needle_line, int at_line) {
    std::string s;
    for (int i = 1; i < at_line; ++i)
        s += "    int filler_" + std::to_string(i) + " = 0;\n";
    s += needle_line;
    s += "\n";
    for (int i = 0; i < 40; ++i) s += "    int tail = 1;\n";
    return s;
}

agentty::tools::ExecResult edit_at(const fs::path& p, std::string old_text,
                                   std::string new_text, int line) {
    json one{{"old_text", std::move(old_text)}, {"new_text", std::move(new_text)}};
    if (line > 0) one["line"] = line;
    return agentty::tool::DynamicDispatch::execute(
        "edit", json{{"path", p.string()}, {"edits", json::array({one})}});
}

} // namespace

TEST_CASE("line hint: an EXACT match is applied however stale the hint") {
    // The common case this must never break: the region moved, the caller's
    // remembered line number is wrong, but the text is exact.
    const auto p = scratch("hint_exact");
    put(p, spread_file("    const int target = 7;", 600));

    auto r = edit_at(p, "    const int target = 7;",
                        "    const int target = 8;", /*line=*/5);
    REQUIRE_MESSAGE(r.has_value(),
                    "an exact match must not be refused over a stale line");
    CHECK(get(p).find("target = 8;") != std::string::npos);
    fs::remove(p);
}

TEST_CASE("line hint: a FUZZY match far from the hint is refused, with the line") {
    // Only an approximate candidate, and it sits ~600 lines from where the
    // caller said. Weak on both axes at once -- the shape of a silently
    // wrong edit.
    const auto p = scratch("hint_far");
    put(p, spread_file("    const int target = 7;   // trailing note", 600));
    const auto before = get(p);

    auto r = edit_at(p, "    const int target = 7;   // trailing note!",
                        "    const int target = 8;", /*line=*/5);

    REQUIRE_FALSE(r.has_value());
    const auto& detail = r.error().detail;
    const bool names_the_line = detail.find("600") != std::string::npos
                             || detail.find("601") != std::string::npos;
    CHECK_MESSAGE(names_the_line,
                  "the refusal has to name where it DID find it -- 'no match' "
                  "would send the caller on a blind retry");
    CHECK_MESSAGE(get(p) == before, "and nothing is written");
    fs::remove(p);
}

TEST_CASE("line hint: a FUZZY match near the hint still applies") {
    const auto p = scratch("hint_near");
    put(p, spread_file("    const int target = 7;   // trailing note", 600));

    auto r = edit_at(p, "    const int target = 7;   // trailing note!",
                        "    const int target = 8;", /*line=*/595);
    REQUIRE_MESSAGE(r.has_value(),
                    "within tolerance the hint corroborates rather than blocks");
    CHECK(get(p).find("target = 8;") != std::string::npos);
    fs::remove(p);
}

TEST_CASE("line hint: with NO hint a lone fuzzy match still applies") {
    // Nothing was claimed, so there is nothing to contradict. Unchanged
    // behaviour -- the fix must not make hint-less edits stricter.
    const auto p = scratch("hint_none");
    put(p, spread_file("    const int target = 7;   // trailing note", 600));

    auto r = edit_at(p, "    const int target = 7;   // trailing note!",
                        "    const int target = 8;", /*line=*/0);
    REQUIRE(r.has_value());
    CHECK(get(p).find("target = 8;") != std::string::npos);
    fs::remove(p);
}
