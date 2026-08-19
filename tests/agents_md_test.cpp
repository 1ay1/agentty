// agents_md_test — locks the wire::agents_md_block helper that reads the
// AAIF-stewarded AGENTS.md standard file (https://agents.md) and wraps it
// in a dedicated <agents-md> system-prompt block, separate from the
// existing CLAUDE.md <memory> block.
//
// Per the published spec, AGENTS.md is project-scoped only (no user tier,
// no local tier). The root file lives at <workspace_root>/AGENTS.md and is
// capped at 64 KiB by the shared wire::read_capped_file pipeline.
//
// The spec also describes nested monorepo files: "Place another AGENTS.md
// inside each package. Agents automatically read the nearest file in the
// directory tree, so the closest one takes precedence." When search_from
// (the agent's cwd clamped inside the workspace) is a subdirectory of
// workspace_root, the helper walks upward looking for a second AGENTS.md
// and emits it in a separate <agents-md-package> block.
//
// Locks:
//   • missing file → empty block (elided from the prompt)
//   • present file → wrapped as <agents-md>…</agents-md> with intro line
//   • >64 KiB body truncated to 64 KiB (same cap as CLAUDE.md)
//   • path resolved from workspace_root(), not the raw process cwd,
//     so --workspace overrides are honored
//   • nested AGENTS.md in a subpackage → emitted in <agents-md-package>
//   • no nested file when search_from == workspace_root
//   • walk stops at workspace boundary (never reads outside)
//   • nested file also capped at 64 KiB
//   • dedup: same canonical path as root → no second block
//   • global AGENTS.md (~/.agentty/ or ~/.agents/) → <agents-md-global> block
//   • ~/.agentty/AGENTS.md wins over ~/.agents/AGENTS.md
//   • global-only (no root file) → only <agents-md-global>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>   // getpid (POSIX)

#include "agentty/provider/msg_shared.hpp"
#include "agentty/tool/util/fs_helpers.hpp"

namespace fs = std::filesystem;
namespace wire = agentty::provider::wire;
using agentty::tools::util::project_root;
using agentty::tools::util::set_workspace_root;
using agentty::tools::util::workspace_root;

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
    const std::string r = wire::agents_md_block("intro", workspace_root());
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
        wire::agents_md_block("Standard project guidance.", workspace_root());
    CHECK(!r.empty());
    CHECK(r.find("<agents-md>") != std::string::npos);
    CHECK(r.find("</agents-md>") != std::string::npos);
    CHECK(r.find("Standard project guidance.") != std::string::npos);
    CHECK(r.find("## Build") != std::string::npos);
    CHECK(r.find("cmake --build build") != std::string::npos);
    CHECK(r.find("## Tests") != std::string::npos);
    CHECK(r.find("ctest") != std::string::npos);
    // No nested file → no <agents-md-package> block.
    CHECK(r.find("<agents-md-package>") == std::string::npos);
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
    const std::string r = wire::agents_md_block(intro, workspace_root());
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
    const std::string r = wire::agents_md_block("intro", workspace_root(),
                                                   project_root());
    CHECK(r.find("workspace-root-AGENTS") != std::string::npos);
    CHECK(r.find("should-not-be-read") == std::string::npos);
    // search_from is outside workspace → dir != root_canon fails immediately
    // (project_root() falls back to workspace_root), so no nested block.
    CHECK(r.find("<agents-md-package>") == std::string::npos);
    fs::current_path(fs::temp_directory_path());
    fs::remove_all(ws);
    fs::remove_all(other);
}

// ── Nested monorepo walk tests ───────────────────────────────────────────

static void test_nested_overrides_root() {
    // Root AGENTS.md + a nested one in a subpackage. search_from is the
    // subpackage dir. The helper should emit BOTH blocks: root <agents-md>
    // and nested <agents-md-package>.
    const auto ws = make_workspace();
    set_workspace_root(ws);
    fs::current_path(ws);
    write_file(ws / "AGENTS.md", "root-level guidance");
    const auto pkg = ws / "packages" / "auth";
    fs::create_directories(pkg);
    write_file(pkg / "AGENTS.md", "auth-package guidance");
    // search_from = the subpackage directory (simulates agent working there)
    const std::string r = wire::agents_md_block("intro", workspace_root(),
                                                   pkg);
    CHECK(r.find("<agents-md>") != std::string::npos);
    CHECK(r.find("root-level guidance") != std::string::npos);
    CHECK(r.find("<agents-md-package>") != std::string::npos);
    CHECK(r.find("auth-package guidance") != std::string::npos);
    CHECK(r.find("</agents-md-package>") != std::string::npos);
    fs::remove_all(ws);
}

