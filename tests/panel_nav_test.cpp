// Panel navigation is a STACK: Esc walks back the way you came in.
//
// The failure this pins shipped, and it was reported as "Esc drops me all
// the way out instead of back to the panel I came from". Opening a panel had
// two spellings with different meanings:
//
//     m.ui.panel.descend(pn::X{...});   // stashes the parent — Esc unwinds
//     m.ui.panel.descend(pn::X{...});          // DROPS it   — Esc leaves entirely
//
// and which one a reducer used was a coin flip: eight Open* handlers
// descended, thirteen assigned. So palette → providers → Esc left the panel
// stack, while palette → models → Esc went back. Same gesture, two
// behaviours, decided by which opener you happened to be in.
//
// Assignment is deleted now, so a call site must say descend() (open) or
// restore() (put back, chain included) — but the property worth pinning is
// behavioural, not syntactic. These cases drive the REAL reducers through
// the real Msg types, because that is the level a user meets it at.

#include "agtest.hpp"

#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/panel/slot.hpp"

#include <print>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace pn = agentty::ui::panel;
using agentty::Model;
using agentty::Msg;

agentty::store::Settings g_settings;

// The reducers reach for injected IO; a nav test wants none of it to happen.
void install_stub_deps() {
    using namespace agentty;
    app::install_deps(app::Deps{
        .stream         = [](auto, auto) {},
        .save_thread    = [](const auto&) {},
        .delete_thread  = [](const auto&) {},
        .load_threads   = [] { return std::vector<Thread>{}; },
        .load_thread    = [](const auto&) -> std::optional<Thread> { return std::nullopt; },
        .load_settings  = [] { return g_settings; },
        .save_settings  = [](const store::Settings& x) { g_settings = x; },
        .new_thread_id  = [] { return ThreadId{}; },
        .title_from     = [](std::string_view t) { return std::string{t}; },
        .auth           = auth::AuthHeader{auth::ApiKeyHeader{std::string{}}},
    });
}

[[nodiscard]] Model step(Model m, Msg msg) {
    return std::move(agentty::app::update(std::move(m), std::move(msg)).first);
}

}  // namespace

TEST_CASE("panel nav: Esc returns to the panel you came from") {
    install_stub_deps();
    // The reported bug, end to end: open the palette, open providers from
    // it, press Esc. You should be back in the palette, not in the thread.
    Model m;
    m = step(std::move(m), Msg{agentty::OpenPalette{}});
    REQUIRE(m.ui.panel.is<pn::Palette>());
    CHECK(m.ui.panel.depth() == 1);

    m = step(std::move(m), Msg{agentty::OpenProviders{}});
    REQUIRE(m.ui.panel.is<pn::Providers>());
    // Two panels deep: the palette is stashed underneath.
    CHECK(m.ui.panel.depth() == 2);

    m = step(std::move(m), Msg{agentty::CloseProviders{}});
    CHECK(m.ui.panel.is<pn::Palette>());   // ← the bug: used to be None
    CHECK(m.ui.panel.depth() == 1);

    // A second Esc leaves, because the palette was opened over the thread.
    m = step(std::move(m), Msg{agentty::ClosePalette{}});
    CHECK(!m.ui.panel.any_open());
    CHECK(m.ui.panel.depth() == 0);
}

TEST_CASE("panel nav: a panel opened directly still closes on Esc") {
    install_stub_deps();
    // The other half of the contract. Descending over None stashes nothing,
    // so Esc closes rather than trying to restore a parent that never
    // existed. A stack that could only unwind would trap you in the panel.
    Model m;
    m = step(std::move(m), Msg{agentty::OpenProviders{}});
    REQUIRE(m.ui.panel.is<pn::Providers>());
    CHECK(m.ui.panel.depth() == 1);

    m = step(std::move(m), Msg{agentty::CloseProviders{}});
    CHECK(!m.ui.panel.any_open());
}

TEST_CASE("panel nav: the chain unwinds one level per Esc") {
    // Depth is the property the contract is actually about, so assert it
    // directly rather than inferring it from which panel is on top.
    pn::State s;
    s.descend(pn::Palette{});
    s.descend(pn::Models{{0, ""}});
    s.descend(pn::Providers{{0}});
    CHECK(s.depth() == 3);

    CHECK(s.ascend());
    CHECK(s.is<pn::Models>());
    CHECK(s.depth() == 2);

    CHECK(s.ascend());
    CHECK(s.is<pn::Palette>());
    CHECK(s.depth() == 1);

    // Bottom of the stack: nothing to restore, so the caller closes.
    CHECK(!s.ascend());
}

