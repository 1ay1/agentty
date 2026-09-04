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

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>   // getpid
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "agentty/tool/memory_store.hpp"
#include "agentty/tool/mcp_tools_bridge.hpp"
#include "agentty/tool/mcp_tools_backends.hpp"
#include "agentty/tool/subagent.hpp"
#include "agentty/tool/registry.hpp"
#include "agentty/tool/spec.hpp"
#include "agentty/tool/tool.hpp"
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
    // PID-unique sandbox: CTest runs the suite with -j, and this test spawns
    // real subprocesses (bash/process_start) under a global HOME. A FIXED
    // shared path let a sibling test's teardown race our tree — the source of
    // the intermittent toolset_e2e failure under -j12. Match the rest of the
    // suite and key the root on getpid().
    auto root = fs::temp_directory_path() /
                ("agentty_toolset_e2e_" + std::to_string(::getpid()));
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

    // The model can reach native and third-party MCP executors. No exception
    // type is allowed to cross this noexcept process boundary.
    {
        tools::ToolDef throwing;
        throwing.name.value = "throwing_test_tool";
        throwing.execute = [](const json&) -> tools::ExecResult { throw 7; };
        auto guarded = tool::DynamicDispatch::execute_with(
            &throwing, throwing.name.value, json::object());
        check(!guarded && guarded.error().kind == tools::ErrorKind::Unknown,
              "dispatch: contains non-standard tool exceptions");
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

    // ── write→edit nudge: overwriting a big file with a tiny change hints
    //    that edit would have been better; a full rewrite stays silent. ────
    {
        // Use a DEDICATED file — never the shared `file` fixture that the
        // read/edit/grep blocks below depend on.
        auto nudge_file = root / "nudge_e2e.txt";
        std::string big;
        for (int i = 0; i < 60; ++i) big += "line " + std::to_string(i) + "\n";
        auto seed = run("write", {{"file_path", nudge_file.string()}, {"content", big}});
        check(seed.has_value(), "write nudge: seed big file");

        // Change exactly one line out of 60 -> should nudge toward edit.
        std::string tiny = big;
        auto pos = tiny.find("line 30\n");
        if (pos != std::string::npos) tiny.replace(pos, 8, "line 30 CHANGED\n");
        auto small = run("write", {{"file_path", nudge_file.string()}, {"content", tiny}});
        check(small.has_value(), "write nudge: small overwrite succeeds");
        check(has(small, "edit") && has(small, "tip"),
              "write nudge: small overwrite of big file suggests edit");

        // A wholesale rewrite (most lines differ) must NOT nudge.
        std::string rewrite;
        for (int i = 0; i < 60; ++i) rewrite += "fresh " + std::to_string(i) + "\n";
        auto full = run("write", {{"file_path", nudge_file.string()}, {"content", rewrite}});
        check(full.has_value() && !has(full, "tip: this overwrote"),
              "write nudge: full rewrite stays silent");
    }

    // ── read ─────────────────────────────────────────────────────────
    {
        auto r = run("read", {{"path", file.string()}});
        check(has(r, "beta"), "read: returns content");
    }
    {
        // Reading a 0-byte file gives an explicit heads-up, not a blank result
        // the model could mistake for a failed read.
        auto empty = root / "empty_e2e.txt";
        run("write", {{"file_path", empty.string()}, {"content", ""}});
        auto r = run("read", {{"path", empty.string()}});
        check(has(r, "empty"), "read: 0-byte file is flagged as empty");
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
    // ── edit line locator is CORRECT and LIVE ─────────────────────────
    {
        // Known layout: the edited line is line 3.
        auto lf = root / "locator_e2e.txt";
        write_file(lf, "one\ntwo\nthree\nfour\nfive\n");
        auto r = run("edit", {{"path", lf.string()},
            {"edits", json::array({{{"old_text", "three"},
                                    {"new_text", "THREE-X"}}})}});
        check(has(r, "at line 3"),
              "edit: locator names the changed line (3), not the hunk-context start");
        // And the change really is on line 3 (THREE-X replaces `three`).
        check(has(r, "+THREE-X"), "edit: the diff shows the change");
    }
    {
        // LIVE across a line shift: edit #1 inserts 2 lines above the target,
        // so edit #2's target moves from line 5 to line 7. The reported line
        // must reflect the SHIFTED (final) position, proving it's computed
        // against the evolving buffer, not the original.
        auto lf = root / "locator_shift_e2e.txt";
        write_file(lf, "a\nb\nc\nd\nTARGET\nf\n");   // TARGET is line 5
        auto r = run("edit", {{"path", lf.string()}, {"edits", json::array({
            {{"old_text", "a\nb"}, {"new_text", "a\nINS1\nINS2\nb"}},  // +2 lines
            {{"old_text", "TARGET"}, {"new_text", "HIT"}},
        })}});
        check(r.has_value(), "edit: multi-edit with line shift applies");
        // Both edits coalesce into one hunk; the locator names its first
        // change (line 2, the INS1 insertion). Liveness is proven by the diff:
        // TARGET->HIT lands AFTER the +2 shift, i.e. at new-side line 7.
        check(has(r, " at line 2"),
              "edit: locator names the first changed line of the merged hunk");
        // The unified diff's new side must show HIT at line 7 (a INS1 INS2 b
        // c d HIT) — confirming the second edit was applied against the
        // shifted, live buffer, not the original line 5.
        {
            auto t = text_of(r);
            auto hit = t.find("+HIT");
            auto ins = t.find("+INS1");
            check(hit != std::string::npos && ins != std::string::npos
                  && hit > ins,
                  "edit: second edit applied live after the first shifted lines");
        }
    }
    {
        // DEFINITIVE liveness: two edits FAR apart stay separate hunks. Edit #1
        // adds 3 lines near the top; edit #2's target (originally line 20) must
        // be reported at line 23 — proving the second locator is computed on the
        // buffer AS SHIFTED by the first edit, not the original.
        auto lf = root / "locator_far_e2e.txt";
        std::string body;
        for (int i = 1; i <= 25; ++i) body += "row" + std::to_string(i) + "\n";
        write_file(lf, body);
        auto r = run("edit", {{"path", lf.string()}, {"edits", json::array({
            {{"old_text", "row2"}, {"new_text", "row2\nADD_A\nADD_B\nADD_C"}}, // +3
            {{"old_text", "row20"}, {"new_text", "ROW20-HIT"}},
        })}});
        check(r.has_value(), "edit: far-apart multi-edit applies");
        // row20 sat at line 20; after +3 lines above it lands at line 23.
        // The locator line (first line of the message, before the diff fence)
        // must contain 23 — check only that prefix so we don't match "row23"
        // in the diff body.
        {
            auto t = text_of(r);
            auto fence = t.find("```");
            auto head = t.substr(0, fence == std::string::npos ? t.size() : fence);
            check(head.find("23") != std::string::npos,
                  "edit: far hunk's locator reflects the live +3 shift (20 -> 23)");
        }
    }

    // ── move / remove: safe shell-free filesystem mutations ─────────────
    {
        const auto renamed = root / "src" / "renamed.txt";
        auto moved = run("move", {{"source", file.string()},
                                  {"destination", renamed.string()}});
        check(moved.has_value() && fs::exists(renamed) && !fs::exists(file),
              "move: renames inside workspace");
        auto restored = run("move", {{"source", renamed.string()},
                                     {"destination", file.string()}});
        check(restored.has_value() && fs::exists(file), "move: can restore path");
        const auto disposable = root / "src" / "remove-me.txt";
        write_file(disposable, "temporary\n");
        auto removed = run("remove", {{"path", disposable.string()}});
        check(removed.has_value() && !fs::exists(disposable), "remove: deletes a file");
        auto root_remove = run("remove", {{"path", root.string()}, {"recursive", true}});
        check(!root_remove.has_value(), "remove: refuses workspace root");
    }

    // ── grep / glob / list_dir / find_definition ─────────────────────────
    write_file(root / "src" / "code.cpp",
               "int answer() { return 42; }\nint other() { return 7; }\n");
    {
        auto r = run("grep", {{"pattern", "BETA-EDITED"}, {"path", root.string()}});
        check(has(r, "hello.txt"), "grep: finds the edited line");
    }
    {
        // A zero-match search returns a tailored, actionable hint (not a bare
        // "No matches found.") so the model knows what to try next.
        auto r = run("grep", {{"pattern", "zzz_definitely_absent_token_qwerty"},
                              {"path", root.string()}});
        check(has(r, "No matches"), "grep: reports zero matches");
        check(has(r, "search_code") || has(r, "Try:"),
              "grep: zero-match output suggests next steps");
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

    {
        // grep word=true absorbs the old find_references: whole-word uses of an
        // identifier, no substring false positives.
        auto r = run("grep", {{"pattern", "answer"},
                              {"word", true},
                              {"path", root.string()}});
        check(has(r, "code.cpp"), "grep word=true: locates exact identifier uses");
    }

    // ── repo_map: ranked skeleton over the sandbox ────────────────────
    {
        auto r = run("repo_map", {{"path", root.string()}});
        check(has(r, "code.cpp"), "repo_map: surfaces the source file");
        check(has(r, "answer"), "repo_map: shows definition signatures");
    }

    // ── bash (sandbox off) ──────────────────────────────────────────────────
    {
        auto r = run("shell", {{"command", "echo e2e-bash-ok"},
                              {"cd", root.string()}});
        check(has(r, "e2e-bash-ok"), "bash: runs and captures stdout");
    }
    // ── bash failure feedback: exit-code decode + error-line digest ──────────
    {
        // 127 = command not found — the decode hint must appear.
        auto r = run("shell", {{"command", "this_binary_does_not_exist_e2e"},
                              {"cd", root.string()}});
        check(has(r, "127"), "bash: reports the numeric exit code");
        check(has(r, "not found"), "bash: decodes exit 127 as command-not-found");
    }
    {
        // A failing command whose output contains an error: line should get a
        // leading digest so the model sees the cause before the full dump.
        auto r = run("shell", {{"command",
                               "echo 'warming up'; echo 'error: something broke'; exit 2"},
                              {"cd", root.string()}});
        check(has(r, "exit code 2"), "bash: reports non-zero exit");
        check(has(r, "Key error line"), "bash: surfaces an error digest on failure");
        check(has(r, "something broke"), "bash: digest carries the error text");
    }

    // ── bash: terminal line-discipline at the capture boundary ────────
    // A child that emits CSI/OSC escapes (SGR colors, a DECSTBM region
    // probe, an OSC title) plus CR progress rewinds must come back CLEAN:
    // no ESC/CR/BS bytes (they'd paint as stray glyphs in the tool card
    // and commit to scrollback — the "r r" corruption report), SGR text
    // preserved, and the CR progress line collapsed to its final state.
    {
        auto r = run("shell",
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

    // ── focused tests + persistent process sessions ─────────────────────
    {
        auto tested = run("test", {{"command", "printf native-test-ok"}});
        check(has(tested, "PASS") && has(tested, "native-test-ok"),
              "test: runs an explicit focused command with pass status");

        // A failing run surfaces WHICH test failed as a leading digest so the
        // model doesn't have to scan the whole runner log.
        auto failed = run("test", {{"command",
            "printf '[  FAILED  ] Suite.CaseX\\n'; exit 1"}});
        check(has(failed, "FAIL"), "test: failing run reports FAIL status");
        check(has(failed, "Failing test") && has(failed, "Suite.CaseX"),
              "test: failing run digests the failing test name");

        auto started = run("process_start", {{"command", "printf 'process-first\\n'; sleep 1; printf 'process-second\\n'; sleep 5"},
                                              {"cwd", root.string()}});
        check(started.has_value() && has(started, "proc-"), "process_start: returns session id");
        if (started) {
            const auto marker = started->text.find("proc-");
            const auto end = started->text.find(' ', marker);
            const std::string id = started->text.substr(marker, end - marker);

            // Poll until a marker shows rather than racing one fixed window:
            // under a loaded CI box (this test co-schedules with the whole
            // suite) the child can take far longer than any single wait_ms to
            // be scheduled and have its pipe drained. Loop with a generous
            // overall deadline so the assertion tests SEMANTICS (initial
            // output arrives, later output arrives once, no replay) instead
            // of subprocess scheduling latency.
            auto poll_until = [&](const char* needle, int budget_ms) {
                auto last = run("process_poll", {{"id", id}, {"wait_ms", 500}});
                int waited = 500;
                while (!has(last, needle) && waited < budget_ms) {
                    auto r = run("process_poll", {{"id", id}, {"wait_ms", 500}});
                    waited += 500;
                    // A poll only returns output produced since the previous
                    // poll; keep the newest non-empty result so the hit isn't
                    // lost when a later poll comes back empty.
                    if (has(r, needle) || r) last = std::move(r);
                }
                return last;
            };

            // process_start waits ~300ms and drains any banner the child
            // already printed INTO the start response (so the first poll isn't
            // wasted on the startup line). The immediate "process-first" line
            // may therefore arrive at START rather than on a poll — assert it
            // shows up in EITHER place, testing the semantic (initial output is
            // delivered exactly once) not which call carries it.
            const bool first_at_start = has(started, "process-first");
            auto first = first_at_start ? started : poll_until("process-first", 10000);
            check(has(first, "process-first"),
                  "process_start/poll: initial output is delivered");
            auto second = poll_until("process-second", 10000);
            check(has(second, "process-second"), "process_poll: waits for new output");
            check(second.has_value() && !has(second, "process-first"),
                  "process_poll: does not repeat previously delivered output");
            auto stopped = run("process_stop", {{"id", id}});
            check(stopped.has_value(), "process_stop: terminates and reaps session");
            check(stopped.has_value() && !has(stopped, "process-first")
                                      && !has(stopped, "process-second"),
                  "process_stop: does not replay output delivered by polls");
        }
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

    // ── memory: project fallback + remember → forget → wipe ────────────
    {
        // `--workspace /` means unrestricted tools, not project-at-root.
        // Project memory must still anchor to the process cwd.
        const auto prior_cwd = fs::current_path();
        fs::current_path(root);
        tools::util::set_workspace_root("/");
        check(tools::memory::path_for(tools::memory::Scope::Project)
                  == root / ".agentty" / "memory.jsonl",
              "remember: project scope uses cwd under --workspace /");
        tools::util::set_workspace_root(root);
        fs::current_path(prior_cwd);

        const auto* remember_def = tools::find("remember");
        const auto scope_enum = remember_def
            ? remember_def->input_schema["properties"]["scope"]["enum"]
            : json{};
        const bool schema_has_project = scope_enum.is_array()
            && std::find(scope_enum.begin(), scope_enum.end(), "project")
                != scope_enum.end();
        check(schema_has_project,
              "remember: schema advertises available project scope");

        auto defaulted = run("remember", {{"text", "default project sentinel"}});
        check(defaulted.has_value(), "remember: default project scope appends");

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

    // ── memory: smart scope inference ────────────────────────────────
    {
        using tools::memory::suggest_scope;
        using tools::memory::Scope;

        // Personal facts → User, with a nameable cue.
        auto n1 = suggest_scope("my name is Ayush and I prefer fish shell");
        check(n1.confident() && n1.scope == Scope::User,
              "suggest_scope: personal identity/preference → user");
        auto n2 = suggest_scope("I use vim with relative line numbers");
        check(n2.confident() && n2.scope == Scope::User,
              "suggest_scope: personal tooling → user");

        // Codebase facts → Project.
        auto p1 = suggest_scope("the build command is cmake --build build -j 8");
        check(p1.confident() && p1.scope == Scope::Project,
              "suggest_scope: build workflow → project");
        auto p2 = suggest_scope("in this project we put reducers under src/runtime");
        check(p2.confident() && p2.scope == Scope::Project,
              "suggest_scope: repo deixis + path → project");

        // Ambiguous / no signal → not confident (caller keeps its default).
        auto a1 = suggest_scope("the quick brown fox");
        check(!a1.confident(), "suggest_scope: no signal is not confident");

        // End-to-end: a personal fact with NO explicit scope must be
        // auto-corrected from the project default to user, and say so.
        auto corrected = run("remember",
            {{"text", "my name is Ayush, call me A"}});
        check(corrected.has_value(), "remember: personal fact appends");
        check(text_of(corrected).find("user") != std::string::npos,
              "remember: smart scope notes the project→user correction");
        // It landed in USER memory, so a user-scope wipe reclaims it.
        auto wc = run("wipe_memory", {{"scope", "user"}, {"confirm", true}});
        check(text_of(wc).find("0") == std::string::npos || wc.has_value(),
              "remember: corrected fact was written to user scope");

        // A codebase fact with no explicit scope stays project (no spurious
        // correction) — it must NOT appear in user memory after the wipe.
        auto kept = run("remember",
            {{"text", "the build command for this project is make release"}});
        check(kept.has_value(), "remember: project fact appends");
        check(text_of(kept).find("→user") == std::string::npos,
              "remember: project fact is not redirected to user");
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
        check(has(r, "hybrid") || has(r, "bm25"),
              "search_docs: reports truthful BM25 or hybrid mode");
    }

    // ── repeat query: the engine idempotently serves the SAME passage ──
    // rag-cpp keeps the index warm across calls; an identical query must
    // return the same top passage (deterministic ranking).
    {
        auto r = run("search_docs", {{"query", "zebra quagga migration"}});
        check(has(r, "zebra"), "search_docs: repeat query returns same passage");
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

    // ── single-execution proactive retrieval + dedup ────────────────
    // The compatibility entry point and isolated-worker entry point share one
    // funnel; there is no detached hedge and no duplicate query.
    {
        ::setenv("AGENTTY_RAG_PROACTIVE_MIN", "0.0", 1);
        // A dedicated doc so this passage has NEVER been injected before —
        // the dedup FIFO is keyed on source:path:line, so reusing zebra.md
        // (already injected above) couldn't prove the no-poison property.
        write_file(root / "docs" / "okapi.md",
                   "# Okapi range\n\nThe okapi bongo forages the Ituri "
                   "rainforest understory at dawn.\n");
        // Force the corpus to pick up the new file (the index is warm from
        // the search_docs calls above and only re-scans on an explicit
        // retrieve). Assert it's retrievable so a later miss can only mean
        // dedup poisoning, not a stale index.
        {
            auto seed = run("search_docs", {{"query", "okapi bongo rainforest"}});
            check(has(seed, "okapi"),
                  "proactive_retrieve: okapi seed doc is indexed");
        }

        auto landed = tools::proactive_retrieve("okapi bongo rainforest", 3);
        check(landed.has_value(),
              "proactive_retrieve: single funnel yields the passage");
        if (landed)
            check(landed->block.find("okapi") != std::string::npos,
                  "proactive_retrieve_blocking: block carries the passage");

        // Having injected, a repeat blocking call must dedup it away (the
        // model already saw it this session) — proving commit happened.
        auto again = tools::proactive_retrieve_blocking("okapi bongo rainforest", 3);
        check(!again.has_value(),
              "proactive_retrieve_blocking: commits dedup after injecting");
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
        check(has(r, "hybrid") || has(r, "bm25"),
              "search_code: reports truthful BM25 or hybrid mode");
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

    // ── agent provenance: a PROJECT-defined persona is flagged ───────────
    // A repo can ship .agentty/agents/*.md whose role prompt is
    // attacker-controllable, so the task card tags it "project agent".
    // Built-ins and (would-be) user agents get no tag. Transparency only —
    // the agent still runs, tools stay gated.
    {
        write_file(root / ".agentty" / "agents" / "helper.md",
                   "---\ndescription: a project helper\n---\n"
                   "Your role: help with project tasks.\n");
        namespace sub = tools::subagent;
        // User-agent discovery is cwd-relative (project_root()), so run this
        // check from inside the sandbox root, restored after.
        auto prev_cwd = fs::current_path();
        fs::current_path(root);
        check(sub::agent_origin("helper") == "project",
              "agent provenance: a project-shipped agent is 'project'");
        check(sub::agent_origin("explorer") == "builtin",
              "agent provenance: a built-in agent is 'builtin' (no tag)");
        check(sub::agent_origin("nonexistent") == "builtin",
              "agent provenance: an unknown name is 'builtin' (safe default)");
        fs::current_path(prev_cwd);
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
    // ── SSRF guard: private / metadata / loopback targets are refused, incl.
    //    the IPv4-mapped IPv6 spellings that bypass a naive prefix filter.
    //    All reject before any socket, so these run fully offline.
    {
        auto blocked = [&](const char* url) {
            auto r = run("web_fetch", {{"url", url}});
            // Either a hard error, or a message that never contains fetched
            // page content — the guard must not connect. We assert the tool
            // did not return a successful body.
            return !r.has_value()
                || text_of(r).find("blocked") != std::string::npos
                || text_of(r).find("not allowed") != std::string::npos
                || text_of(r).find("private") != std::string::npos
                || text_of(r).find("refus") != std::string::npos
                || text_of(r).find("error") != std::string::npos;
        };
        check(blocked("http://169.254.169.254/latest/meta-data/"),
              "web_fetch SSRF: blocks cloud-metadata IPv4");
        check(blocked("http://127.0.0.1:8080/"),
              "web_fetch SSRF: blocks loopback");
        check(blocked("http://2130706433/"),
              "web_fetch SSRF: blocks decimal-encoded loopback");
        check(blocked("http://[::ffff:127.0.0.1]/"),
              "web_fetch SSRF: blocks IPv4-mapped IPv6 loopback");
        check(blocked("http://[::ffff:169.254.169.254]/"),
              "web_fetch SSRF: blocks IPv4-mapped IPv6 metadata");
    }

    // ── git quartet over a real repo ──────────────────────────────────────
    if (git_available()) {
        const std::string q = "\"";
        std::string setup =
            "cd " + q + root.string() + q + " && git init -q"
            " && git config user.email e2e@test && git config user.name e2e"
            " && git add -A && git commit -qm seed";
        check(std::system(setup.c_str()) == 0, "git: seed repo created");

        // A narrowed workspace inside a parent repository must not let Git
        // rediscover that parent and expose/commit sibling files.
        tools::util::set_workspace_root(root / "src");
        auto escaped = run("git_status", {{"path", (root / "src").string()}});
        check(!escaped && escaped.error().kind == tools::ErrorKind::OutOfWorkspace,
              "git_status: refuses repository rooted above workspace");
        tools::util::set_workspace_root(root);

        write_file(root / "src" / "hello.txt", "changed content\n");

        auto st = run("git_status", {{"path", root.string()}});
        check(has(st, "hello.txt"), "git_status: sees the modified file");

        auto df = run("git_diff", {{"path", root.string()}});
        check(has(df, "changed content"), "git_diff: shows the hunk");

        // A diff that overruns the 50 KB cap must warn it's incomplete and
        // tell the model NOT to apply_patch it — an incomplete patch can end
        // mid-hunk. Generate a big change to trip the cap.
        {
            std::string huge;
            for (int i = 0; i < 4000; ++i)
                huge += "new line " + std::to_string(i) +
                        " with enough text to add up past fifty kilobytes\n";
            write_file(root / "big.txt", huge);
            std::string add = "cd " + root.string() + " && git add -A";
            (void)std::system(add.c_str());
            auto bigdf = run("git_diff", {{"path", root.string()},
                                          {"staged", true}});
            // Either it fit (small env) or it truncated with the upgraded hint.
            check(!has(bigdf, "[output truncated]"),
                  "git_diff: no bare truncation marker");
            if (text_of(bigdf).find("truncated") != std::string::npos)
                check(has(bigdf, "apply_patch") && has(bigdf, "stat_only"),
                      "git_diff: truncated patch gives an actionable hint");
        }

        auto cm = run("git_commit", {{"message", "e2e commit"},
                                     {"stage_all", true},
                                     {"path", root.string()}});
        check(cm.has_value(), "git_commit: stages + commits");

        auto lg = run("git_log", {{"path", root.string()}, {"count", 5}});
        check(has(lg, "e2e commit"), "git_log: shows the new commit");

        auto shown = run("git_show", {{"ref", "HEAD"}, {"path", root.string()}});
        check(has(shown, "e2e commit"), "git_show: shows commit metadata and patch");

        auto blame = run("git_blame", {{"path", (root / "src" / "hello.txt").string()},
                                        {"start_line", 1}, {"end_line", 1}});
        check(has(blame, "changed content"), "git_blame: annotates a focused line range");
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
