// The status bar's activity row is cached on a content key.
//
// That row is a measured degradation ladder: it builds each sub-widget as a
// real styled fragment and MEASURES it, up to eight candidate shapes, to pick
// the richest that fits. It is the right way to lay the row out and a poor
// thing to redo every frame — profiling a live streaming session put
// StatusBar::activity_row at 53% of ALL render time, because the ladder ran
// ~30 times a second whether or not anything in it had changed.
//
// So the host states the row's identity and maya caches the built row under
// it. The danger of a cache like this is not slowness, it is STALENESS: a key
// that misses an input paints a status bar that has quietly stopped tracking
// the thing it exists to report. These cases pin both halves — the key moves
// when the content moves, and it does not move when the content does not.

#include "agtest.hpp"

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/view/status_bar/status_bar.hpp"

#include <maya/app/inline.hpp>

#include <string>

namespace {

using agentty::Model;

[[nodiscard]] std::uint64_t key_of(const Model& m) {
    return agentty::ui::status_bar_config(m).content_key;
}

// A model with a plausible live turn in it, so the fields the ladder reads
// are all non-default. Returned by value — Model is move-only, so every
// case builds its own rather than copying a shared base.
[[nodiscard]] Model streaming_model() {
    Model m;
    m.s.phase = agentty::phase::Streaming{};
    return m;
}

}  // namespace

TEST_CASE("status bar: the row carries a content key") {
    // Opt-in caching: 0 means "rebuild every frame". agentty opts in, so a
    // zero key here would silently restore the 53% cost with no other symptom.
    const Model m = streaming_model();
    CHECK(key_of(m) != 0);
}

TEST_CASE("status bar: the key is stable for unchanged content") {
    // The whole point. Two configs built from the same model must agree, or
    // the cache never hits and the key is pure overhead.
    const Model m = streaming_model();
    CHECK(key_of(m) == key_of(m));

    const Model same = streaming_model();
    CHECK(key_of(same) == key_of(m));
}

TEST_CASE("status bar: the key moves when the row's content moves") {
    // Each of these is an input the ladder reads and renders. If the key
    // ignored one, the row would keep painting the previous value — the
    // failure this test exists to make impossible.
    const std::uint64_t k0 = key_of(streaming_model());

    {   // phase drives the verb, the glyph and the accent colour
        Model m = streaming_model();
        m.s.phase = agentty::phase::Idle{};
        CHECK(key_of(m) != k0);
    }
    {   // a status banner TAKES OVER the slot entirely
        Model m = streaming_model();
        m.s.status = "saved";   // non-empty with no deadline = active
        CHECK(key_of(m) != k0);
    }
}

TEST_CASE("status bar: a cached row never paints stale content") {
    // End to end through the renderer rather than through the key alone:
    // distinct content must produce distinct pixels even with caching on,
    // and identical content must be reproducible.
    auto render_with = [](const char* verb, std::uint64_t key) {
        maya::StatusBar::Config c;
        c.phase.verb = verb;
        c.phase.glyph = "*";
        c.phase.elapsed_secs = 1.0f;
        c.context.used = 100;
        c.context.max = 1000;
        c.context.cells = 10;
        c.content_key = key;
        return maya::render_to_string(maya::StatusBar{c}.build(), 120);
    };

    const std::string a = render_with("Thinking", 111);
    const std::string b = render_with("Running",  222);
    const std::string c = render_with("Thinking", 111);

    CHECK(a != b);                                   // not stale
    CHECK(a == c);                                   // and deterministic
    CHECK(a.find("Thinking") != std::string::npos);
    CHECK(b.find("Running")  != std::string::npos);
}
