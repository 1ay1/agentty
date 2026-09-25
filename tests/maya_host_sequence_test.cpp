// maya_host_sequence_test — the emit_host_sequence host-escape primitive.
//
// Verifies the maya-level building blocks agentty uses to talk to a
// cooperating host terminal (Emacs/vterm's OSC hooks, etc.):
//   • emit_osc(code, payload) builds a well-formed, ST-terminated OSC,
//   • emit_host_sequence(raw) carries an arbitrary sequence verbatim,
//   • both survive Cmd::map() (the functor used to embed a child's Msg),
//   • the alternative is a distinct variant arm.
//
// Post-jaal shape: the effect is maya::EmitHostSequence (a payload struct in
// maya/host/effects.hpp), and a program's Cmd is jaal::Cmd<Msg, effects...>
// naming the effects it uses — not maya::Cmd<Msg>, which no longer exists.
// `inner` is still a plain std::variant, so the assertions are unchanged.
#include <doctest/doctest.h>

#include <maya/host/effects.hpp>

#include <string>
#include <variant>

namespace {

// Two unrelated Msg types so we can exercise map<Parent>(child_cmd).
struct ChildMsg { int v = 0; };
struct ParentMsg { int v = 0; };

// The row: just the two effects this test drives.
template <class Msg>
using Cmd = jaal::Cmd<Msg, maya::emit_host_sequence, maya::set_title>;

using CCmd = Cmd<ChildMsg>;
using PCmd = Cmd<ParentMsg>;

// Pull the raw sequence out of an EmitHostSequence Cmd, or "" otherwise.
template <class Msg>
std::string sequence_of(const Cmd<Msg>& c) {
    if (auto* e = std::get_if<maya::EmitHostSequence>(&c.inner))
        return e->sequence;
    return {};
}

// The OSC builder is a plain function on the payload now.
CCmd osc(int code, std::string payload) {
    return CCmd(maya::EmitHostSequence{
        "\x1b]" + std::to_string(code) + ";" + std::move(payload) + "\x1b\\"});
}

} // namespace

TEST_CASE("maya emit_osc builds a well-formed OSC") {
    // ESC ] 7 ; hello ESC \   (ST-terminated, the spec-preferred form)
    CHECK(sequence_of(osc(7, "hello")) == std::string("\x1b]7;hello\x1b\\"));
}

TEST_CASE("maya emit_osc carries an arbitrary numeric code + payload") {
    CHECK(sequence_of(osc(1337, "path=/tmp/x.cpp;line=42")) ==
          std::string("\x1b]1337;path=/tmp/x.cpp;line=42\x1b\\"));
}

TEST_CASE("maya emit_host_sequence is verbatim") {
    const std::string raw = "\x1b]52;c;YWJj\x1b\\";   // an OSC 52 the caller pre-built
    CCmd c = CCmd(maya::EmitHostSequence{raw});
    CHECK(sequence_of(c) == raw);
}

TEST_CASE("maya EmitHostSequence survives Cmd::map") {
    auto child = osc(9, "ping");
    PCmd parent = std::move(child).map([](ChildMsg m) { return ParentMsg{m.v}; });
    // map() must preserve the effect (it carries no Msg), not drop it.
    CHECK(sequence_of(parent) == std::string("\x1b]9;ping\x1b\\"));
}

TEST_CASE("maya EmitHostSequence is a distinct variant arm") {
    CCmd host  = CCmd(maya::EmitHostSequence{"x"});
    CCmd title = CCmd(maya::SetTitle{"t"});
    CHECK(std::holds_alternative<maya::EmitHostSequence>(host.inner));
    CHECK(!std::holds_alternative<maya::EmitHostSequence>(title.inner));
    // Empty raw sequence is still representable (runtime no-ops it).
    CHECK(sequence_of(CCmd(maya::EmitHostSequence{""})) == "");
}