static void test_no_nested_when_at_root() {
    // search_from == workspace_root → no nested block, only root.
    const auto ws = make_workspace();
    set_workspace_root(ws);
    fs::current_path(ws);
    write_file(ws / "AGENTS.md", "root-only guidance");
    const std::string r = wire::agents_md_block("intro", workspace_root(),
                                                   workspace_root());
    CHECK(r.find("<agents-md>") != std::string::npos);
    CHECK(r.find("root-only guidance") != std::string::npos);
    CHECK(r.find("<agents-md-package>") == std::string::npos);
    fs::remove_all(ws);
}

static void test_walk_stops_at_workspace_boundary() {
    // AGENTS.md outside the workspace should never be read, even if
    // search_from is deep inside the workspace and the walk passes near
    // an external AGENTS.md.
    const auto ws = make_workspace();
    set_workspace_root(ws);
    fs::current_path(ws);
    write_file(ws / "AGENTS.md", "root guidance");
    // Create a nested dir but no AGENTS.md inside it.
    const auto pkg = ws / "packages" / "auth";
    fs::create_directories(pkg);
    // search_from = deep subpackage; walk goes up to ws, finds no nested
    // AGENTS.md, stops. No <agents-md-package> block.
    const std::string r = wire::agents_md_block("intro", workspace_root(),
                                                   pkg);
    CHECK(r.find("<agents-md>") != std::string::npos);
    CHECK(r.find("root guidance") != std::string::npos);
    CHECK(r.find("<agents-md-package>") == std::string::npos);
    fs::remove_all(ws);
}

static void test_nested_truncates_at_cap() {
    // Nested AGENTS.md is also capped at 64 KiB.
    const auto ws = make_workspace();
    set_workspace_root(ws);
    fs::current_path(ws);
    write_file(ws / "AGENTS.md", "root");
    const auto pkg = ws / "packages" / "auth";
    fs::create_directories(pkg);
    std::string huge(64u * 1024u + 1000u, 'x');
    write_file(pkg / "AGENTS.md", huge);
    const std::string r = wire::agents_md_block("intro", workspace_root(),
                                                   pkg);
    CHECK(r.find("<agents-md-package>") != std::string::npos);
    // Find the nested block content and verify it's capped.
    const auto pkg_start = r.find("<agents-md-package>");
    const auto pkg_end   = r.find("</agents-md-package>");
    CHECK(pkg_start != std::string::npos);
    CHECK(pkg_end   != std::string::npos);
    // Block content = everything between the opening tag line and the closing
    // tag. The opening tag includes an intro line + '\n', then the file
    // content. We just verify the block isn't larger than cap + overhead.
    const auto block_size = pkg_end - pkg_start;
    // 64 KiB content + ~300 bytes for tags + intro line.
    CHECK(block_size <= 64u * 1024u + 500u);
    fs::remove_all(ws);
}

static void test_dedup_when_same_file() {
    // When search_from resolves to workspace_root (same canonical path),
    // the walk would find the same AGENTS.md as the root file. The dedup
    // check (candidate_canon != root_agents) prevents a duplicate block.
    const auto ws = make_workspace();
    set_workspace_root(ws);
    fs::current_path(ws);
    write_file(ws / "AGENTS.md", "only-one-file");
    // search_from is a subdirectory that does NOT have its own AGENTS.md,
    // so the walk reaches workspace_root and the loop condition dir != root_canon
    // terminates it. No nested block.
    const auto sub = ws / "src";
    fs::create_directories(sub);
    const std::string r = wire::agents_md_block("intro", workspace_root(),
                                                   sub);
    CHECK(r.find("<agents-md>") != std::string::npos);
    CHECK(r.find("only-one-file") != std::string::npos);
    CHECK(r.find("<agents-md-package>") == std::string::npos);
    fs::remove_all(ws);
}

