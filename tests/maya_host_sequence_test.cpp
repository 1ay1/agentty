// maya_host_sequence_test — the Cmd::EmitHostSequence host-escape primitive.
//
// Verifies the maya-level building blocks agentty uses to talk to a
// cooperating host terminal (Emacs/vterm's OSC hooks, etc.):
//   • emit_osc(code, payload) builds a well-formed, ST-terminated OSC,
//   • emit_host_sequence(raw) carries an arbitrary sequence verbatim,
//   • both survive Cmd::map() (the functor used to embed a child's Msg),
//   • the alternative is a distinct variant arm.
#include <doctest/doctest.h>

#include "maya/core/cmd.hpp"

#include <string>
#include <variant>

namespace {

// Two unrelated Msg types so we can exercise map<Parent>(child_cmd).
struct ChildMsg { int v = 0; };
struct ParentMsg { int v = 0; };

using CCmd = maya::Cmd<ChildMsg>;
using PCmd = maya::Cmd<ParentMsg>;

// Pull the raw sequence out of an EmitHostSequence Cmd, or "" otherwise.
template <class Msg>
std::string sequence_of(const maya::Cmd<Msg>& c) {
    if (auto* e = std::get_if<typename maya::Cmd<Msg>::EmitHostSequence>(&c.inner))
        return e->sequence;
    return {};
}

} // namespace

TEST_CASE("maya emit_osc builds a well-formed OSC") {
    // ESC ] 7 ; hello ESC \   (ST-terminated, the spec-preferred form)
    auto c = CCmd::emit_osc(7, "hello");
    CHECK(sequence_of(c) == std::string("\x1b]7;hello\x1b\\"));
}

TEST_CASE("maya emit_osc carries an arbitrary numeric code + payload") {
    auto c = CCmd::emit_osc(1337, "path=/tmp/x.cpp;line=42");
    CHECK(sequence_of(c) ==
          std::string("\x1b]1337;path=/tmp/x.cpp;line=42\x1b\\"));
}

TEST_CASE("maya emit_host_sequence is verbatim") {
    const std::string raw = "\x1b]52;c;YWJj\x1b\\";   // an OSC 52 the caller pre-built
    auto c = CCmd::emit_host_sequence(raw);
    CHECK(sequence_of(c) == raw);
}

TEST_CASE("maya EmitHostSequence survives Cmd::map") {
    auto child = CCmd::emit_osc(9, "ping");
    PCmd parent = std::move(child).map([](ChildMsg m) { return ParentMsg{m.v}; });
    // map() must preserve the effect (it carries no Msg), not drop it.
    CHECK(sequence_of(parent) == std::string("\x1b]9;ping\x1b\\"));
}

TEST_CASE("maya EmitHostSequence is a distinct variant arm") {
    auto host = CCmd::emit_host_sequence("x");
    auto title = CCmd::set_title("t");
    CHECK(std::holds_alternative<CCmd::EmitHostSequence>(host.inner));
    CHECK(!std::holds_alternative<CCmd::EmitHostSequence>(title.inner));
    // Empty raw sequence is still representable (runtime no-ops it).
    CHECK(sequence_of(CCmd::emit_host_sequence("")) == "");
}
