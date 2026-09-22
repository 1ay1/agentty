// wire_audit.cpp — see wire_audit.hpp for why this exists.
//
// One pass over the wire messages, collecting every defect that has ever
// produced a 400 we had to diagnose from a body dump.

#include "agentty/runtime/app/wire_audit.hpp"

#include <unordered_set>

namespace agentty::app::cmd {

WireAudit audit_wire(const std::vector<Message>& wire) {
    WireAudit a;
    a.message_count = wire.size();
    if (!wire.empty()) a.last_role = wire.back().role;

    // Pass 1 — shape counts, plus the defects a single message can be
    // judged on alone.
    //
    // Tool-call/result PAIRING needs both ends, so it waits for pass 2.
    // Everything here is local: a name is empty or it is not.
    std::unordered_set<std::string> call_ids;     // ids the assistant asked for
    std::unordered_set<std::string> answered;     // ids that carry a result

    for (std::size_t i = 0; i < wire.size(); ++i) {
        const auto& m = wire[i];

        a.image_part_count += m.images.size();
        for (const auto& img : m.images) {
            if (img.media_type.empty()) {
                a.defects.push_back({Defect::Kind::ImageMissingMediaType, i, {}});
            }
        }

        // An entirely contentless message.
        //
        // EXCEPT the last one. Every turn appends the assistant message
        // the model is ABOUT to fill before the request goes out, so the
        // final slot is legitimately empty at audit time — by
        // construction, on every healthy turn.
        //
        // This fired on real traffic immediately: a 928-message thread
        // reported `defects=1` on every single turn, always at index
        // 927. A warning that fires on every healthy turn is worse than
        // no warning, because it trains the reader to skip the channel
        // the real defects arrive on — the exact failure the
        // "checker that cries wolf gets muted" note in the pending-call
        // arm below was written about. I wrote that note and then shipped
        // the same mistake one arm up.
        const bool has_text  = !m.text.empty() || !m.streaming_text.empty()
                            || !m.pending_stream.empty();
        const bool is_last   = (i + 1 == wire.size());
        if (!has_text && m.tool_calls.empty() && m.images.empty()
            && !is_last) {
            a.defects.push_back({Defect::Kind::EmptyMessage, i, {}});
        }

        for (const auto& tc : m.tool_calls) {
            ++a.tool_call_count;
            call_ids.insert(tc.id.value);

            // THE one. A tool call the server cannot route, which comes
            // back as a 400 naming an index rather than a cause.
            if (tc.name.value.empty()) {
                a.defects.push_back({Defect::Kind::NamelessToolCall, i,
                                     tc.id.value});
            }

            // A call in a TERMINAL state carries its result on the wire;
            // one still pending does not, and that is correct mid-turn.
            // Only a terminal call counts as answered.
            if (tc.is_terminal()) {
                ++a.tool_result_count;
                answered.insert(tc.id.value);

                for (const auto& img : tc.done_images()) {
                    ++a.image_part_count;
                    if (img.media_type.empty()) {
                        a.defects.push_back(
                            {Defect::Kind::ImageMissingMediaType, i,
                             tc.id.value});
                    }
                }
            }
        }
    }

    // Pass 2 — pairing.
    //
    // A result with no call is always wrong. A call with no result is only
    // wrong once the turn has moved on: the LAST message is allowed to
    // hold calls still awaiting execution, which is the normal mid-turn
    // state and not a defect.
    for (std::size_t i = 0; i < wire.size(); ++i) {
        const auto& m = wire[i];
        const bool is_last = (i + 1 == wire.size());
        for (const auto& tc : m.tool_calls) {
            if (!tc.is_terminal() && !is_last) {
                a.defects.push_back({Defect::Kind::UnansweredToolCall, i,
                                     tc.name.value});
            }
        }
    }

    // An orphan result cannot be detected from ToolUse alone — a terminal
    // call is its own result in this model, so the id is always paired by
    // construction. The kind stays in the enum because the ACP and
    // subagent paths assemble tool results independently and can produce
    // one; leaving a hole in the taxonomy would just move the diagnosis
    // back to reading a body dump.
    (void)call_ids;
    (void)answered;

    return a;
}

}  // namespace agentty::app::cmd
