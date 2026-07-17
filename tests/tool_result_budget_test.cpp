// tool_result_budget_test — wire-only byte-budget truncation for tool
// results. A single oversized tool output (500 KiB grep, dump of a big
// file) must NOT ship verbatim on the wire; it replays on every turn
// otherwise and burns tokens long before auto-compaction fires.
//
// Drives the REAL wire serializer (messages_json_string) over an
// Assistant turn carrying a Done tool_call with a giant output, and
// asserts:
//   1. the serialized tool_result content is bounded (<= budget + slack)
//   2. the elision marker is present
//   3. BOTH head and tail bytes survive (Zed keeps both ends)
//   4. a SMALL result is byte-identical (no marker, no truncation)
//   5. the cut lands on a UTF-8 boundary (no mojibake / no invalid json)

#include <cstdio>
#include <string>
#include <vector>

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

int g_fails = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_fails; }
    else     { std::fprintf(stderr, "ok:   %s\n", what); }
}

// Build a one-turn thread: User prompt + Assistant turn with a single
// Done tool_call carrying `output` as its result.
Thread make_thread_with_tool_output(const std::string& output) {
    Message user;
    user.role = Role::User;
    user.text = "do a thing";

    ToolUse tc;
    tc.id   = ToolCallId{"toolu_test_1"};
    tc.name = ToolName{"grep"};
    tc.status = ToolUse::Done{
        std::chrono::steady_clock::now(),
        std::chrono::steady_clock::now(),
        output,
    };

    Message asst;
    asst.role = Role::Assistant;
    asst.text = "running grep";
    asst.tool_calls.push_back(std::move(tc));

    return Thread{ThreadId{"t"}, "", {user, asst}, {}, {}};
}

// Extract the tool_result content string for tool_use_id from the wire.
std::string tool_result_content(const std::string& wire, const char* tool_use_id) {
    auto j = nlohmann::json::parse(wire);
    for (const auto& msg : j) {
        if (msg.value("role", "") != "user") continue;
        for (const auto& block : msg.value("content", nlohmann::json::array())) {
            if (block.value("type", "") != "tool_result") continue;
            if (block.value("tool_use_id", "") != tool_use_id) continue;
            return block.value("content", "");
        }
    }
    return {};
}

// Build a thread with `n` Assistant tool-call turns, each carrying one Done
// tool_call whose output is `big`. Returns the thread; the FIRST tool turn is
// the oldest (highest recency rank), the LAST is the newest (rank 0).
Thread make_thread_with_n_tool_turns(int n, const std::string& big) {
    std::vector<Message> msgs;
    Message user; user.role = Role::User; user.text = "start";
    msgs.push_back(user);
    for (int i = 0; i < n; ++i) {
        ToolUse tc;
        tc.id   = ToolCallId{"toolu_" + std::to_string(i)};
        tc.name = ToolName{"grep"};
        tc.status = ToolUse::Done{
            std::chrono::steady_clock::now(),
            std::chrono::steady_clock::now(),
            big,
        };
        Message asst; asst.role = Role::Assistant;
        asst.text = "call " + std::to_string(i);
        asst.tool_calls.push_back(std::move(tc));
        msgs.push_back(std::move(asst));
    }
    return Thread{ThreadId{"t"}, "", std::move(msgs), {}, {}};
}

} // namespace

