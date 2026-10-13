// copilot_item_id_test — Copilot's proxy rewrites item_id; position doesn't.
//
// ── THE BUG, FROM THE USER'S LOG ─────────────────────────────────────────
//
// Every argument-taking tool failed with "[invalid args] path required"
// while zero-argument tools (list_dir, repo_map) worked. 45 tools
// advertised, 16 calls opened, and:
//
//     responses.tool_args_unroutable: item_id=+SqMJNRKVsofdaNo…  bytes=91
//     responses.tool_closed:          call_id=call_4D9…          args=0
//
// 48 unroutable frames for 16 calls — exactly 3 per call, one for each
// argument carrier (.added, .done, output_item.done), all with identical
// byte counts. So the server DID send the arguments, three times over, and
// we failed to attach any of them. All 16 calls closed with args=0.
//
// The unroutable item_ids were ~440-char base64 blobs that matched nothing
// the server had announced. That is Copilot's encrypted reasoning content:
// its proxy rewrites `item_id` on argument frames and the rewriting does
// not round-trip. Documented elsewhere as openclaw#72602 ("Encrypted
// content item_id did not match the target item id") and
// github/copilot-sdk#615.
//
// ── WHY IT WAS SILENT ────────────────────────────────────────────────────
//
// addressed_item() took any non-empty item_id as authoritative. A rewritten
// id is non-empty, so it won — and because it won, the sole() fallback that
// would have routed a single-call turn correctly never ran. The lookup
// missed, the payload was dropped, and the tool dispatched with `{}`.
//
// ── THE RULE ─────────────────────────────────────────────────────────────
//
//     Prefer output_index. An item_id that resolves to nothing is noise,
//     not authority.
//
// output_index is the item's slot in the response's output[] array — the
// one identity a proxy cannot rewrite without renumbering the array it is
// describing.

#include <string>
#include <vector>

#include "agtest.hpp"

#include "agentty/provider/chatgpt/responses.hpp"

using namespace agentty;
namespace cc = agentty::provider::chatgpt;

namespace {

template <class Leaf>
const Leaf* leaf(const Msg& m) {
    const Leaf* found = nullptr;
    std::visit([&](const auto& domain) {
        std::visit([&](const auto& l) {
            if constexpr (std::is_same_v<std::decay_t<decltype(l)>, Leaf>)
                found = &l;
        }, domain);
    }, m);
    return found;
}

// Everything the decoder accumulated for one call id.
std::string args_for(const std::vector<Msg>& msgs, std::string_view call_id) {
    std::string out;
    for (const auto& m : msgs)
        if (const auto* d = leaf<StreamToolUseDelta>(m))
            if (d->id.value == call_id) out += d->partial_json;
    return out;
}

// The blob Copilot substitutes for item_id — shape-accurate (long, base64),
// and critically it matches no announced item.
const char* kEncryptedBlob =
    "gAAAAABn5kQx7VqLmN0pRz8sW3bYcJ4fHtKoP1dXeR6uM9aQwZ2iT5vN8hL0gB7yC3rE"
    "6mK9pJ4sX1nW8zQ2vY5tA0cF7bH3dG6kM1oP9rS4uE8wI2xT5yV0aN6lJ3qR7fD1gC4b"
    "H8mK2pX5sW9nZ1vY6tB0eG7cJ4dF3kL9oQ2rT5uI8wA1xM6yN0pS4vE7bH3gD6kR9fC2"
    "jL8mW5nX1sZ4tY0aB7eK3dG6oJ9pQ2rF5uH8iT1wM4xN7yV0cS3bE6gL9kD2fR5jP8mA";

}  // namespace

TEST_CASE("copilot: arguments route by output_index when item_id is rewritten") {
    // One call, arguments delivered on the `.done` carrier with a rewritten
    // item_id — the exact shape from the log. output_index is intact.
    const std::vector<std::string> sse = {
        R"({"type":"response.output_item.added","output_index":0,"item":{"type":"function_call","id":"fc_real","call_id":"call_9","name":"read"}})",
        std::string{R"({"type":"response.function_call_arguments.done","output_index":0,"item_id":")"}
            + kEncryptedBlob + R"(","arguments":"{\"path\":\"a.txt\"}"})",
        R"({"type":"response.output_item.done","output_index":0,"item":{"type":"function_call","id":"fc_real"}})",
        R"({"type":"response.completed","response":{"usage":{}}})",
    };

    const auto msgs = cc::parse_sse_for_test(sse);

    // The arguments land. Before the fix this was empty and `read` failed
    // with "path required".
    CHECK(args_for(msgs, "call_9") == R"({"path":"a.txt"})");
}

