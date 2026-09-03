// render_key_coverage_test.cpp — pins Message::compute_render_key() coverage.
//
// WHY THIS TEST EXISTS
// ====================
// compute_render_key() is the CONTENT-identity hash the maya render cache keys
// on (via assistant_run_hash_id / per-turn hash_id in turn.cpp). The cache's
// contract: a hash_id uniquely determines the rendered cells. If a Message
// field that AFFECTS the rendered card changes but is NOT mixed into
// compute_render_key(), the key stays constant, maya's blit fast path serves
// the STALE cached cells, and the user sees corruption (a card frozen on old
// content). This is the exact "hash doesn't cover rendered content" class that
// hid a terminal-resize bug in maya's height cache (fixed separately).
//
// compute_render_key documents the invariant in a comment ("keep in sync with
// the reads in turn.cpp"), but a comment isn't enforcement. This test makes it
// enforceable: a declarative table of "fields that must move the key." Add a
// rendered Message field → add a row here → the test then REQUIRES the matching
// mix() line. It mirrors visual_hash_coverage_test for Program::visual_hash.
//
// NOTE ON SIZE-VS-BYTES: compute_render_key mixes string SIZES, not bytes, by
// design — assistant text is append-only during streaming, so size uniquely
// identifies content. These axes therefore change LENGTH, matching the real
// mutation shape. (A same-length in-place byte swap is out of contract and not
// asserted; it never happens for the settled cards the cache serves.)
#undef NDEBUG
#include "agtest.hpp"

#include "agentty/domain/conversation.hpp"

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using agentty::Message;
using agentty::Role;

namespace {

struct Axis {
    const char*                    name;
    std::function<void(Message&)>  mutate;
};

// A representative assistant message carrying every card kind, so each axis
// mutates a field that is actually populated.
Message base_message() {
    Message m;
    m.role           = Role::Assistant;
    m.text           = "settled body text";
    m.streaming_text = "live tail";
    m.pending_stream = "buffered";
    m.error          = std::nullopt;
    // Smart routing card.
    m.smart_routing          = true;
    m.smart_route_model      = "claude-opus-4-5";
    m.smart_route_effort     = "high";
    m.smart_route_complexity = "standard";
    m.smart_route_note       = "bumped for ambiguity";
    m.smart_route_orchestrate = false;
    m.smart_route_subagents   = false;
    // Fork card.
    m.fork_note       = true;
    m.fork_transcript = "/tmp/parent.md";
    return m;
}

const std::vector<Axis>& render_axes() {
    static const std::vector<Axis> axes = {
        {"role", [](Message& m) { m.role = Role::User; }},
        {"text grows", [](Message& m) { m.text += " more"; }},
        {"streaming_text grows", [](Message& m) { m.streaming_text += " x"; }},
        {"pending_stream grows", [](Message& m) { m.pending_stream += " y"; }},
        {"error appears", [](Message& m) { m.error = "boom"; }},
        {"tool_calls appended", [](Message& m) {
            agentty::ToolUse tc; tc.name = agentty::ToolName{"bash"};
            m.tool_calls.push_back(tc);
        }},
        {"attachment appended", [](Message& m) {
            agentty::Attachment a; a.kind = agentty::Attachment::Kind::FileRef;
            a.name = "f.txt"; a.body = "data";
            m.attachments.push_back(a);
        }},
        {"is_compact_summary toggles", [](Message& m) {
            m.is_compact_summary = !m.is_compact_summary;
        }},
        {"proactive identity toggles", [](Message& m) {
            if (m.proactive) m.proactive.reset();
            else             m.proactive = Message::ProactiveContext{};
        }},
        {"proactive expanded toggles", [](Message& m) {
            if (!m.proactive) m.proactive = Message::ProactiveContext{};
            m.proactive->expanded = !m.proactive->expanded;
        }},
        {"reasoning text (thinking) changes length", [](Message& m) {
            // Drives Message::reasoning_display_text(), which the reasoning
            // block renders. Must be in the render key or the cached Element
            // serves a stale (or missing) "Thought for N tokens" summary.
            m.thinking += "chain of thought step";
        }},
        {"smart_routing toggles", [](Message& m) {
            m.smart_routing = !m.smart_routing;
        }},
        {"smart_route_model changes length", [](Message& m) {
            m.smart_route_model += "-mini";
        }},
        {"smart_route_effort changes length", [](Message& m) {
            m.smart_route_effort = "xhigh";
        }},
        {"smart_route_complexity changes length", [](Message& m) {
            m.smart_route_complexity = "complex";
        }},
        {"smart_route_note changes length", [](Message& m) {
            m.smart_route_note += " (learned prior)";
        }},
        {"smart_route_orchestrate toggles", [](Message& m) {
            m.smart_route_orchestrate = !m.smart_route_orchestrate;
        }},
        {"smart_route_subagents toggles", [](Message& m) {
            m.smart_route_subagents = !m.smart_route_subagents;
        }},
        {"fork_note toggles", [](Message& m) { m.fork_note = !m.fork_note; }},
        {"fork_transcript changes length", [](Message& m) {
            m.fork_transcript += "/deeper.md";
        }},
        {"proactive confidence bucket", [](Message& m) {
            if (!m.proactive) m.proactive = Message::ProactiveContext{};
            const double cur = m.proactive->confidence.value_or(0.0);
            m.proactive->confidence = cur < 0.5 ? 0.9 : 0.1;
        }},
    };
    return axes;
}

} // namespace

TEST_CASE("render_key covers every rendered Message field") {
    // Non-vacuity guard: two IDENTICAL messages must hash equal, and a no-op
    // must NOT move the key — proves the assertions below are comparing real,
    // differing values (not a hash that changes on every call, which would
    // make every axis "pass" vacuously).
    {
        Message a = base_message();
        Message b = base_message();
        CHECK(a.compute_render_key() == b.compute_render_key());
        const std::uint64_t k = a.compute_render_key();
        (void)a.text.size();                 // a genuine no-op read
        CHECK(a.compute_render_key() == k);  // stable across calls
    }

    int failures = 0;
    for (const auto& ax : render_axes()) {
        Message before = base_message();
        const std::uint64_t k0 = before.compute_render_key();

        Message after = base_message();
        ax.mutate(after);
        const std::uint64_t k1 = after.compute_render_key();

        if (k0 == k1) {
            ++failures;
            std::fprintf(stderr,
                "render_key COVERAGE GAP: mutating '%s' did NOT change "
                "compute_render_key() \u2014 the render cache would serve stale "
                "cells for this field. Add a mix() for it in "
                "Message::compute_render_key (conversation.hpp).\n",
                ax.name);
        }
    }
    CHECK(failures == 0);
}
