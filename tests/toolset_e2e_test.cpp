// toolset_e2e_test — drives EVERY built-in tool through agentty's REAL
// production dispatch path: tools::registry() → mcp_tools_bridge (ToolDef
// re-wrap) → mcp-cpp toolset implementation → decode_result → ToolOutput.
//
// Why this exists: mcp-cpp's own tests prove the tool BODIES; this test
// proves agentty's WIRING of them — the layer where a rename, a schema
// drift, a missing HostServices backend, or a broken decode would make a
// tool silently vanish or fail for the model while every unit test stays
// green. One process, one temp workspace, zero network (web tools are
// exercised only on their offline refusal paths; search_docs is pointed at
// an unreachable embed host so it falls back to BM25-only).
//
// Sandboxing discipline (learned the hard way — acp_integration_test once
// polluted the developer's real ~/.agentty/threads): HOME/USERPROFILE are
// redirected to the temp dir BEFORE any agentty code runs, so remember/
// forget/wipe_memory land in the sandbox, never in the user's real store.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "agentty/tool/mcp_tools_bridge.hpp"
#include "agentty/tool/mcp_tools_backends.hpp"
#include "agentty/tool/registry.hpp"
#include "agentty/tool/spec.hpp"
#include "agentty/tool/util/fs_helpers.hpp"

namespace fs = std::filesystem;
using nlohmann::json;
using namespace agentty;

namespace {

int g_checks = 0;
int g_fails  = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (ok) { std::printf("ok:   %s\n", what.c_str()); }
    else    { std::printf("FAIL: %s\n", what.c_str()); ++g_fails; }
}

// Dispatch through the production path — exactly what cmd_factory does.
tools::ExecResult run(std::string_view name, json args) {
    const auto* td = tools::find(name);
    if (!td) return std::unexpected(tools::ToolError::unknown(
        "tool not in registry: " + std::string{name}));
    return td->execute(args);
}

std::string text_of(const tools::ExecResult& r) {
    return r ? r->text : r.error().detail;
}

bool has(const tools::ExecResult& r, std::string_view needle) {
    return r && r->text.find(needle) != std::string::npos;
}

void write_file(const fs::path& p, std::string_view body) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << body;
}

bool git_available() {
    return std::system("git --version >/dev/null 2>&1") == 0;
}

} // namespace