static void test_nearest_wins_not_deepest() {
    // Multiple nested AGENTS.md files in the path. The walk finds the
    // NEAREST one (closest to search_from), not the deepest or the root.
    const auto ws = make_workspace();
    set_workspace_root(ws);
    fs::current_path(ws);
    write_file(ws / "AGENTS.md", "root guidance");
    fs::create_directories(ws / "packages");
    write_file(ws / "packages" / "AGENTS.md", "packages-level guidance");
    const auto auth = ws / "packages" / "auth";
    fs::create_directories(auth);
    write_file(auth / "AGENTS.md", "auth-level guidance");
    // search_from = auth/src — walk goes up: auth/src → auth (has AGENTS.md!) → stop.
    const auto deep = auth / "src";
    fs::create_directories(deep);
    const std::string r = wire::agents_md_block("intro", workspace_root(),
                                                   deep);
    CHECK(r.find("<agents-md>") != std::string::npos);
    CHECK(r.find("root guidance") != std::string::npos);
    CHECK(r.find("<agents-md-package>") != std::string::npos);
    // Nearest is auth-level, NOT packages-level.
    CHECK(r.find("auth-level guidance") != std::string::npos);
    CHECK(r.find("packages-level guidance") == std::string::npos);
    fs::remove_all(ws);
}

// ── Global scope tests ───────────────────────────────────────────────────

static void test_global_agents_md() {
    // A global AGENTS.md passed explicitly via global_path → <agents-md-global> block.
    const auto ws = make_workspace();
    set_workspace_root(ws);
    fs::current_path(ws);
    write_file(ws / "AGENTS.md", "root guidance");
    // Create a fake global file in a temp dir (simulates ~/.agentty/AGENTS.md).
    const auto fake_home = fs::temp_directory_path() / ("agentty_test_home_" +
                          std::to_string(::getpid()));
    fs::create_directories(fake_home / ".agentty");
    const auto global_file = fake_home / ".agentty" / "AGENTS.md";
    write_file(global_file, "global-level guidance");
    const std::string r = wire::agents_md_block("intro", workspace_root(),
                                                   workspace_root(),
                                                   global_file);
    CHECK(r.find("<agents-md-global>") != std::string::npos);
    CHECK(r.find("global-level guidance") != std::string::npos);
    CHECK(r.find("</agents-md-global>") != std::string::npos);
    CHECK(r.find("<agents-md>") != std::string::npos);
    CHECK(r.find("root guidance") != std::string::npos);
    // Global block should come BEFORE root block.
    CHECK(r.find("<agents-md-global>") < r.find("<agents-md>"));
    fs::remove_all(ws);
    fs::remove_all(fake_home);
}

static void test_global_only_no_root() {
    // Global exists but root AGENTS.md is missing → only <agents-md-global>.
    const auto ws = make_workspace();
    set_workspace_root(ws);
    fs::current_path(ws);
    // No AGENTS.md at workspace root.
    const auto fake_home = fs::temp_directory_path() / ("agentty_test_home_" +
                          std::to_string(::getpid()));
    fs::create_directories(fake_home / ".agentty");
    const auto global_file = fake_home / ".agentty" / "AGENTS.md";
    write_file(global_file, "global-only guidance");
    const std::string r = wire::agents_md_block("intro", workspace_root(),
                                                   workspace_root(),
                                                   global_file);
    CHECK(r.find("<agents-md-global>") != std::string::npos);
    CHECK(r.find("global-only guidance") != std::string::npos);
    CHECK(r.find("<agents-md>") == std::string::npos);  // no root block
    CHECK(r.find("<agents-md-package>") == std::string::npos);
    fs::remove_all(ws);
    fs::remove_all(fake_home);
}

static void test_no_global_when_not_provided() {
    // global_path is empty → no <agents-md-global> block.
    const auto ws = make_workspace();
    set_workspace_root(ws);
    fs::current_path(ws);
    write_file(ws / "AGENTS.md", "root guidance");
    const std::string r = wire::agents_md_block("intro", workspace_root(),
                                                   workspace_root());
    CHECK(r.find("<agents-md>") != std::string::npos);
    CHECK(r.find("root guidance") != std::string::npos);
    CHECK(r.find("<agents-md-global>") == std::string::npos);
    fs::remove_all(ws);
}

