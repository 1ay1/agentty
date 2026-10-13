// mcp_result_type_test — the `resultType` field is REQUIRED from protocol
// 2026-07-28, which is the version we advertise.
//
// The spec is unusually explicit about both halves:
//
//   "Servers implementing this protocol version MUST include this field."
//
//   "For backward compatibility, when a client receives a result from a
//    server implementing an earlier protocol version (which does not include
//    `resultType`), the client MUST treat the absent field as `complete`."
//
// So this is two contracts, not one: always EMIT it, and tolerate its
// ABSENCE when decoding. A codec that merely has the field satisfies
// neither — `Maybe<std::string>` would compile, omit the key on the wire,
// and quietly break every 2026-07-28 client.
//
// Worth a dedicated test because a missing required field is invisible
// locally: our own client defaults it, so agentty-talking-to-agentty works
// perfectly while a conformant third-party client rejects the response.

#include <mcp/methods.hpp>

#include <string>

#include "agtest.hpp"

using namespace mcp;

namespace {
// Every result type the spec marks `required: [..., "resultType", ...]`.
template <class T>
void check_emits_result_type(const char* what) {
    T v{};
    const Json j = codec<T>().encode(v);
    INFO(what);
    CHECK(j.contains("resultType"));
    if (j.contains("resultType")) CHECK(j["resultType"] == "complete");
}

template <class T>
void check_absent_decodes_complete(const char* what, Json minimal) {
    INFO(what);
    // A pre-2026-07-28 server omits the field entirely.
    CHECK(!minimal.contains("resultType"));
    const T v = codec<T>().decode(minimal);
    CHECK(v.resultType == "complete");
}
} // namespace

TEST_CASE("mcp: every result emits resultType") {
    // A default-constructed result must already carry it — call sites do not
    // have to remember, which is the only way this stays true as new ones
    // are added.
    check_emits_result_type<CallToolResult>("CallToolResult");
    check_emits_result_type<ListToolsResult>("ListToolsResult");
    check_emits_result_type<ListPromptsResult>("ListPromptsResult");
    check_emits_result_type<GetPromptResult>("GetPromptResult");
    check_emits_result_type<ListResourcesResult>("ListResourcesResult");
    check_emits_result_type<ReadResourceResult>("ReadResourceResult");
    check_emits_result_type<ListResourceTemplatesResult>("ListResourceTemplatesResult");
    check_emits_result_type<CompleteResult>("CompleteResult");
}

TEST_CASE("mcp: an absent resultType decodes as complete") {
    // The spec's backward-compat rule, verbatim. An older server sends no
    // such field and the client MUST read it as "complete" rather than
    // empty — an empty string here would make a valid response look like an
    // unknown result type.
    check_absent_decodes_complete<CallToolResult>(
        "CallToolResult", Json{{"content", Json::array()}});
    check_absent_decodes_complete<ListToolsResult>(
        "ListToolsResult", Json{{"tools", Json::array()}});
    check_absent_decodes_complete<CompleteResult>(
        "CompleteResult",
        Json{{"completion", Json{{"values", Json::array()}}}});
}

TEST_CASE("mcp: a server-sent resultType survives the round trip") {
    // "input_required" is the other value the spec names — the server needs
    // more from the user before it can finish. Decoding it as "complete"
    // would make the client stop waiting and treat a half-answer as final.
    Json j{{"content", Json::array()}, {"resultType", "input_required"}};
    const auto v = codec<CallToolResult>().decode(j);
    CHECK(v.resultType == "input_required");
    // And it survives re-encoding, so a proxy that decodes and forwards
    // doesn't silently downgrade it.
    CHECK(codec<CallToolResult>().encode(v)["resultType"] == "input_required");
}