int main() {
    namespace ap = agentty::provider::anthropic;

    // ── 1. Small result: byte-identical passthrough, no marker ──
    {
        const std::string small = "matched 3 lines\nfoo\nbar\nbaz\n";
        Thread t = make_thread_with_tool_output(small);
        std::string wire = ap::messages_json_string(t);
        std::string content = tool_result_content(wire, "toolu_test_1");
        check(content == small, "small result ships verbatim");
        check(content.find("bytes elided") == std::string::npos,
              "small result has no elision marker");
    }

    // ── 2. Oversized result: bounded, marker present, head+tail kept ──
    {
        // 500 KiB: a HEAD_MARKER..filler..TAIL_MARKER sandwich so we can
        // assert both ends survived the cut.
        std::string big;
        big.reserve(512 * 1024);
        big += "HEAD_SENTINEL_AAAA\n";
        big.append(500 * 1024, 'x');
        big += "\nTAIL_SENTINEL_ZZZZ";

        Thread t = make_thread_with_tool_output(big);
        std::string wire = ap::messages_json_string(t);
        std::string content = tool_result_content(wire, "toolu_test_1");

        // Budget is 64 KiB; serialized content (after JSON unescaping)
        // must be far smaller than the 500 KiB input. Allow generous
        // slack for marker + boundary rounding.
        check(content.size() < 80 * 1024,
              "oversized result is bounded near the 64 KiB budget");
        check(content.size() < big.size(),
              "oversized result is smaller than the raw output");
        check(content.find("bytes elided") != std::string::npos,
              "oversized result carries the elision marker");
        check(content.find("HEAD_SENTINEL_AAAA") != std::string::npos,
              "head bytes survive the cut");
        check(content.find("TAIL_SENTINEL_ZZZZ") != std::string::npos,
              "tail bytes survive the cut");
    }

    // ── 3. Multibyte boundary: cut must not split a UTF-8 sequence ──
    {
        // Fill with a 3-byte code point (U+4E2D, CJK) so a naive byte cut
        // would land mid-sequence. The wire must still parse as valid
        // JSON (it would throw on invalid UTF-8 / split escape).
        std::string mb;
        const std::string cp = "\xE4\xB8\xAD"; // 中
        mb.reserve(200 * 1024);
        for (int i = 0; i < 70 * 1024; ++i) mb += cp;

        Thread t = make_thread_with_tool_output(mb);
        bool parsed_ok = true;
        std::string content;
        try {
            std::string wire = ap::messages_json_string(t);
            content = tool_result_content(wire, "toolu_test_1");
        } catch (...) {
            parsed_ok = false;
        }
        check(parsed_ok, "multibyte oversized result yields valid JSON");
        check(!content.empty(), "multibyte result content non-empty");
    }

    // ── 4. Empty-bytes image must NOT serialize an empty image block ──
    // A draft image attachment whose bytes were already drained can leak
    // into a thread (pre-fix) and surface as a User message carrying an
    // ImageContent with empty bytes. The wire must drop it entirely —
    // emitting `"data":""` 400s the whole request ("image cannot be
    // empty"). A REAL image in the same message must still ship.
    {
        using agentty::ImageContent;
        using agentty::Message;
        using agentty::Role;

        Message user;
        user.role = Role::User;
        user.text = "look at this";
        ImageContent empty_img;          // empty bytes — the leak shape
        empty_img.media_type = "image/png";
        user.images.push_back(std::move(empty_img));

        Thread t{ThreadId{"t"}, "", {user}, {}, {}};
        std::string wire = ap::messages_json_string(t);
        auto j = nlohmann::json::parse(wire);
        // No image block anywhere; the message still ships its text.
        bool any_image = false, any_text = false;
        for (const auto& msg : j)
            for (const auto& b : msg.value("content", nlohmann::json::array())) {
                if (b.value("type", "") == "image")  any_image = true;
                if (b.value("type", "") == "text")   any_text  = true;
            }
        check(!any_image, "empty-bytes image is dropped from the wire");
        check(any_text, "the message's text still ships");

        // Now a real image alongside an empty one: only the real one ships.
        Message user2;
        user2.role = Role::User;
        user2.text = "two images";
        ImageContent empty2; empty2.media_type = "image/png";
        ImageContent real2;  real2.media_type = "image/png";
        real2.bytes = "\x89PNG\r\n\x1a\n realbytes";
        user2.images.push_back(std::move(empty2));
        user2.images.push_back(std::move(real2));
        Thread t2{ThreadId{"t"}, "", {user2}, {}, {}};
        auto j2 = nlohmann::json::parse(ap::messages_json_string(t2));
        int image_blocks = 0;
        for (const auto& msg : j2)
            for (const auto& b : msg.value("content", nlohmann::json::array()))
                if (b.value("type", "") == "image") {
                    ++image_blocks;
                    check(!b["source"].value("data", "").empty(),
                          "shipped image block has non-empty base64");
                }
        check(image_blocks == 1, "exactly the one real image ships");
    }

    // ── 5. Age-tiered clearing: recent full, ancient faded ──
    // A long tool-burst turn: 20 calls, each dumping 60 KiB. The newest
    // ~8 must keep the full budget (active working set); the oldest must
    // collapse to the tight faded budget so stale dumps stop replaying.
    {
        std::string big;
        big += "HEAD_SENTINEL_AAAA\n";
        big.append(60 * 1024, 'x');
        big += "\nTAIL_SENTINEL_ZZZZ";

        const int n = 20;
        Thread t = make_thread_with_n_tool_turns(n, big);
        std::string wire = ap::messages_json_string(t);

        // Newest call (id toolu_19, rank 0) keeps the full budget.
        std::string newest = tool_result_content(wire, "toolu_19");
        check(newest.size() > 40 * 1024,
              "newest tool result keeps the full (large) budget");
        check(newest.find("HEAD_SENTINEL_AAAA") != std::string::npos
           && newest.find("TAIL_SENTINEL_ZZZZ") != std::string::npos,
              "newest result keeps head+tail");

        // Oldest call (id toolu_0, rank 19) is faded to the tight budget.
        std::string oldest = tool_result_content(wire, "toolu_0");
        check(oldest.size() < 6 * 1024,
              "oldest tool result is faded to the tight budget");
        check(oldest.find("bytes elided") != std::string::npos,
              "faded result carries the elision marker");
        check(oldest.find("HEAD_SENTINEL_AAAA") != std::string::npos
           && oldest.find("TAIL_SENTINEL_ZZZZ") != std::string::npos,
              "faded result STILL keeps head+tail (recall, not replay)");

        // A result well inside the recent window (id toolu_15, rank 4)
        // must NOT be faded.
        std::string recent = tool_result_content(wire, "toolu_15");
        check(recent.size() > 40 * 1024,
              "a result inside the recent window stays full");
    }

    // ── 6. Error results NEVER fade, however old ──
    {
        std::string bigerr;
        bigerr += "ERR_HEAD_AAAA\n";
        bigerr.append(60 * 1024, 'e');
        bigerr += "\nERR_TAIL_ZZZZ";

        // 20 turns; make the OLDEST one a Failed (error) call, the rest Done.
        std::vector<Message> msgs;
        Message user; user.role = Role::User; user.text = "start"; msgs.push_back(user);
        for (int i = 0; i < 20; ++i) {
            ToolUse tc;
            tc.id   = ToolCallId{"toolu_e" + std::to_string(i)};
            tc.name = ToolName{"bash"};
            if (i == 0) {
                tc.status = ToolUse::Failed{
                    std::chrono::steady_clock::now(),
                    std::chrono::steady_clock::now(), bigerr};
            } else {
                tc.status = ToolUse::Done{
                    std::chrono::steady_clock::now(),
                    std::chrono::steady_clock::now(), bigerr};
            }
            Message asst; asst.role = Role::Assistant; asst.text = "c";
            asst.tool_calls.push_back(std::move(tc));
            msgs.push_back(std::move(asst));
        }
        Thread t{ThreadId{"t"}, "", std::move(msgs), {}, {}};
        std::string wire = ap::messages_json_string(t);

        // The oldest call is an ERROR — even at rank 19 it keeps full budget.
        std::string olderr = tool_result_content(wire, "toolu_e0");
        check(olderr.size() > 40 * 1024,
              "oldest ERROR result is NOT faded (model needs it to recover)");
    }

    // ── 7. Short old results ship verbatim (nothing to fade) ──
    {
        const std::string small = "3 matches\n";
        Thread t = make_thread_with_n_tool_turns(20, small);
        std::string wire = ap::messages_json_string(t);
        std::string oldest = tool_result_content(wire, "toolu_0");
        check(oldest == small,
              "small old result ships verbatim (no marker, no fade)");
    }

    if (g_fails == 0) std::fprintf(stderr, "\nALL PASS\n");
    else              std::fprintf(stderr, "\n%d FAILURE(S)\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
