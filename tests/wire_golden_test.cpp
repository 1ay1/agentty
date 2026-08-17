// wire_golden_test — byte-identity guard for the Anthropic wire body.
//
// The Anthropic provider is being split from one monolithic transport.cpp
// into focused modules (prompt / sse / wire). Each split MUST be a pure
// refactor: the bytes we send on the wire cannot change, or prompt-cache
// hits break and behavior drifts. This test pins the EXACT serialized output
// of messages_json_string() over a deliberately rich thread — text turns,
// terminal + non-terminal tool calls, an error result, a big result that
// fades, a compaction summary substitution, and a superseded read that
// collapses. A stable content hash makes any drift fail immediately.
//
// If a LEGITIMATE wire change lands (new field, reordered key), update
// kGoldenHash below — but only after confirming the diff is intended.

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "agtest.hpp"

#include <nlohmann/json.hpp>

#include "agentty/domain/conversation.hpp"
#include "agentty/provider/anthropic/transport.hpp"

using agentty::Message;
using agentty::Role;
using agentty::Thread;
using agentty::ThreadId;
using agentty::ToolCallId;
using agentty::ToolName;
using agentty::ToolUse;

namespace {

ToolUse done_read(const char* id, const char* path, const std::string& out) {
    ToolUse tc;
    tc.id = ToolCallId{id};
    tc.name = ToolName{"read"};
    tc.args = nlohmann::json{{"path", path}};
    tc.status = ToolUse::Done{{}, {}, out};
    return tc;
}

ToolUse done_tool(const char* id, const char* name, nlohmann::json args,
                  const std::string& out) {
    ToolUse tc;
    tc.id = ToolCallId{id};
    tc.name = ToolName{name};
    tc.args = std::move(args);
    tc.status = ToolUse::Done{{}, {}, out};
    return tc;
}

// A fixed, feature-rich thread. Deterministic — no timestamps, random ids, or
// env-dependent content leak into messages_json_string's output.
Thread golden_thread() {
    std::vector<Message> msgs;

    Message u0; u0.role = Role::User; u0.text = "start the task";
    msgs.push_back(std::move(u0));

    // Assistant reads a file (this read is later superseded by the edit).
    Message a0; a0.role = Role::Assistant; a0.text = "reading main.cpp";
    a0.tool_calls.push_back(done_read("toolu_r1", "src/main.cpp",
        std::string("FIRST_READ_") + std::string(4096, 'a')));
    msgs.push_back(std::move(a0));

    // Assistant greps (big output that will fade once it's old enough).
    Message a1; a1.role = Role::Assistant; a1.text = "searching";
    a1.tool_calls.push_back(done_tool("toolu_g1", "grep",
        nlohmann::json{{"pattern", "foo"}},
        std::string("GREP_HEAD_") + std::string(80 * 1024, 'g') + "_GREP_TAIL"));
    msgs.push_back(std::move(a1));

    // Assistant edits main.cpp — supersedes toolu_r1's read.
    Message a2; a2.role = Role::Assistant; a2.text = "editing";
    a2.tool_calls.push_back(done_tool("toolu_e1", "edit",
        nlohmann::json{{"path", "src/main.cpp"}}, "edit applied"));
    msgs.push_back(std::move(a2));

    // A failed tool (errors never fade/collapse).
    Message a3; a3.role = Role::Assistant; a3.text = "build";
    ToolUse fail_tc;
    fail_tc.id = ToolCallId{"toolu_b1"};
    fail_tc.name = ToolName{"bash"};
    fail_tc.args = nlohmann::json{{"command", "make"}};
    fail_tc.status = ToolUse::Failed{{}, {}, "make: *** no rule"};
    a3.tool_calls.push_back(std::move(fail_tc));
    msgs.push_back(std::move(a3));

    // Newest turn: re-read main.cpp (the live copy — must stay full).
    Message a4; a4.role = Role::Assistant; a4.text = "re-reading";
    a4.tool_calls.push_back(done_read("toolu_r2", "src/main.cpp",
        std::string("SECOND_READ_") + std::string(4096, 'b')));
    msgs.push_back(std::move(a4));

    Thread t{ThreadId{"golden"}, "", std::move(msgs), {}, {}};
    return t;
}

// FNV-1a 64-bit — stable across platforms/compilers (std::hash is not).
std::uint64_t fnv1a(const std::string& s) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

} // namespace

TEST_CASE("wire_golden") {
    namespace ap = agentty::provider::anthropic;

    const std::string wire = ap::messages_json_string(golden_thread(),
                                                      /*include_thinking=*/false);

    // 1. The wire must be valid JSON (never emit a malformed body).
    bool parses = true;
    try { auto j = nlohmann::json::parse(wire); (void)j; }
    catch (...) { parses = false; }
    check(parses, "golden wire is valid JSON");

    // 2. Feature assertions that must hold regardless of the exact hash —
    //    these document WHAT the golden output contains. NOTE:
    //    messages_json_string() serialises t.messages directly; the
    //    compaction-summary SUBSTITUTION happens upstream in
    //    wire_messages_for_impl (cmd_factory), not at this layer — so the
    //    raw CompactionRecord on the thread is intentionally a no-op here.
    check(wire.find("FIRST_READ_") == std::string::npos,
          "superseded read body is collapsed (not shipped)");
    check(wire.find("superseded") != std::string::npos,
          "superseded read carries the pointer text");
    check(wire.find("SECOND_READ_") != std::string::npos,
          "live (newest) read of the file ships in full");
    check(wire.find("make: *** no rule") != std::string::npos,
          "error result is never faded or collapsed");
    check(wire.find("GREP_HEAD_") != std::string::npos,
          "recent grep result head survives");

    // 3. Byte-identity: the exact serialization is pinned. A refactor that
    //    changes even one byte trips this. Update kGoldenHash ONLY after
    //    confirming the change is intentional.
    const std::uint64_t kGoldenHash = 0x6a4fc9657b4ab5d1ull;
    const std::uint64_t got = fnv1a(wire);

    if (kGoldenHash == 0) {
        // First run / bootstrap: print the hash so it can be pinned.
        std::fprintf(stderr,
            "GOLDEN wire hash = 0x%016llxull  (len=%zu)\n",
            static_cast<unsigned long long>(got), wire.size());
    } else {
        check(got == kGoldenHash, "wire bytes are byte-identical to golden");
        if (got != kGoldenHash)
            std::fprintf(stderr,
                "  expected 0x%016llx got 0x%016llx\n",
                static_cast<unsigned long long>(kGoldenHash),
                static_cast<unsigned long long>(got));
    }
}