static void test_global_truncated_at_cap() {
    // Global AGENTS.md is also capped at 64 KiB.
    const auto ws = make_workspace();
    set_workspace_root(ws);
    fs::current_path(ws);
    write_file(ws / "AGENTS.md", "root");
    const auto fake_home = fs::temp_directory_path() / ("agentty_test_home_" +
                          std::to_string(::getpid()));
    fs::create_directories(fake_home / ".agentty");
    const auto global_file = fake_home / ".agentty" / "AGENTS.md";
    std::string huge(64u * 1024u + 1000u, 'x');
    write_file(global_file, huge);
    const std::string r = wire::agents_md_block("intro", workspace_root(),
                                                   workspace_root(),
                                                   global_file);
    const auto glb_start = r.find("<agents-md-global>");
    const auto glb_end   = r.find("</agents-md-global>");
    CHECK(glb_start != std::string::npos);
    CHECK(glb_end   != std::string::npos);
    const auto block_size = glb_end - glb_start;
    // 64 KiB content + ~300 bytes for tags + intro line.
    CHECK(block_size <= 64u * 1024u + 500u);
    fs::remove_all(ws);
    fs::remove_all(fake_home);
}

static void test_resolve_global_agentty_wins() {
    // resolve_global_agents_md(): ~/.agentty/AGENTS.md wins over ~/.agents/AGENTS.md.
    const auto fake_home = fs::temp_directory_path() / ("agentty_test_home_" +
                          std::to_string(::getpid()));
    fs::create_directories(fake_home / ".agentty");
    fs::create_directories(fake_home / ".agents");
    write_file(fake_home / ".agentty" / "AGENTS.md", "agentty-global");
    write_file(fake_home / ".agents"  / "AGENTS.md", "agents-global");
    // Temporarily set HOME to our fake home.
    const char* orig_home = std::getenv("HOME");
    setenv("HOME", fake_home.string().c_str(), 1);
    const auto resolved = wire::resolve_global_agents_md();
    CHECK(!resolved.empty());
    // Should resolve to the ~/.agentty/ version.
    CHECK(resolved.filename() == "AGENTS.md");
    std::string content = wire::read_capped_file(resolved);
    CHECK(content.find("agentty-global") != std::string::npos);
    CHECK(content.find("agents-global") == std::string::npos);
    // Restore HOME.
    if (orig_home) setenv("HOME", orig_home, 1);
    else unsetenv("HOME");
    fs::remove_all(fake_home);
}

static void test_resolve_global_fallback_to_agents_dir() {
    // resolve_global_agents_md(): when ~/.agentty/AGENTS.md is missing,
    // falls back to ~/.agents/AGENTS.md.
    const auto fake_home = fs::temp_directory_path() / ("agentty_test_home_" +
                          std::to_string(::getpid()));
    fs::create_directories(fake_home / ".agents");
    write_file(fake_home / ".agents" / "AGENTS.md", "agents-fallback");
    const char* orig_home = std::getenv("HOME");
    setenv("HOME", fake_home.string().c_str(), 1);
    const auto resolved = wire::resolve_global_agents_md();
    CHECK(!resolved.empty());
    std::string content = wire::read_capped_file(resolved);
    CHECK(content.find("agents-fallback") != std::string::npos);
    if (orig_home) setenv("HOME", orig_home, 1);
    else unsetenv("HOME");
    fs::remove_all(fake_home);
}

static void test_resolve_global_both_missing() {
    // resolve_global_agents_md(): when neither file exists → empty path.
    const auto fake_home = fs::temp_directory_path() / ("agentty_test_home_" +
                          std::to_string(::getpid()));
    fs::create_directories(fake_home);  // home exists but no AGENTS.md anywhere
    const char* orig_home = std::getenv("HOME");
    setenv("HOME", fake_home.string().c_str(), 1);
    const auto resolved = wire::resolve_global_agents_md();
    CHECK(resolved.empty());
    if (orig_home) setenv("HOME", orig_home, 1);
    else unsetenv("HOME");
    fs::remove_all(fake_home);
}

int main() {
    test_missing_returns_empty();
    test_wraps_content();
    test_truncates_at_cap();
    test_uses_project_root_not_process_cwd();
    test_nested_overrides_root();
    test_no_nested_when_at_root();
    test_walk_stops_at_workspace_boundary();
    test_nested_truncates_at_cap();
    test_dedup_when_same_file();
    test_nearest_wins_not_deepest();
    test_global_agents_md();
    test_global_only_no_root();
    test_no_global_when_not_provided();
    test_global_truncated_at_cap();
    test_resolve_global_agentty_wins();
    test_resolve_global_fallback_to_agents_dir();
    test_resolve_global_both_missing();
    if (g_failures == 0) {
        std::printf("agents_md_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "agents_md_test: %d check(s) failed\n", g_failures);
    return 1;
}