int main() {
    std::printf("toolset_e2e_test\n");

    // ── Sandbox: everything under one temp root, BEFORE first registry()
    // touch (the registry is a process-lifetime static; workspace root and
    // HOME must be final before it is built).
    auto root = fs::temp_directory_path() / "agentty_toolset_e2e";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "src");
    root = fs::canonical(root);

    ::setenv("HOME",        root.c_str(), 1);
    ::setenv("USERPROFILE", root.c_str(), 1);
    // search_docs: unreachable embed host → instant connect-refused →
    // BM25-only fallback, no 3 s Ollama probe stall, no network.
    ::setenv("AGENTTY_OLLAMA_HOST", "127.0.0.1:1", 1);
    ::unsetenv("AGENTTY_MCP_CONFIG");   // no external MCP servers
    ::unsetenv("AGENTTY_DOCS_DIR");

    tools::util::set_workspace_root(root);
    tools::wire_mcp_runtime("off");   // no bwrap wrapping — CI portability

    // ── Registry completeness: every catalog tool must be advertised. ──
    {
        const auto& reg = tools::registry();
        for (const auto& spec : tools::spec::kCatalog) {
            const auto* td = tools::find(spec.name);
            check(td != nullptr,
                  std::string{"registry advertises '"} + std::string{spec.name} + "'");
            if (!td) continue;
            check(td->input_schema.is_object(),
                  std::string{spec.name} + ": input_schema is an object");
            check(td->effects.bits() == spec.effects.bits(),
                  std::string{spec.name} + ": effects match spec catalog");
            check(static_cast<bool>(td->execute),
                  std::string{spec.name} + ": execute closure installed");
        }
        // Recall-bias ordering: read/edit lead, host shells trail.
        check(!reg.empty() && reg.front().name.value == "read",
              "wire order: 'read' listed first");
    }

    const auto file = root / "src" / "hello.txt";

    // ── write → FileChange carried ──────────────────────────────────────
    {
        auto r = run("write", {{"file_path", file.string()},
                               {"content", "alpha\nbeta\ngamma\n"}});
        check(r.has_value(), "write: succeeds");
        check(r && r->change.has_value(), "write: carries FileChange");
        check(r && r->change && r->change->added == 3, "write: 3 lines added");
    }

    // ── read ─────────────────────────────────────────────────────────────
    {
        auto r = run("read", {{"path", file.string()}});
        check(has(r, "beta"), "read: returns content");
    }

    // ── edit → fuzzy splice + FileChange with hunks ──────────────────────
    {
        json e = {{"old_text", "beta"}, {"new_text", "BETA-EDITED"}};
        auto r = run("edit", {{"path", file.string()},
                              {"edits", json::array({e})}});
        check(r.has_value(), "edit: succeeds");
        check(r && r->change && !r->change->hunks.empty(),
              "edit: FileChange has recomputed hunks (diff-review feed)");
        std::ifstream f(file);
        std::string body((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
        check(body.find("BETA-EDITED") != std::string::npos,
              "edit: change landed on disk");
    }

    // ── grep / glob / list_dir / find_definition ─────────────────────────
    write_file(root / "src" / "code.cpp",
               "int answer() { return 42; }\nint other() { return 7; }\n");
    {
        auto r = run("grep", {{"pattern", "BETA-EDITED"}, {"path", root.string()}});
        check(has(r, "hello.txt"), "grep: finds the edited line");
    }
    {
        auto r = run("glob", {{"pattern", "*.cpp"}, {"path", root.string()}});
        check(has(r, "code.cpp"), "glob: matches by extension");
    }
    {
        auto r = run("list_dir", {{"path", (root / "src").string()}});
        check(has(r, "hello.txt") && has(r, "code.cpp"),
              "list_dir: lists both files");
    }
    {
        auto r = run("find_definition", {{"symbol", "answer"},
                                         {"path", root.string()}});
        check(has(r, "code.cpp"), "find_definition: locates the function");
    }

    // ── repo_map: ranked skeleton over the sandbox ────────────────────
    {
        auto r = run("repo_map", {{"path", root.string()}});
        check(has(r, "code.cpp"), "repo_map: surfaces the source file");
        check(has(r, "answer"), "repo_map: shows definition signatures");
    }

    // ── bash (sandbox off) ──────────────────────────────────────────────────
    {
        auto r = run("bash", {{"command", "echo e2e-bash-ok"},
                              {"cd", root.string()}});
        check(has(r, "e2e-bash-ok"), "bash: runs and captures stdout");
    }

    // ── bash: terminal line-discipline at the capture boundary ────────
    // A child that emits CSI/OSC escapes (SGR colors, a DECSTBM region
    // probe, an OSC title) plus CR progress rewinds must come back CLEAN:
    // no ESC/CR/BS bytes (they'd paint as stray glyphs in the tool card
    // and commit to scrollback — the "r r" corruption report), SGR text
    // preserved, and the CR progress line collapsed to its final state.
    {
        auto r = run("bash",
            {{"command",
              "printf 'p 1\\r'; printf 'p 2\\n';"
              " printf '\\033[1;32mgreen-e2e\\033[0m\\n';"
              " printf '\\033[3;24r'; printf '\\033]0;title\\007after-e2e\\n'"},
             {"cd", root.string()}});
        const std::string body = text_of(r);
        bool clean = true;
        for (unsigned char c : body)
            if (c == 0x1b || c == '\r' || c == '\b'
                || (c < 0x20 && c != '\n' && c != '\t'))
                clean = false;
        check(clean, "bash: output free of ESC/CR/BS control bytes");
        check(has(r, "green-e2e"), "bash: SGR-wrapped text survives the strip");
        check(has(r, "after-e2e"), "bash: text after DECSTBM/OSC survives");
        check(has(r, "p 2"), "bash: CR progress collapses to final state");
        check(body.find("[3;24r") == std::string::npos
                  && body.find(";24r") == std::string::npos,
              "bash: no stray CSI parameter bytes (the 'r r' glyphs)");
    }

    // ── workspace-root boundary: fs tools refuse escapes ─────────────────
    {
        auto r = run("read", {{"path", "/etc/hostname"}});
        check(!r.has_value(), "read: refuses path outside workspace root");
    }

    // ── todo (stateless shell) ────────────────────────────────────────────
    {
        json item = {{"content", "prove the tools"}, {"status", "in_progress"}};
        auto r = run("todo", {{"todos", json::array({item})}});
        check(has(r, "prove the tools"), "todo: echoes the plan");
    }

    // ── memory: remember → forget(dry-run) → forget → wipe ───────────────
    {
        auto r = run("remember", {{"text", "e2e sentinel fact alpha"},
                                  {"scope", "user"}});
        check(r.has_value(), "remember: appends (user scope, sandboxed HOME)");
        check(fs::exists(root / ".agentty" / "memory.jsonl"),
              "remember: wrote inside the sandbox, not the real HOME");

        auto p = run("forget", {{"substring", "sentinel fact"}, {"dry_run", true}});
        check(has(p, "alpha"), "forget: dry-run previews the record");

        auto d = run("forget", {{"substring", "sentinel fact"}});
        check(d.has_value(), "forget: removes by substring");

        (void)run("remember", {{"text", "wipe me"}, {"scope", "user"}});
        auto w = run("wipe_memory", {{"scope", "user"}, {"confirm", true}});
        check(w.has_value(), "wipe_memory: confirmed wipe succeeds");
    }

    // ── skill: unknown name → recovery hint, not a crash ─────────────────
    {
        auto r = run("skill", {{"name", "no-such-skill-xyz"}});
        check(text_of(r).find("no skill named") != std::string::npos,
              "skill: unknown name yields recovery hint");
    }

    // ── search_docs: BM25-only RAG over a real docs corpus ──────────────
    // resolve_docs_root() checks AGENTTY_DOCS_DIR first, then CWD/docs —
    // and this test's cwd is the build dir, so the env var must point at
    // the sandbox corpus. Read per-retrieve, so setting it here is fine.
    write_file(root / "docs" / "zebra.md",
               "# Zebra habits\n\nThe zebra quagga migrates across the "
               "savanna every solstice season.\n");
    write_file(root / "docs" / "other.md",
               "# Unrelated\n\nNothing to see here about databases.\n");
    ::setenv("AGENTTY_DOCS_DIR", (root / "docs").c_str(), 1);
    {
        auto r = run("search_docs", {{"query", "zebra quagga migration"}});
        check(has(r, "zebra"), "search_docs: BM25 retrieval hits the passage");
        check(has(r, "BM25"), "search_docs: reports BM25-only mode (no embed host)");
    }

    // ── #5 per-turn query cache: an identical query is served from cache ──
    // The result must be identical AND the mode string must gain ", cached".
    {
        auto r = run("search_docs", {{"query", "zebra quagga migration"}});
        check(has(r, "zebra"), "search_docs: cached hit returns same passage");
        check(has(r, "cached"), "search_docs: repeat query reports cached mode");
    }

    // ── #2 corrective retrieval (CRAG): a conversationally-phrased query
    // that still contains the content word must retrieve. The distiller
    // strips the stopwords ("can you tell me how the ... works") so the
    // retry probe lands on "zebra quagga". Confidence-gated, so on a strong
    // first pass it simply won't fire — either way the passage is found.
    {
        auto r = run("search_docs",
                     {{"query", "can you tell me how the zebra quagga works"}});
        check(has(r, "zebra"),
              "search_docs: corrective/distilled retry still finds the passage");
    }

    // ── search_docs × memory: learned facts are a fused knowledge source ──
    // A remembered fact must be retrievable by query — including facts that
    // rolled OUT of the 6 KiB prompt budget — with memory:// provenance.
    {
        (void)run("remember", {{"text", "the flux capacitor requires "
                                        "gigawatt plutonium calibration"},
                               {"scope", "user"}});
        auto r = run("search_docs", {{"query", "flux capacitor plutonium"}});
        check(has(r, "flux capacitor"),
              "search_docs: retrieves a remembered fact");
        check(has(r, "memory"),
              "search_docs: memory hit carries memory provenance");
        (void)run("wipe_memory", {{"scope", "user"}, {"confirm", true}});
    }

    // ── #1 proactive retrieval: the pre-turn active-RAG path ─────────────
    // proactive_retrieve() runs the SAME pipeline out of band and only
    // returns a block when confidence clears the HIGH bar. With the bar
    // lowered via env, a strong query must produce a fenced context block
    // that names the source and carries the passage; an off-topic query
    // must produce nothing (no unprompted token spend).
    {
        ::setenv("AGENTTY_RAG_PROACTIVE_MIN", "0.0", 1);
        auto hit = tools::proactive_retrieve("zebra quagga migration", 3);
        check(hit.has_value(), "proactive_retrieve: strong query yields a hit");
        if (hit) {
            check(hit->block.find("<retrieved-context>") != std::string::npos,
                  "proactive_retrieve: emits a fenced context block");
            check(hit->block.find("zebra") != std::string::npos,
                  "proactive_retrieve: block carries the retrieved passage");
            check(hit->passages >= 1,
                  "proactive_retrieve: reports at least one passage");
        }
        // With a bar of 1.01 (unreachable), nothing should ever inject.
        ::setenv("AGENTTY_RAG_PROACTIVE_MIN", "1.01", 1);
        auto none = tools::proactive_retrieve("zebra quagga migration", 3);
        check(!none.has_value(),
              "proactive_retrieve: an unreachable bar suppresses injection");
        ::unsetenv("AGENTTY_RAG_PROACTIVE_MIN");
    }

    // ── #3 search_code: semantic code retrieval over the workspace ───────
    // The retriever walks CWD, so chdir into the sandbox (restored after)
    // where a distinctive source file exists. BM25-only here (no embed
    // host); the conceptual-query win needs embeddings, but the lexical
    // path must already index + retrieve + cite path:lines.
    {
        write_file(root / "src" / "throttler.cpp",
                   "// Request rate limiting\n"
                   "int backoff_ms(int attempt) {\n"
                   "    return (1 << attempt) * 100; // exponential backoff\n"
                   "}\n");
        auto prev_cwd = fs::current_path();
        fs::current_path(root);
        auto r = run("search_code", {{"query", "exponential backoff rate"}});
        fs::current_path(prev_cwd);
        check(has(r, "backoff"), "search_code: BM25 retrieval hits the function");
        check(has(r, "throttler.cpp"), "search_code: result cites the source path");
        check(has(r, "BM25"), "search_code: reports BM25-only mode (no embed host)");
    }

    // ── task: no subagent runner installed → graceful refusal ────────────
    {
        auto r = run("task", {{"prompt", "explore the codebase"}});
        check(!text_of(r).empty(), "task: unavailable runner answers, no crash");
        check(!r.has_value() || text_of(r).find("unavailable") != std::string::npos
              || text_of(r).find("not configured") != std::string::npos
              || text_of(r).find("no ") != std::string::npos,
              "task: names the missing backend");
    }

    // ── web tools: offline arg/error paths only ──────────────────────────
    {
        auto r = run("web_fetch", {{"url", "http://example.com"}});
        check(!r.has_value() || text_of(r).find("https") != std::string::npos,
              "web_fetch: refuses non-https without touching the network");
    }
    {
        auto r = run("web_fetch", {{"url", "not a url"}});
        check(!r.has_value() || !text_of(r).empty(),
              "web_fetch: malformed url yields an error, not a crash");
    }

    // ── git quartet over a real repo ──────────────────────────────────────
    if (git_available()) {
        const std::string q = "\"";
        std::string setup =
            "cd " + q + root.string() + q + " && git init -q"
            " && git config user.email e2e@test && git config user.name e2e"
            " && git add -A && git commit -qm seed";
        check(std::system(setup.c_str()) == 0, "git: seed repo created");

        write_file(root / "src" / "hello.txt", "changed content\n");

        auto st = run("git_status", {{"path", root.string()}});
        check(has(st, "hello.txt"), "git_status: sees the modified file");

        auto df = run("git_diff", {{"path", root.string()}});
        check(has(df, "changed content"), "git_diff: shows the hunk");

        auto cm = run("git_commit", {{"message", "e2e commit"},
                                     {"stage_all", true},
                                     {"path", root.string()}});
        check(cm.has_value(), "git_commit: stages + commits");

        auto lg = run("git_log", {{"path", root.string()}, {"count", 5}});
        check(has(lg, "e2e commit"), "git_log: shows the new commit");
    } else {
        std::printf("skip: git not available — git_* section skipped\n");
    }

    // ── diagnostics: no build system → structured answer, not a hang ─────
    {
        auto r = run("diagnostics", json::object());
        check(!text_of(r).empty(), "diagnostics: answers without a build system");
    }

    std::printf("%d checks, %d failures\n", g_checks, g_fails);
    if (g_fails == 0) { std::printf("PASSED\n"); return 0; }
    std::printf("FAILED\n");
    return 1;
}
