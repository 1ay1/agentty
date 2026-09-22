// wire_audit_test — the request is malformed; say WHICH message, not "400".
//
// ── WHY THIS EXISTS ──────────────────────────────────────────────────────
//
// Every provider rejects a malformed request with a 400 and a sentence
// written for a human. By the time it arrives the payload is a 90 KB JSON
// blob and the defect is one empty string inside it, so the diagnosis is
// an afternoon of reading a body dump.
//
// GitHub's runtime does not accept that. Its failure telemetry carries a
// seven-field `ModelCallFailureRequestFingerprint` describing the SHAPE of
// the request that failed, and a preflight counter
// (`request_nameless_tool_call_count`) measured BEFORE the request goes
// out. They know the request is malformed while they can still name the
// defect, and they know the rate across their fleet.
//
// audit_wire() is that, as a pure function over the wire payload.
//
// ── WHAT THIS PINS ───────────────────────────────────────────────────────
//
// 1. Each defect kind is detected, and detected at the right INDEX — an
//    audit that says "something is wrong" is the 400 we already had.
// 2. Legitimate shapes stay clean. A checker that cries wolf gets muted,
//    and a muted checker is worse than none: it converts "we don't know"
//    into "we checked".
// 3. The taxonomy is exhaustive by construction — a new Kind cannot be
//    added without `describe()` naming it.

#include <string>
#include <vector>

#include "agtest.hpp"

#include "agentty/runtime/app/wire_audit.hpp"
#include "agentty/domain/catalog.hpp"

using namespace agentty;
using namespace agentty::app::cmd;

namespace {

Message umsg(std::string text) {
    Message m; m.role = Role::User; m.text = std::move(text);
    return m;
}

Message amsg(std::string text) {
    Message m; m.role = Role::Assistant; m.text = std::move(text);
    return m;
}

// An assistant turn carrying one tool call. `terminal` decides whether the
// call already has its result (a completed round) or is still awaiting
// execution (the normal mid-turn state).
Message with_call(std::string name, std::string id, bool terminal) {
    Message m; m.role = Role::Assistant;
    ToolUse tc;
    tc.id   = ToolCallId{std::move(id)};
    tc.name = ToolName{std::move(name)};
    tc.args = nlohmann::json::object();
    if (terminal) {
        tc.status = ToolUse::Done{{}, {}, "output"};
    } else {
        tc.status = ToolUse::Pending{};
    }
    m.tool_calls.push_back(std::move(tc));
    return m;
}

bool has_kind(const WireAudit& a, Defect::Kind k) {
    return a.count(k) > 0;
}

}  // namespace

TEST_CASE("wire audit: a healthy exchange is clean") {
    // The load-bearing case. Every other assertion is worthless if the
    // ordinary shape trips the checker — a warning that fires on healthy
    // traffic teaches people to ignore the channel it fires on.
    std::vector<Message> wire;
    wire.push_back(umsg("list the files"));
    wire.push_back(with_call("shell", "call_1", /*terminal=*/true));
    wire.push_back(amsg("here they are"));

    const auto a = audit_wire(wire);

    CHECK(a.clean());
    CHECK(a.message_count     == 3);
    CHECK(a.tool_call_count   == 1);
    CHECK(a.tool_result_count == 1);
    CHECK(a.last_role == Role::Assistant);
}

TEST_CASE("wire audit: a nameless tool call is named, with its index") {
    // THE one. A tool call the server cannot route comes back as a 400
    // that names an array index, not a cause — so the audit has to supply
    // the cause and the index itself.
    //
    // GitHub counts exactly this metric by name, which is how we know it
    // is frequent enough on a multi-vendor wire to deserve its own
    // counter rather than a generic "malformed request".
    std::vector<Message> wire;
    wire.push_back(umsg("go"));
    wire.push_back(with_call("", "call_broken", /*terminal=*/true));

    const auto a = audit_wire(wire);

    REQUIRE(a.count(Defect::Kind::NamelessToolCall) == 1);
    const auto& d = a.defects.front();
    CHECK(d.kind          == Defect::Kind::NamelessToolCall);
    CHECK(d.message_index == 1);              // WHICH message
    CHECK(d.detail        == "call_broken");  // and which call in it
}

TEST_CASE("wire audit: an image with no media type is flagged") {
    // Several gateways 400 rather than sniffing the bytes. GitHub tracks
    // it as its own fingerprint field (`imagePartsMissingMediaType`),
    // which is the tell that it happens often enough to matter.
    std::vector<Message> wire;
    Message m = umsg("what is this?");
    m.images.emplace_back("", std::string("\x89PNG fake bytes"));
    wire.push_back(std::move(m));

    const auto a = audit_wire(wire);

    CHECK(a.image_part_count == 1);
    CHECK(has_kind(a, Defect::Kind::ImageMissingMediaType));

    // And a well-formed image does NOT trip it.
    std::vector<Message> good;
    Message g = umsg("what is this?");
    g.images.emplace_back("image/png", std::string("\x89PNG fake bytes"));
    good.push_back(std::move(g));

    CHECK(!has_kind(audit_wire(good), Defect::Kind::ImageMissingMediaType));
}

