// scheduler_path_test — path-aware parallel tool scheduling
// (cmd::schedule_parallel_batch in cmd_factory.cpp).
//
//   agentty already gates parallel tool execution by EffectSet (read/net
//   compose; write/exec demand exclusive access). This test pins the PATH-
//   AWARE refinement: two writers to disjoint files run in the same wave, a
//   read alongside an unrelated write runs concurrently, and overlapping /
//   exec / blind-writer cases still serialise. Pure planner — no maya, no
//   phase machinery.

#include "agentty/runtime/app/cmd_factory.hpp"
#include "agentty/domain/conversation.hpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using agentty::ToolUse;
using agentty::ToolName;
using agentty::ToolCallId;
using agentty::app::cmd::schedule_parallel_batch;

namespace {
int failures = 0, total = 0;
void check(const char* name, bool cond, const std::string& detail = {}) {
    ++total;
    if (cond) std::printf("  ok   %s\n", name);
    else { ++failures; std::printf("  FAIL %s — %s\n", name, detail.c_str()); }
}

// A PENDING tool call with the given name + args.
ToolUse pending(const char* name, nlohmann::json args) {
    ToolUse t;
    static int c = 0;
    t.id   = ToolCallId{"c" + std::to_string(++c)};
    t.name = ToolName{name};
    t.args = std::move(args);
    t.status = ToolUse::Pending{std::chrono::steady_clock::now()};
    return t;
}
ToolUse running(const char* name, nlohmann::json args) {
    ToolUse t = pending(name, std::move(args));
    t.status = ToolUse::Running{std::chrono::steady_clock::now(), {}};
    return t;
}

std::string promoted_str(const std::vector<std::size_t>& v) {
    std::string s = "{";
    for (auto i : v) { s += std::to_string(i); s += ','; }
    s += "}";
    return s;
}
bool has(const std::vector<std::size_t>& v, std::size_t i) {
    for (auto x : v) if (x == i) return true;
    return false;
}
} // namespace

