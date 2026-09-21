// responses_log_test — the Responses dialect must be debuggable from the log.
//
// ── WHY ──────────────────────────────────────────────────────────────────
//
// A Copilot turn came back with every argument-taking tool failing
// ("[invalid args] path required") while zero-argument tools succeeded.
// That asymmetry means arguments were lost, not that the model was weak.
//
// The log could not settle where. The Chat transport dumps its request
// (`openai.request` + `openai.request.body`) and its decode decisions, but
// the Responses codec — the transport Copilot uses for gpt-* models — logged
// only failures. A healthy-looking stream that produced empty arguments left
// no trace at all, so "we never sent the tools", "the server sent no
// arguments", and "we failed to route them" were indistinguishable without
// attaching a debugger to the user's machine.
//
// ── THE RULE ─────────────────────────────────────────────────────────────
//
// Every tool call on this dialect leaves a trail through its whole life:
//
//     responses.request            what we asked for (incl. tool count)
//     responses.tool_open          item_id ↔ call_id ↔ name
//     responses.tool_args_routed   which carrier delivered bytes, how many
//     responses.tool_closed        what the call ended up with
//
// Given those four, an empty-arguments report is a lookup rather than a
// guess. This test pins that they exist and carry the identifiers that make
// them joinable, because a log line is trivially deletable and nothing else
// in the suite would notice.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "agtest.hpp"

#include "agentty/provider/chatgpt/responses.hpp"
#include "agentty/util/logx.hpp"

using namespace agentty;
namespace cc = agentty::provider::chatgpt;

namespace {

// Drive the real codec, then read back what the flight recorder captured.
//
// The ring records Warn+ always, and everything when the build/filter asks
// (debug builds default to Trace — see logx::init). dump_flight_recorder_to
// is the supported way to get the slots out, and it is what the crash
// handler and `agentty diagnostics` both use, so testing through it means
// testing the path a real bug report travels.
// logx::detail::init() runs exactly once, from a magic static on the first
// log call anywhere in the process. So the environment has to be set before
// that — not inside the test body, which may well run after some other test
// has already logged. A namespace-scope initialiser gets us in ahead of
// main(), and therefore ahead of any TEST_CASE.
//
// wire=debug is precisely what a user debugging this would run, so the test
// exercises the same filter path a real bug report travels.
const bool g_log_env = [] {
    ::setenv("AGENTTY_LOG", "wire=debug", /*overwrite=*/1);
    return true;
}();

std::string run_and_capture(const std::vector<std::string>& sse) {
    REQUIRE(g_log_env);

    const auto path = std::filesystem::temp_directory_path()
                    / "agentty_responses_log_test.txt";
    std::filesystem::remove(path);

    // The flight ring is process-wide and cumulative — it has no reset, by
    // design (a crash wants everything). So stamp a unique marker, run the
    // turn, and keep only what lands after the marker. Without this the
    // second test reads the first test's lines and "no arguments were
    // routed" is unprovable.
    static int seq = 0;
    ++seq;
    // The dump line is "<meta> <channel> <site>: <message>", so the joint
    // of site and message is ": " — match on that, not a bare space.
    const std::string mark = "responses_log_test.mark: seq="
                           + std::to_string(seq);
    AGT_LOG(Wire, Debug, "responses_log_test.mark", "seq={}", seq);

    (void)cc::parse_sse_for_test(sse);
    REQUIRE(logx::dump_flight_recorder_to(path.string().c_str()));

    std::ifstream in(path);
    std::string dump((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    std::filesystem::remove(path);

    const auto at = dump.rfind(mark);
    REQUIRE(at != std::string::npos);
    return dump.substr(at);
}

// A Copilot-shaped turn: Copilot does NOT stream argument fragments, it
// coalesces and sends one `.done` snapshot. That is the exact shape the
// original bug hid in, so it is the shape worth pinning.
const std::vector<std::string> kHealthyTurn = {
    R"({"type":"response.output_item.added","item":{"type":"function_call","id":"fc_1","call_id":"call_9","name":"read"}})",
    R"({"type":"response.function_call_arguments.done","item_id":"fc_1","arguments":"{\"path\":\"a.txt\"}"})",
    R"({"type":"response.output_item.done","item":{"type":"function_call","id":"fc_1"}})",
    R"({"type":"response.completed","response":{"usage":{}}})",
};

// The failure the user actually hit: the call opens and closes, but no
// arguments arrive on any carrier.
const std::vector<std::string> kEmptyArgsTurn = {
    R"({"type":"response.output_item.added","item":{"type":"function_call","id":"fc_1","call_id":"call_9","name":"read"}})",
    R"({"type":"response.output_item.done","item":{"type":"function_call","id":"fc_1"}})",
    R"({"type":"response.completed","response":{"usage":{}}})",
};

bool has(const std::string& hay, std::string_view needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("responses: a tool call's whole life is in the log") {
    const auto dump = run_and_capture(kHealthyTurn);

    // Opening: the identity pairing. item_id is what later argument frames
    // address; call_id is what the reducer dispatches on. When they differ
    // — they do on Copilot — a mismatch here is precisely the difference
    // between arguments landing and going to tool_args_unroutable.
    CHECK(has(dump, "responses.tool_open"));
    CHECK(has(dump, "item_id=fc_1"));
    CHECK(has(dump, "call_id=call_9"));
    CHECK(has(dump, "name=read"));

    // Routing: which carrier delivered bytes, and whether they were new.
    // On Copilot this is the single line for the call — total=1 (a
    // snapshot, not a fragment) carrying the whole argument string.
    CHECK(has(dump, "responses.tool_args_routed"));
    CHECK(has(dump, "total=1"));

    // Closing: the verdict, with the arguments the call actually ended up
    // with. Non-zero here is a healthy turn.
    CHECK(has(dump, "responses.tool_closed"));
    CHECK(!has(dump, "args=0"));
}

TEST_CASE("responses: the empty-arguments failure is visible in the log") {
    const auto dump = run_and_capture(kEmptyArgsTurn);

    // The call was announced…
    CHECK(has(dump, "responses.tool_open"));

    // …no carrier ever routed bytes…
    CHECK(!has(dump, "responses.tool_args_routed"));

    // …and the close records that it ended with nothing. Those three facts
    // together say "the server sent no arguments", which is a different bug
    // from "we dropped them" and points at a different fix. Without this
    // line the two are indistinguishable from a bug report.
    CHECK(has(dump, "responses.tool_closed"));
    CHECK(has(dump, "args=0"));
}