TEST_CASE("copilot: parallel calls stay separate under rewritten item_ids") {
    // The case that makes position keying non-obvious: two calls in flight,
    // both with rewritten ids. Getting this wrong appends one call's
    // arguments to the other — valid JSON, wrong tool, silent.
    const std::string blob_a = std::string{kEncryptedBlob} + "AA";
    const std::string blob_b = std::string{kEncryptedBlob} + "BB";

    const std::vector<std::string> sse = {
        R"({"type":"response.output_item.added","output_index":0,"item":{"type":"function_call","id":"fc_a","call_id":"call_a","name":"read"}})",
        R"({"type":"response.output_item.added","output_index":1,"item":{"type":"function_call","id":"fc_b","call_id":"call_b","name":"grep"}})",
        // Arguments arrive out of order, each addressed only by position.
        R"({"type":"response.function_call_arguments.done","output_index":1,"item_id":")" + blob_b + R"(","arguments":"{\"pattern\":\"b\"}"})",
        R"({"type":"response.function_call_arguments.done","output_index":0,"item_id":")" + blob_a + R"(","arguments":"{\"path\":\"a.txt\"}"})",
        R"({"type":"response.output_item.done","output_index":0,"item":{"type":"function_call","id":"fc_a"}})",
        R"({"type":"response.output_item.done","output_index":1,"item":{"type":"function_call","id":"fc_b"}})",
        R"({"type":"response.completed","response":{"usage":{}}})",
    };

    const auto msgs = cc::parse_sse_for_test(sse);

    CHECK(args_for(msgs, "call_a") == R"({"path":"a.txt"})");
    CHECK(args_for(msgs, "call_b") == R"({"pattern":"b"})");
}

TEST_CASE("copilot: a rewritten item_id alone still routes a lone call") {
    // No output_index anywhere and one call open. The old code took the
    // rewritten id as authoritative, missed, and dropped the payload —
    // the sole() fallback never got a chance. An id that resolves to
    // nothing must not suppress it.
    const std::vector<std::string> sse = {
        R"({"type":"response.output_item.added","item":{"type":"function_call","id":"fc_real","call_id":"call_9","name":"read"}})",
        std::string{R"({"type":"response.function_call_arguments.done","item_id":")"}
            + kEncryptedBlob + R"(","arguments":"{\"path\":\"a.txt\"}"})",
        R"({"type":"response.output_item.done","item":{"type":"function_call","id":"fc_real"}})",
        R"({"type":"response.completed","response":{"usage":{}}})",
    };

    const auto msgs = cc::parse_sse_for_test(sse);

    CHECK(args_for(msgs, "call_9") == R"({"path":"a.txt"})");
}

TEST_CASE("copilot: a good item_id still wins over position") {
    // The fix must not regress well-behaved servers. Here item_id is
    // correct and output_index DISAGREES with it (a server that renumbers
    // between frames); the id names a real open call, so it must be used.
    const std::vector<std::string> sse = {
        R"({"type":"response.output_item.added","output_index":0,"item":{"type":"function_call","id":"fc_a","call_id":"call_a","name":"read"}})",
        R"({"type":"response.output_item.added","output_index":1,"item":{"type":"function_call","id":"fc_b","call_id":"call_b","name":"grep"}})",
        // Position says slot 1, but the id explicitly names fc_a.
        R"({"type":"response.function_call_arguments.done","item_id":"fc_a","arguments":"{\"path\":\"a.txt\"}"})",
        R"({"type":"response.output_item.done","output_index":0,"item":{"type":"function_call","id":"fc_a"}})",
        R"({"type":"response.output_item.done","output_index":1,"item":{"type":"function_call","id":"fc_b"}})",
        R"({"type":"response.completed","response":{"usage":{}}})",
    };

    const auto msgs = cc::parse_sse_for_test(sse);

    CHECK(args_for(msgs, "call_a") == R"({"path":"a.txt"})");
    CHECK(args_for(msgs, "call_b").empty());
}