int main() {
    std::printf("scheduler_path_test — path-aware parallel scheduling\n\n");

    // (a) Two writes to DISJOINT files → BOTH promoted (run concurrently).
    {
        std::vector<ToolUse> b = {
            pending("write", {{"file_path", "a.cpp"}, {"content", "x"}}),
            pending("write", {{"file_path", "b.cpp"}, {"content", "y"}}),
        };
        auto d = schedule_parallel_batch(b);
        check("disjoint writes both run", d.promote.size() == 2,
              promoted_str(d.promote));
    }

    // (b) Two writes to the SAME file → only the FIRST runs (serialise).
    {
        std::vector<ToolUse> b = {
            pending("write", {{"file_path", "same.cpp"}, {"content", "x"}}),
            pending("edit",  {{"path", "same.cpp"}}),
        };
        auto d = schedule_parallel_batch(b);
        check("same-file writes serialise", d.promote.size() == 1 && has(d.promote, 0),
              promoted_str(d.promote));
    }

    // (c) Read of a.cpp + write of b.cpp → BOTH run (disjoint, even though
    //     one writes — the coarse effect rule would have serialised them).
    {
        std::vector<ToolUse> b = {
            pending("read",  {{"path", "a.cpp"}}),
            pending("write", {{"file_path", "b.cpp"}, {"content", "y"}}),
        };
        auto d = schedule_parallel_batch(b);
        check("read + disjoint write run together", d.promote.size() == 2,
              promoted_str(d.promote));
    }

    // (d) Read of a.cpp + write of a.cpp → only the read runs (overlap).
    {
        std::vector<ToolUse> b = {
            pending("read",  {{"path", "a.cpp"}}),
            pending("write", {{"file_path", "a.cpp"}, {"content", "y"}}),
        };
        auto d = schedule_parallel_batch(b);
        check("read + same-file write serialise", d.promote.size() == 1 && has(d.promote, 0),
              promoted_str(d.promote));
    }

    // (e) Directory-prefix overlap: edit "src/a.c" vs write "src/" → serialise.
    {
        std::vector<ToolUse> b = {
            pending("edit",  {{"path", "src/a.c"}}),
            pending("write", {{"file_path", "src"}, {"content", "x"}}),
        };
        auto d = schedule_parallel_batch(b);
        check("dir-prefix overlap serialises", d.promote.size() == 1,
              promoted_str(d.promote));
    }

    // (f) "srcfoo" is NOT under "src" → boundary-correct, both run.
    {
        std::vector<ToolUse> b = {
            pending("write", {{"file_path", "src"}, {"content", "x"}}),
            pending("write", {{"file_path", "srcfoo"}, {"content", "y"}}),
        };
        auto d = schedule_parallel_batch(b);
        check("srcfoo not under src", d.promote.size() == 2, promoted_str(d.promote));
    }

    // (g) bash (Exec) anywhere in the batch → it serialises against everything.
    //     A read emitted BEFORE bash runs; bash + the later read stay pending.
    {
        std::vector<ToolUse> b = {
            pending("read", {{"path", "a.cpp"}}),
            pending("bash", {{"command", "rm -rf build"}}),
            pending("read", {{"path", "b.cpp"}}),
        };
        auto d = schedule_parallel_batch(b);
        // 0 (read) runs; 1 (bash) conflicts with the running read → stays
        // pending; 2 (read) would be read-safe but bash hasn't run — it's
        // read-compatible with the active read so it ALSO runs. bash alone defers.
        check("bash defers, reads run", has(d.promote, 0) && has(d.promote, 2)
              && !has(d.promote, 1), promoted_str(d.promote));
    }

    // (h) Blind writer (no extractable path) → exclusive, serialises.
    {
        std::vector<ToolUse> b = {
            pending("read",  {{"path", "a.cpp"}}),
            pending("write", {{"content", "no path here"}}),   // no file_path
        };
        auto d = schedule_parallel_batch(b);
        check("blind writer serialises", d.promote.size() == 1 && has(d.promote, 0),
              promoted_str(d.promote));
    }

    // (i) Many disjoint reads → all run (the common fan-out case).
    {
        std::vector<ToolUse> b = {
            pending("read", {{"path", "a"}}),
            pending("read", {{"path", "b"}}),
            pending("read", {{"path", "c"}}),
            pending("grep", {{"pattern", "foo"}}),
        };
        auto d = schedule_parallel_batch(b);
        check("read fan-out all parallel", d.promote.size() == 4, promoted_str(d.promote));
    }

    // (j) A RUNNING write to a.cpp blocks a pending read of a.cpp but not a
    //     pending read of b.cpp.
    {
        std::vector<ToolUse> b = {
            running("write", {{"file_path", "a.cpp"}, {"content", "x"}}),
            pending("read",  {{"path", "a.cpp"}}),   // blocked
            pending("read",  {{"path", "b.cpp"}}),   // free
        };
        auto d = schedule_parallel_batch(b);
        check("running write blocks overlapping read only",
              !has(d.promote, 1) && has(d.promote, 2), promoted_str(d.promote));
    }

    // (k) task FAN-OUT: several subagents in one wave ALL run concurrently.
    //     This is the tool's whole point — sched_effects maps task to
    //     {ReadFs, Net} (composable) while its PERMISSION gate stays Exec.
    {
        std::vector<ToolUse> b = {
            pending("task", {{"prompt", "explore module A"}}),
            pending("task", {{"prompt", "explore module B"}}),
            pending("task", {{"prompt", "review the diff"}}),
        };
        auto d = schedule_parallel_batch(b);
        check("task fan-out all parallel", d.promote.size() == 3,
              promoted_str(d.promote));
    }

    // (l) task + reads compose (a subagent runs alongside direct reads).
    {
        std::vector<ToolUse> b = {
            pending("task", {{"prompt", "map the codebase"}}),
            pending("read", {{"path", "a.cpp"}}),
            pending("grep", {{"pattern", "foo"}}),
        };
        auto d = schedule_parallel_batch(b);
        check("task + reads run together", d.promote.size() == 3,
              promoted_str(d.promote));
    }

    // (m) task does NOT ride alongside bash: Exec is exclusive and task's
    //     sched view still carries ReadFs/Net which conflict with Exec.
    {
        std::vector<ToolUse> b = {
            pending("bash", {{"command", "make -j8"}}),
            pending("task", {{"prompt", "explore while building"}}),
        };
        auto d = schedule_parallel_batch(b);
        check("bash excludes task", d.promote.size() == 1 && has(d.promote, 0),
              promoted_str(d.promote));
    }

    std::printf("\n%d/%d checks passed\n", total - failures, total);
    return failures == 0 ? 0 : 1;
}
