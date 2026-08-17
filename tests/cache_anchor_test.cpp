// cache_anchor_test — SOTA prompt-cache breakpoint placement.
//
// SOTA prompt-cache breakpoint placement. Two message breakpoints roll or
// anchor, and the full body spends the other 2 of Anthropic's 4-breakpoint
// budget on system + tools. This test locks the message-array shape:
//
//   • LONG thread: a 1-HOUR anchor deep in history + ONE rolling 5m pin on the
//     newest message (the second-to-last rolling pin is dropped so the request
//     never exceeds 4 total breakpoints — exceeding 4 makes Anthropic evict the
//     system-prompt pin from the front).
//   • anchor is QUANTIZED (doesn't move when one turn is appended).
//   • SHORT thread: no anchor; the classic rolling PAIR (≤ 2 pins).
//   • message breakpoints never exceed 2.

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agtest.hpp"

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

// A thread of `pairs` User/Assistant text turns → 2*pairs messages.
Thread make_thread(int pairs) {
    std::vector<Message> msgs;
    for (int i = 0; i < pairs; ++i) {
        Message u; u.role = Role::User;      u.text = "u" + std::to_string(i);
        Message a; a.role = Role::Assistant; a.text = "a" + std::to_string(i);
        msgs.push_back(std::move(u));
        msgs.push_back(std::move(a));
    }
    return Thread{ThreadId{"t"}, "", std::move(msgs), {}, {}};
}

// Walk the serialized messages array and collect, per emitted message, the ttl
// of its cache_control (or "" if none). Index in the returned vector == the
// message's position in the wire array.
struct Pin { int msg_index; std::string ttl; };
std::vector<Pin> collect_pins(const std::string& wire) {
    std::vector<Pin> pins;
    auto j = nlohmann::json::parse(wire);
    int idx = 0;
    for (const auto& msg : j) {
        for (const auto& block : msg.value("content", nlohmann::json::array())) {
            if (block.contains("cache_control")) {
                std::string ttl = block["cache_control"].value("ttl", "5m-default");
                pins.push_back(Pin{idx, ttl});
            }
        }
        ++idx;
    }
    return pins;
}

} // namespace

TEST_CASE("cache_anchor") {
    namespace ap = agentty::provider::anthropic;

    // ── 1. Long thread: exactly one 1h anchor + the two rolling 5m pins ──
    {
        Thread t = make_thread(40);                    // 80 messages
        std::string wire = ap::messages_json_string(t);
        auto pins = collect_pins(wire);

        int anchors = 0, rolling = 0;
        int anchor_idx = -1;
        for (const auto& p : pins) {
            if (p.ttl == "1h") { ++anchors; anchor_idx = p.msg_index; }
            else               { ++rolling; }
        }
        check(anchors == 1, "long thread has exactly one 1h anchor breakpoint");
        check(rolling == 1, "long thread keeps one rolling 5m breakpoint (2nd dropped for the 4-cap)");
        check(pins.size() <= 2, "≤ 2 message breakpoints so total ≤ 4 (system+tools spend 2)");

        // Anchor sits strictly before the rolling pair (deep in history).
        int total = 80;
        check(anchor_idx > 0 && anchor_idx < total - 2,
              "anchor lands deep in history, before the rolling pair");
    }

    // ── 2. Anchor is STABLE: adding one turn does not move it ──
    {
        Thread t1 = make_thread(40);   // 80 msgs
        Thread t2 = make_thread(40);
        // Append one more user+assistant pair (82 msgs) — still same quantum.
        Message u; u.role = Role::User;      u.text = "extra";
        Message a; a.role = Role::Assistant; a.text = "extra-a";
        t2.messages.push_back(std::move(u));
        t2.messages.push_back(std::move(a));

        auto anchor_of = [](const std::string& wire) {
            for (const auto& p : collect_pins(wire))
                if (p.ttl == "1h") return p.msg_index;
            return -1;
        };
        int a1 = anchor_of(ap::messages_json_string(t1));
        int a2 = anchor_of(ap::messages_json_string(t2));
        check(a1 != -1 && a1 == a2,
              "anchor index is unchanged when one turn is appended (quantized)");
    }

    // ── 3. Short thread: no 1h anchor (rolling pair covers everything) ──
    {
        Thread t = make_thread(3);                     // 6 messages
        std::string wire = ap::messages_json_string(t);
        auto pins = collect_pins(wire);
        int anchors = 0;
        for (const auto& p : pins) if (p.ttl == "1h") ++anchors;
        check(anchors == 0, "short thread emits no 1h anchor");
        check(pins.size() <= 2, "short thread has only the rolling pins");
    }

    // ── 4. Empty thread: no breakpoints, valid JSON ──
    {
        Thread t{ThreadId{"t"}, "", {}, {}, {}};
        std::string wire = ap::messages_json_string(t);
        auto pins = collect_pins(wire);
        check(pins.empty(), "empty thread has no cache breakpoints");
    }
}
