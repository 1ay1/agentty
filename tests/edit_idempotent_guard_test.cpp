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
