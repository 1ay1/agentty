// agents_md_test — locks the wire::agents_md_block helper that reads the
// AAIF-stewarded AGENTS.md standard file (https://agents.md) and wraps it
// in a dedicated <agents-md> system-prompt block, separate from the
// existing CLAUDE.md <memory> block.
//
// Per the published spec, AGENTS.md is project-scoped only (no user tier,
// no local tier, no nested monorepo walk inside agentty's single-tier
// workspace model). The file lives at <project_root>/AGENTS.md and is
// capped at 64 KiB by the shared wire::read_capped_file pipeline.
//
// Locks:
//   • missing file → empty block (elided from the prompt)
//   • present file → wrapped as <agents-md>…</agents-md> with intro line
//   • >64 KiB body truncated to 64 KiB (same cap as CLAUDE.md)
//   • path resolved from util::project_root(), not the raw process cwd,
//     so --workspace overrides are honored

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "agentty/provider/msg_shared.hpp"
#include "agentty/tool/util/fs_helpers.hpp"

namespace fs = std::filesystem;
namespace wire = agentty::provider::wire;
using agentty::tools::util::project_root;
using agentty::tools::util::set_workspace_root;

static int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static fs::path make_workspace() {
    const fs::path tmp =
        fs::temp_directory_path() / "agentty_agents_md_test_workspace";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    return tmp;
}

static void write_file(const fs::path& p, std::string_view content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << std::string(content);
}

static void test_missing_returns_empty() {
    const auto ws = make_workspace();
    set_workspace_root(ws);
    fs::current_path(ws);
    const std::string r = wire::agents_md_block("intro", project_root());
    CHECK(r.empty());
    fs::remove_all(ws);
}

static void test_wraps_content() {
    const auto ws = make_workspace();
    set_workspace_root(ws);
    fs::current_path(ws);
    write_file(ws / "AGENTS.md",
               "## Build\n- cmake --build build\n## Tests\n- ctest\n");
    const std::string r =
        wire::agents_md_block("Standard project guidance.", project_root());
    CHECK(!r.empty());
    CHECK(r.find("<agents-md>") != std::string::npos);
    CHECK(r.find("</agents-md>") != std::string::npos);
    CHECK(r.find("Standard project guidance.") != std::string::npos);
    CHECK(r.find("## Build") != std::string::npos);
    CHECK(r.find("cmake --build build") != std::string::npos);
    CHECK(r.find("## Tests") != std::string::npos);
    CHECK(r.find("ctest") != std::string::npos);
    fs::remove_all(ws);
}

static void test_truncates_at_cap() {
    const auto ws = make_workspace();
    set_workspace_root(ws);
    fs::current_path(ws);
    // 64 KiB + 1000 bytes — must be clipped to the 64 KiB cap that
    // wire::read_capped_file enforces (same cap as CLAUDE.md).
    std::string huge(64u * 1024u + 1000u, 'x');
    write_file(ws / "AGENTS.md", huge);
    // Use a unique intro marker so we can isolate the file-content portion
    // (intro + content + closing tag together would otherwise inflate the
    // measured size past the cap, masking whether the cap was applied).
    // The helper inserts exactly one '\n' between intro and content, so an
    // intro WITHOUT a trailing '\n' makes the offset arithmetic clean.
    const std::string intro = "INTRO_MARKER_FOR_TRUNCATION";
    const std::string r = wire::agents_md_block(intro, project_root());
    const auto intro_pos = r.find(intro);
    const auto close_pos = r.find("\n</agents-md>");
    CHECK(intro_pos != std::string::npos);
    CHECK(close_pos != std::string::npos);
    // +1 to skip the single '\n' the helper inserts between intro and content.
    const auto content_start = intro_pos + intro.size() + 1;
    const auto body = r.substr(content_start, close_pos - content_start);
    CHECK(body.size() <= 64u * 1024u);
    fs::remove_all(ws);
}

static void test_uses_project_root_not_process_cwd() {
    // Set workspace_root to ws, but chdir into an "other" dir that also
    // has an AGENTS.md. project_root() returns the workspace root here
    // (cwd outside the boundary falls back to the boundary per mcp-cpp's
    // project_root contract), so the ws AGENTS.md is read and the "other"
    // one is NOT.
    const auto ws    = make_workspace();
    const auto other = fs::temp_directory_path() / "agentty_agents_md_test_other";
    fs::remove_all(other);
    fs::create_directories(other);
    set_workspace_root(ws);
    write_file(ws / "AGENTS.md",    "workspace-root-AGENTS");
    write_file(other / "AGENTS.md", "should-not-be-read");
    // chdir OUTSIDE the workspace boundary so project_root() falls back to ws.
    fs::current_path(other);
    const std::string r = wire::agents_md_block("intro", project_root());
    CHECK(r.find("workspace-root-AGENTS") != std::string::npos);
    CHECK(r.find("should-not-be-read") == std::string::npos);
    fs::current_path(fs::temp_directory_path());
    fs::remove_all(ws);
    fs::remove_all(other);
}

int main() {
    test_missing_returns_empty();
    test_wraps_content();
    test_truncates_at_cap();
    test_uses_project_root_not_process_cwd();
    if (g_failures == 0) {
        std::printf("agents_md_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "agents_md_test: %d check(s) failed\n", g_failures);
    return 1;
}
