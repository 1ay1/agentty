// empty_tool_args_test — "the model sent no arguments" is not "the arguments
// failed to parse".
//
// ── THE BUG THIS PINS ────────────────────────────────────────────────────
//
// A tool whose parameters are all optional — `git_status`, `list_dir`,
// `repo_map` — is legitimately called with no arguments. On every
// OpenAI-compatible wire that call arrives as a tool_calls[] entry with
// `function.arguments` absent, `""`, or `"{}"`: zero argument deltas reach
// the decoder, so the accumulated buffer is empty at the end of the call.
//
// Both agent loops then decide what `ToolUse::args` holds. The main reducer
// seeds `json::object()` at StreamToolUseStart, so an empty buffer leaves a
// valid `{}` and the tool runs. The subagent loop did not seed anything, so
// `args` stayed at nlohmann's default — `null` — and the dispatcher reads a
// null `args` as its parse-failure signal:
//
//     "tool args failed to parse — re-emit the call with complete, valid
//      JSON arguments"
//
// That message goes back to the model as a tool_result. The model re-emits
// the same argumentless call, gets the same error, and burns its turn budget
// on a call that was correct the first time. Nothing in the log says
// "argumentless"; it reads as a model that cannot produce valid JSON.
//
// ── THE RULE ─────────────────────────────────────────────────────────────
//
//     ABSENT arguments are an empty object.
//     Only a FAILED parse may leave args null.
//
// The null sentinel has to keep meaning exactly one thing, because the
// dispatcher's error message is written on the assumption that it does.
//
// ── WHY A SOURCE SCAN, NOT A BEHAVIOURAL TEST ────────────────────────────
//
// The behavioural version needs a full subagent turn: a provider, a stream,
// a dispatcher, a tool registry. That test would pass today and would not
// notice a THIRD loop — an ACP adapter, a headless `run` path — repeating
// the same omission, because it only exercises the two that exist.
//
// The shape is what recurs: a site that builds a ToolUse from a
// StreamToolUseStart and never seeds `args`. So this asserts the invariant
// at each such site, the way attribution_discipline_test asserts that no
// decoder picks a tool call's owner arbitrarily.

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "agtest.hpp"

#include "agentty/domain/conversation.hpp"

namespace {

namespace fs = std::filesystem;

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

// Walk up from the test binary's CWD looking for the repo root.
fs::path repo_root() {
    auto dir = fs::current_path();
    for (int up = 0; up < 6; ++up) {
        if (fs::exists(dir / "src" / "provider") && fs::exists(dir / "tests"))
            return dir;
        if (!dir.has_parent_path()) break;
        dir = dir.parent_path();
    }
    return {};
}

// Every site that materialises a ToolUse from a stream's tool-use START.
// If a new agent loop appears, add it here — the list is the point.
const char* kToolUseConstructionSites[] = {
    "src/runtime/app/update/stream.cpp",   // main reducer
    "src/tool/mcp_tools_backends.cpp",     // subagent loop
};

}  // namespace

TEST_CASE("empty tool args: nlohmann's default really is null") {
    // The whole bug rests on this. If nlohmann ever defaulted to an empty
    // object, seeding would be redundant and the dispatcher's null check
    // would be dead code — so state the assumption rather than rely on it.
    agentty::ToolUse tc;
    CHECK(tc.args.is_null());
    CHECK(!tc.args.is_object());
}

TEST_CASE("empty tool args: a blank buffer is distinguishable from bad JSON") {
    // The two cases the dispatcher must tell apart, at the level of the
    // decision each loop makes on the accumulated argument buffer.
    auto decide = [](std::string_view buf) -> nlohmann::json {
        // "No arguments at all" — absent, empty, or whitespace. Providers
        // send all three for the same thing, so they must land together.
        if (buf.find_first_not_of(" \t\r\n") == std::string_view::npos)
            return nlohmann::json::object();
        try {
            return nlohmann::json::parse(buf);
        } catch (...) {
            return nlohmann::json{};  // null = genuinely unparseable
        }
    };

    // Absent / empty / whitespace / explicit empty object: all one thing.
    CHECK(decide("").is_object());
    CHECK(decide("   ").is_object());
    CHECK(decide("\n").is_object());
    CHECK(decide("{}").is_object());

    // A real payload survives.
    CHECK(decide(R"({"path":"a.txt"})")["path"] == "a.txt");

    // And only a genuine parse failure produces the null sentinel — the
    // truncated-mid-stream case the dispatcher's message is written for.
    CHECK(decide(R"({"path":"a.tx)").is_null());
    CHECK(decide("not json at all").is_null());
}

TEST_CASE("empty tool args: every ToolUse construction site seeds an object") {
    const auto root = repo_root();
    REQUIRE_MESSAGE(!root.empty(),
                    "could not locate the repo root from the test's CWD");

    for (const char* rel : kToolUseConstructionSites) {
        const auto path = root / rel;
        const auto src  = read_file(path);
        REQUIRE_MESSAGE(!src.empty(), "unreadable test source: " << rel);

        // The site builds a ToolUse from a tool-use start...
        const bool constructs =
            src.find("StreamToolUseStart") != std::string::npos;
        if (!constructs) continue;

        // ...so it must seed args with an object. Both spellings are in
        // use (`tc.args = json::object()` in the loops that build a local
        // ToolUse; `args = nlohmann::json::object()` if one is written out
        // in full), and either satisfies the rule.
        const bool seeds =
            src.find("args   = json::object()") != std::string::npos
            || src.find("args = json::object()") != std::string::npos
            || src.find("args = nlohmann::json::object()") != std::string::npos;

        CHECK_MESSAGE(seeds,
            rel << ": builds a ToolUse from StreamToolUseStart but never "
                   "seeds `args` with an object. An argumentless tool call "
                   "(git_status, list_dir) then leaves args null, which the "
                   "dispatcher reports to the model as \"tool args failed to "
                   "parse\" \u2014 a correct call rejected as malformed. Seed "
                   "`tc.args = json::object()` at the start site; leave null "
                   "to mean a FAILED parse and nothing else.");
    }
}
