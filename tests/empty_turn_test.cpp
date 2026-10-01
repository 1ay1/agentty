// empty_turn_test — a turn that produced NOTHING must say so.
//
// THE BUG THIS PINS, from a real saved thread (~/.agentty/threads):
//
//   rec  7   out=726   bytes=1150   ttft=3442   ← healthy turn
//   rec  9   out=4     bytes=—      ttft=—      ← "stress test this sandbox"
//   rec 11   out=—     bytes=—      ttft=—      ← retried, same
//   rec 13   out=2     bytes=—      ttft=—      ← user typed "?"
//
// Three consecutive turns with text_len=0, tool_calls=0, a VALID thinking
// block with a signature, 2-4 billed output tokens, and no ttft_ms or
// wire_bytes at all — meaning no content byte ever arrived. HTTP was 200 and
// the SSE framing was well-formed, so no error surfaced anywhere. The user
// saw an empty bubble three times and reported it as "it just goes silent".
//
// Two independent defects produced that:
//
//   1. parse_stop_reason() had no `refusal` arm. Anthropic documents it
//      (docs.claude.com/en/api/handling-stop-reasons) and it is how a safety
//      decline is reported — on a 200, mid-stream, via message_delta. It fell
//      through to Unspecified, which reads as "wire said nothing", so a
//      decline was indistinguishable from a clean end_turn.
//
//   2. Nothing rendered an empty assistant turn. The OpenAI transport has
//      had ensure_nonempty_turn() since the qwen salvage work; the Anthropic
//      path never grew an equivalent.
//
// The guard lives in finalize_turn (update/stream.cpp) rather than in a
// transport, because that is the one place every provider's stream converges
// — so a wire that goes quiet cannot be silent on only some of them.

#include <string>

#include "agtest.hpp"

#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"

using namespace agentty;

namespace {

// Drive a turn to its terminal event and hand back the assistant message.
//
// Goes through app::update rather than poking the Model, so this covers the
// wiring: if the guard is ever moved somewhere the reducer does not reach,
// this stops compiling or stops passing.
Message settle_with(StopReason stop, const std::string& streamed = {},
                    bool with_tool_call = false) {
    Model m;
    m.d.current.id = ThreadId{"empty-turn"};

    Message u;
    u.role = Role::User;
    u.text = "stress test this sandbox";
    m.d.current.messages.push_back(std::move(u));

    Message a;
    a.role = Role::Assistant;
    a.streaming_text = streamed;
    if (with_tool_call) {
        ToolUse tc;
        tc.id   = ToolCallId{"call_1"};
        tc.name = ToolName{"bash"};
        a.tool_calls.push_back(std::move(tc));
    }
    m.d.current.messages.push_back(std::move(a));
    m.s.phase = phase::Streaming{phase::Active{}};

    auto [out, _] = app::update(std::move(m),
                                Msg{msg::StreamMsg{StreamFinished{stop}}});
    return out.d.current.messages.back();
}

bool has(std::string_view hay, std::string_view needle) {
    return hay.find(needle) != std::string_view::npos;
}

}  // namespace

TEST_CASE("empty turn: a refusal says it was refused") {
    // The exact shape of the reported bug: terminal stop, no text, no tools.
    const auto msg = settle_with(StopReason::Refusal);

    // The ONE hard guarantee — never an empty bubble again.
    agtest::check(!msg.text.empty(),
                  "a refused turn renders something");
    // ...and it names the cause, because "declined" and "crashed" call for
    // completely different responses from the user.
    agtest::check(has(msg.text, "declined"),
                  "the refusal is described as a decline");
}

TEST_CASE("empty turn: every terminal stop is covered") {
    // The guard must not special-case only the reason we happened to hit.
    // A wire that goes quiet on ANY terminal stop is the same bug.
    for (const auto stop : {StopReason::Refusal,
                            StopReason::MaxTokens,
                            StopReason::ContextExceeded,
                            StopReason::EndTurn,
                            StopReason::StopSequence,
                            StopReason::Unspecified}) {
        const auto msg = settle_with(stop);
        INFO("stop_reason: " << std::string{to_string(stop)});
        agtest::check(!msg.text.empty(),
                      "no terminal stop leaves a blank bubble");
    }
}

TEST_CASE("empty turn: a tool call is NOT an empty turn") {
    // The false-positive that would be worse than the bug. A model calling a
    // tool with no preamble is normal and extremely common — stamping
    // "produced no output" on it would put a lie in the transcript on a huge
    // fraction of healthy turns.
    const auto msg = settle_with(StopReason::ToolUse, /*streamed=*/{},
                                 /*with_tool_call=*/true);
    agtest::check(msg.text.empty(),
                  "a silent tool call keeps its empty text");
}

TEST_CASE("empty turn: real text is never overwritten") {
    // The other false-positive: the guard runs AFTER streaming_text folds
    // into text, so it must observe the folded result, not the pre-fold
    // state. If it ran first it would stamp its notice on every normal turn.
    const auto msg = settle_with(StopReason::EndTurn, "a real answer");
    agtest::check(msg.text == "a real answer",
                  "a turn with content is left exactly alone");
}

TEST_CASE("empty turn: refusal survives the wire round-trip") {
    // Defect (1) on its own: before the fix this parsed to Unspecified, so a
    // decline arrived at the reducer wearing the same face as a clean finish.
    agtest::check(parse_stop_reason("refusal") == StopReason::Refusal,
                  "refusal parses to its own value");
    agtest::check(parse_stop_reason("model_context_window_exceeded")
                      == StopReason::ContextExceeded,
                  "context-window overflow parses to its own value");
    // Round-trip, so the value a thread persists can be read back.
    agtest::check(parse_stop_reason(to_string(StopReason::Refusal))
                      == StopReason::Refusal,
                  "refusal round-trips through to_string");
    // An unknown future value must stay Unspecified rather than crash or
    // alias onto a real reason.
    agtest::check(parse_stop_reason("some_future_reason")
                      == StopReason::Unspecified,
                  "an unknown stop reason is Unspecified");
}