TEST_CASE("panel nav: re-opening the same panel rebuilds, it does not nest") {
    // A reducer that reopens its own panel to refresh it (Smart Mode does
    // this on every slot assignment) must not stash the panel as its own
    // parent — Esc would then peel identical copies one at a time and look
    // like it was doing nothing.
    pn::State s;
    s.descend(pn::Palette{});
    s.descend(pn::Models{{0, ""}});
    CHECK(s.depth() == 2);

    s.descend(pn::Models{{7, ""}});          // same kind, new contents
    CHECK(s.depth() == 2);                   // NOT 3
    CHECK(s.get<pn::Models>()->index == 7);

    // ...and the parent it inherited is still the palette.
    CHECK(s.ascend());
    CHECK(s.is<pn::Palette>());
}

TEST_CASE("panel nav: restore puts a panel back without re-stashing it") {
    // descend() and restore() are the only two ways in, and they must not be
    // interchangeable: a restore that descended would stash the child as its
    // own parent, so Esc would cycle between two copies instead of leaving.
    pn::State s;
    s.descend(pn::Palette{});
    s.descend(pn::Models{{3, ""}});

    auto snapshot = *s.get<pn::Models>();
    s.restore(std::move(snapshot));
    CHECK(s.depth() == 2);                   // unchanged, not 3

    CHECK(s.ascend());
    CHECK(s.is<pn::Palette>());
    CHECK(!s.ascend());
}

TEST_CASE("panel nav: a sideways hop keeps where you came from") {
    // The second chain-dropping bug, and a subtler one than assignment.
    //
    // Hopping between sibling pickers (^P from the model picker to the
    // provider picker) was spelled `close<Models>(); descend(Providers{});`.
    // That reads like a swap and behaves like a truncation: close() leaves
    // the slot empty, so the descend() that follows has nothing to stash and
    // silently drops the GRANDparent. palette → models → ^P → Esc then left
    // the panel stack entirely instead of returning to the palette.
    //
    // replace() is the move that says "sideways": the panel you leave goes,
    // the one underneath does not.
    pn::State s;
    s.descend(pn::Palette{});
    s.descend(pn::Models{{0, ""}});
    CHECK(s.depth() == 2);

    s.replace(pn::Providers{{4}});
    CHECK(s.is<pn::Providers>());
    CHECK(s.depth() == 2);                   // NOT 1 — the palette survives
    CHECK(s.get<pn::Providers>()->index == 4);

    CHECK(s.ascend());
    CHECK(s.is<pn::Palette>());
}

TEST_CASE("panel nav: a hop from nothing is just an open") {
    // replace() over None has no parent to inherit, so it lands exactly
    // where descend() would: one level deep, Esc closes. A hop has to work
    // when the sibling was opened cold, not only mid-stack.
    pn::State s;
    s.replace(pn::Providers{{0}});
    CHECK(s.is<pn::Providers>());
    CHECK(s.depth() == 1);
    CHECK(!s.ascend());                      // nothing to unwind to
}

TEST_CASE("panel nav: the three moves are distinguishable") {
    // descend / replace / restore differ ONLY in what happens to the parent
    // chain, which is exactly the part call sites kept getting wrong. Pin
    // all three against the same starting stack so the difference is the
    // subject of the test rather than a side effect of it.
    auto two_deep = [] {
        pn::State s;
        s.descend(pn::Palette{});
        s.descend(pn::Models{{1, ""}});
        return s;
    };

    auto down = two_deep();
    down.descend(pn::Providers{{0}});
    CHECK(down.depth() == 3);                // DOWN: one deeper

    auto side = two_deep();
    side.replace(pn::Providers{{0}});
    CHECK(side.depth() == 2);                // SIDEWAYS: same depth

    auto up = two_deep();
    auto same = *up.get<pn::Models>();
    up.restore(std::move(same));
    CHECK(up.depth() == 2);                  // UP/in-place: unchanged

    // And all three leave a stack that still unwinds to the palette.
    for (auto* s : {&down, &side, &up}) {
        while (s->depth() > 1) CHECK(s->ascend());
        CHECK(s->is<pn::Palette>());
    }
}

TEST_CASE("panel nav: opening is spelled, never implied") {
    // The syntactic half. Assignment used to be a third way in that silently
    // dropped the chain; it is deleted, so a call site has to name which of
    // the two it means. If someone restores operator=, this stops compiling
    // and the behavioural cases above become a coin flip again.
    static_assert(!std::is_assignable_v<pn::State&, pn::Palette>);
    static_assert(!std::is_assignable_v<pn::State&, pn::Models>);
    // Both real spellings stay available.
    static_assert(requires (pn::State s) { s.descend(pn::Palette{}); });
    static_assert(requires (pn::State s) { s.restore(pn::Palette{}); });
    std::println("PASS\n");
}
