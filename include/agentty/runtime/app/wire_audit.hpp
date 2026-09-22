#pragma once
// wire_audit.hpp — what is WRONG with this request, before we send it.
//
// ════════════════════════════════════════════════════════════════════════
// WHY
// ════════════════════════════════════════════════════════════════════════
//
// Every provider rejects a malformed request with a 400 and a sentence
// written for a human. "Invalid 'messages[7].tool_calls[0].function.name':
// empty string" is one of the better ones; most are worse, and several
// vendors just say "invalid_request_error".
//
// So the failure mode is always the same: the user sees a turn die, the
// log has a 400 and a body, and nobody can say WHICH message was
// malformed or how it got that way — because by then the payload is a
// 90 KB JSON blob and the defect is one empty string inside it.
//
// GitHub's runtime does not do that. Its failure telemetry carries a
// `ModelCallFailureRequestFingerprint` — seven fields describing the
// SHAPE of the request that failed:
//
//     messageCount            toolResultMessageCount
//     namelessToolCallCount   imagePartCount
//     imagePartsMissingMediaType
//     lastMessageRole         badRequestKind
//
// and a preflight counter, `request_nameless_tool_call_count`, which is
// measured BEFORE the request goes out. They know the request is
// malformed while they can still name the defect, and they know how often
// it happens across their fleet.
//
// ════════════════════════════════════════════════════════════════════════
// THE SHAPE OF THIS API
// ════════════════════════════════════════════════════════════════════════
//
// A bool would be useless. "Is this request valid" answers nothing you can
// act on — you still have to go find the defect. So `audit_wire()` returns
// a value that NAMES each defect and WHERE it is:
//
//     for (const auto& d : audit_wire(msgs).defects)
//         log("{} at message {}", describe(d.kind), d.message_index);
//
// `Defect::Kind` is a closed enum, not a string, for the same reason
// ErrorClass is: a caller can switch on it exhaustively and the compiler
// checks the switch. A new defect kind becomes a compile error at every
// site that classifies one, which is what you want — the alternative is a
// string nobody handles.
//
// ════════════════════════════════════════════════════════════════════════
// WHAT THIS IS NOT
// ════════════════════════════════════════════════════════════════════════
//
// Not a validator that refuses to send. Providers disagree about what is
// legal, they change, and a client that refuses a request the server would
// have accepted is worse than one that sends a request the server rejects.
// Every defect here is a STRONG SUSPICION, logged and counted, and the
// request still goes out.
//
// Not a repair pass either. Repair belongs where the defect is created,
// with the context to fix it correctly. This only names things.

#include <cstddef>
#include <string_view>
#include <vector>

#include "agentty/domain/conversation.hpp"

namespace agentty::app::cmd {

// One thing wrong with one message.
struct Defect {
    enum class Kind {
        // A tool call with an empty name. The single most common cause of
        // a 400 on every dialect — the server cannot route it, and the
        // error it returns names the index, not the cause. This is the one
        // GitHub counts by name (`request_nameless_tool_call_count`).
        NamelessToolCall,

        // A tool RESULT whose id matches no call in the transcript.
        // Anthropic rejects the request outright; OpenAI-compatible
        // gateways vary. Usually a sign a call was dropped mid-stream
        // while its result survived — exactly the corruption the
        // attribution seam exists to prevent, observed from the other end.
        OrphanToolResult,

        // A tool CALL with no matching result, in a completed turn. The
        // next request will be rejected by Anthropic ("tool_use ids must
        // have corresponding tool_result blocks"). Benign mid-turn, which
        // is why the audit only flags it on messages that are not last.
        UnansweredToolCall,

        // An image part with no media type. Several gateways 400 on this
        // rather than sniffing. GitHub tracks it as its own fingerprint
        // field (`imagePartsMissingMediaType`), which is how we know it
        // happens often enough to matter.
        ImageMissingMediaType,

        // A message carrying no content at all — no text, no tool calls,
        // no images. Anthropic rejects empty content blocks; others
        // silently drop the turn, which is worse because the model then
        // answers a question it cannot see.
        EmptyMessage,
    };

    Kind        kind;
    std::size_t message_index;   // index into the WIRE messages, not the thread
    // The tool call id or name involved, when the defect has one. Empty
    // otherwise. Deliberately a copy: audits outlive the payload they
    // describe (they go into a log line after the request is gone).
    std::string detail;
};

// A stable, greppable name for a defect kind.
//
// Returns a string_view into static storage — these are log tokens, not
// prose, so they stay machine-greppable across versions.
[[nodiscard]] constexpr std::string_view describe(Defect::Kind k) noexcept {
    switch (k) {
        case Defect::Kind::NamelessToolCall:      return "nameless_tool_call";
        case Defect::Kind::OrphanToolResult:      return "orphan_tool_result";
        case Defect::Kind::UnansweredToolCall:    return "unanswered_tool_call";
        case Defect::Kind::ImageMissingMediaType: return "image_missing_media_type";
        case Defect::Kind::EmptyMessage:          return "empty_message";
    }
    return "unknown";
}

// The SHAPE of a request, and everything suspicious in it.
//
// The counts mirror GitHub's fingerprint because they chose them for the
// right reason: they are the numbers that distinguish one 400 from
// another when you cannot see the body. A request with
// `messages=48 tool_results=23 last_role=assistant` is a different
// situation from `messages=2 tool_results=0 last_role=user`, and the
// distinction survives in a log line that costs nothing.
struct WireAudit {
    std::size_t message_count       = 0;
    std::size_t tool_result_count   = 0;
    std::size_t tool_call_count     = 0;
    std::size_t image_part_count    = 0;
    // The role of the final message. A request ending on an assistant turn
    // is legal on some dialects and a 400 on others, and it is the first
    // thing to check when a turn dies immediately.
    Role        last_role           = Role::User;

    std::vector<Defect> defects;

    [[nodiscard]] bool clean() const noexcept { return defects.empty(); }

    // How many defects of one kind. The per-kind count is what makes a
    // rate visible: one nameless call is a bug report, forty is a
    // systematic failure in whatever produced them.
    [[nodiscard]] std::size_t count(Defect::Kind k) const noexcept {
        std::size_t n = 0;
        for (const auto& d : defects) if (d.kind == k) ++n;
        return n;
    }
};

// Audit the wire payload. Pure: no logging, no mutation, no I/O.
//
// Takes the WIRE messages (post-compaction substitution), because that is
// what the provider will actually see — auditing the raw transcript would
// report defects in messages that are not being sent.
[[nodiscard]] WireAudit audit_wire(const std::vector<Message>& wire);

}  // namespace agentty::app::cmd