TEST_CASE("copilot: an unknown position with several calls open is not guessed") {
    // The discipline this dialect holds everywhere: when the server names a
    // slot we do not have and more than one call is open, there is no
    // honest answer. Dropping the fragment is correct; picking one appends
    // another call's bytes and still parses.
    //
    // This is also the boundary of salvage (below). Salvage recovers only
    // where recovery cannot be wrong — exactly one candidate. Two
    // candidates is a coin flip, and a coin flip that produces valid JSON
    // is worse than a visibly broken turn.
    const std::vector<std::string> sse = {
        R"({"type":"response.output_item.added","output_index":0,"item":{"type":"function_call","id":"fc_a","call_id":"call_a","name":"read"}})",
        R"({"type":"response.output_item.added","output_index":1,"item":{"type":"function_call","id":"fc_b","call_id":"call_b","name":"grep"}})",
        // Slot 7 was never announced, and the id is a rewritten blob.
        std::string{R"({"type":"response.function_call_arguments.done","output_index":7,"item_id":")"}
            + kEncryptedBlob + R"(","arguments":"{\"path\":\"x\"}"})",
        R"({"type":"response.completed","response":{"usage":{}}})",
    };

    const auto msgs = cc::parse_sse_for_test(sse);

    CHECK(args_for(msgs, "call_a").empty());
    CHECK(args_for(msgs, "call_b").empty());
}

TEST_CASE("copilot: unroutable args are salvaged when exactly one call is open") {
    // GitHub's client ships `copilot_salvaged_tool_input_ids` in telemetry:
    // it reconstructs tool inputs that fail to route, and reports how often
    // it had to. You do not build that unless it fires in production
    // against your own servers — so argument loss on a proxied wire is an
    // EXPECTED state, not an exceptional one.
    //
    // The carrier that needs this is `output_item.done`. Its arguments are
    // addressed by the item's OWN `id` field, not by addressed_item(), so
    // the sole()-fallback that rescues the `.delta`/`.done` carriers does
    // not apply. If the proxy rewrites the id here — exactly what it does
    // to item_id on argument frames — the payload has no owner and the
    // call dispatches with `{}`.
    //
    // One call open means there is exactly one thing these bytes can belong
    // to and no order to get wrong. Recover them.
    const std::vector<std::string> sse = {
        R"({"type":"response.output_item.added","item":{"type":"function_call","id":"fc_real","call_id":"call_9","name":"read"}})",
        // The completed item restates the arguments under a REWRITTEN id.
        std::string{R"({"type":"response.output_item.done","item":{"type":"function_call","id":")"}
            + kEncryptedBlob + R"(","arguments":"{\"path\":\"a.txt\"}"}})",
        R"({"type":"response.completed","response":{"usage":{}}})",
    };

    const auto msgs = cc::parse_sse_for_test(sse);

    // Without salvage this is `read` dispatched with `{}` — "[invalid args]
    // path required" — while the arguments sat in the log.
    CHECK(args_for(msgs, "call_9") == R"({"path":"a.txt"})");
}

TEST_CASE("copilot: salvage refuses rather than guessing between two calls") {
    // The same unroutable payload, but now two calls are open. Salvage must
    // NOT fire: either choice produces JSON that parses and dispatches, so
    // a wrong guess is silent corruption — `read` running with `grep`'s
    // arguments, or the reverse.
    //
    // Refusing costs one broken turn and says so in the log. Guessing costs
    // a tool running on the wrong input with nobody the wiser. The asymmetry
    // is the whole reason OpenCalls::sole() returns nullopt for "many"
    // instead of handing back the first element.
    const std::vector<std::string> sse = {
        R"({"type":"response.output_item.added","item":{"type":"function_call","id":"fc_a","call_id":"call_a","name":"read"}})",
        R"({"type":"response.output_item.added","item":{"type":"function_call","id":"fc_b","call_id":"call_b","name":"grep"}})",
        std::string{R"({"type":"response.output_item.done","item":{"type":"function_call","id":"unknown_)"}
            + kEncryptedBlob + R"(","arguments":"{\"path\":\"a.txt\"}"}})",
        R"({"type":"response.completed","response":{"usage":{}}})",
    };

    const auto msgs = cc::parse_sse_for_test(sse);

    // Neither call receives the orphaned bytes.
    CHECK(args_for(msgs, "call_a").empty());
    CHECK(args_for(msgs, "call_b").empty());
}