TEST_CASE("wire audit: an empty message is flagged, except the last one") {
    // No text, no calls, no images. Anthropic rejects empty content
    // blocks; other dialects silently drop the turn, which is worse —
    // the model then answers a question it cannot see.
    std::vector<Message> wire;
    wire.push_back(umsg("hello"));
    wire.push_back(amsg(""));          // nothing at all
    wire.push_back(umsg("still here"));

    const auto a = audit_wire(wire);

    REQUIRE(a.count(Defect::Kind::EmptyMessage) == 1);
    CHECK(a.defects.front().message_index == 1);

    // But the FINAL message is empty by construction: every turn appends
    // the assistant message the model is about to fill before the request
    // goes out. Flagging it fires on every healthy turn.
    //
    // This is not hypothetical — the first version shipped without the
    // exemption and a real 928-message thread reported defects=1 on every
    // single turn, always at the last index. A warning that fires on
    // healthy traffic is worse than none: it trains the reader to skip
    // the channel the real defects arrive on.
    std::vector<Message> in_flight;
    in_flight.push_back(umsg("hello"));
    in_flight.push_back(amsg(""));     // the turn about to be filled

    CHECK(audit_wire(in_flight).count(Defect::Kind::EmptyMessage) == 0);
    CHECK(audit_wire(in_flight).clean());
}

TEST_CASE("wire audit: a pending call mid-transcript is unanswered") {
    // A call with no result is legal as the LAST message — that is the
    // normal state while a tool runs. Anywhere earlier it means a result
    // was lost, and Anthropic rejects the next request outright
    // ("tool_use ids must have corresponding tool_result blocks").
    //
    // So the same shape is a defect or not depending on position, and
    // getting that backwards would make the checker fire on every healthy
    // turn.
    std::vector<Message> mid;
    mid.push_back(umsg("go"));
    mid.push_back(with_call("shell", "call_1", /*terminal=*/false));
    mid.push_back(amsg("moved on without the result"));

    CHECK(has_kind(audit_wire(mid), Defect::Kind::UnansweredToolCall));

    // The same call as the final message: healthy, mid-turn.
    std::vector<Message> tail;
    tail.push_back(umsg("go"));
    tail.push_back(with_call("shell", "call_1", /*terminal=*/false));

    CHECK(!has_kind(audit_wire(tail), Defect::Kind::UnansweredToolCall));
}

TEST_CASE("wire audit: counts make a rate visible, not just a flag") {
    // One nameless call is a bug report. Forty is a systematic failure in
    // whatever produced them, and the difference has to survive into the
    // log — which is the whole reason `count()` exists rather than a bool.
    std::vector<Message> wire;
    wire.push_back(umsg("go"));
    for (int i = 0; i < 5; ++i)
        wire.push_back(with_call("", "call_" + std::to_string(i), true));

    const auto a = audit_wire(wire);

    CHECK(a.count(Defect::Kind::NamelessToolCall) == 5);
    CHECK(a.tool_call_count == 5);
    CHECK(!a.clean());
}

TEST_CASE("wire audit: the taxonomy is exhaustive") {
    // describe() must name every Kind. A new defect added without a name
    // would log as "unknown", which is the string form of the 400 this
    // whole file exists to replace.
    //
    // The switch in describe() has no default for the enum cases, so a new
    // Kind is already a -Wswitch warning at that site. This is the runtime
    // half: it proves the names are distinct and none has silently
    // collapsed to the fallback.
    static constexpr Defect::Kind kAll[] = {
        Defect::Kind::NamelessToolCall,
        Defect::Kind::OrphanToolResult,
        Defect::Kind::UnansweredToolCall,
        Defect::Kind::ImageMissingMediaType,
        Defect::Kind::EmptyMessage,
    };

    std::vector<std::string> seen;
    for (auto k : kAll) {
        const auto name = describe(k);
        CHECK(!name.empty());
        CHECK(name != "unknown");
        for (const auto& prior : seen)
            CHECK(prior != name);            // names are distinct
        seen.emplace_back(name);
    }
}

TEST_CASE("wire audit: an empty payload is clean, not a crash") {
    // Degenerate input. A checker that throws on the empty case turns a
    // recoverable "nothing to send" into a dead turn.
    const auto a = audit_wire({});
    CHECK(a.clean());
    CHECK(a.message_count == 0);
}

TEST_CASE("vision: unknown sends, only a declaration withholds") {
    // The tri-state, and why its default is the opposite of what "be
    // safe" suggests.
    //
    // A model that DECLARED vision:false rejects image parts — a hard 400
    // on most wires, or on Ollama an image it silently ignores while
    // answering about text it cannot see. Withholding there turns a dead
    // turn into a normal one that simply lacks the picture.
    //
    // But UNKNOWN must still send. Stripping on silence would remove
    // images from every model no catalog describes, and a vision model
    // whose screenshot we quietly dropped is indistinguishable from one
    // that looked and was unhelpful — the user reports "it ignored my
    // screenshot" and nothing in the log disagrees.
    //
    // Exactly the reasoning ModelInfo::supports_tools documents, which is
    // why this mirrors it rather than inventing a second convention.
    ModelInfo mi;

    CHECK(!mi.supports_vision.has_value());   // nothing populates it by default
    CHECK(mi.vision_allowed());               // …and unknown SENDS

    mi.supports_vision = true;
    CHECK(mi.vision_allowed());

    mi.supports_vision = false;               // the one case we KNOW
    CHECK(!mi.vision_allowed());

    // And it agrees with the tool gate's shape, so a reader who has
    // internalised one is not surprised by the other.
    ModelInfo t;
    CHECK(t.tools_allowed());                 // unknown advertises tools
    t.supports_tools = false;
    CHECK(!t.tools_allowed());
}
