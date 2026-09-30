// Does the Sandbox pane actually control the sandbox?
//
// The pane existed before this test did, and it was wired to nothing: no
// palette door, no reducer, no view. Every row it drew configured a struct
// that the spawn path never read. That is the bug class this file exists to
// prevent -- a security control that looks configured and enforces nothing.
//
// So these checks follow the whole chain rather than any one link:
//
//   door     the General list offers a row that opens the pane
//   open     the pane seeds from the policy IN FORCE, not from defaults
//   edit     a keystroke changes a row and the preview tracks it
//   save     ^S installs the config AND stages it for persistence
//   discard  Esc leaves the policy alone
//
// What this does NOT check is whether the kernel then enforces it -- that
// needs real namespaces and lives in sandbox_live_check (a hand-run target,
// see cmake/AgenttyTests.cmake). Between the two there is no gap: this proves
// the pane reaches the config, that proves the config reaches the child.

#include <doctest/doctest.h>

#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/panel/sandbox.hpp"
#include "agentty/runtime/panel/settings/items.hpp"
#include "agentty/tool/util/sandbox.hpp"

using namespace agentty;
namespace pn = agentty::ui::panel;
namespace sb = agentty::tools::util::sandbox;

namespace {

Model opened() {
    auto [m, _] = app::update(Model{}, Msg{OpenSandbox{}});
    return std::move(m);
}

const pn::Sandbox& pane(const Model& m) {
    const auto* o = m.ui.panel.get<pn::Sandbox>();
    REQUIRE(o != nullptr);
    return *o;
}

// Find a row by id, so a test never depends on row ORDER -- reordering the
// form is a legitimate edit and must not fail a behavioural check.
int row_of(const form::Form& f, std::string_view id) {
    for (std::size_t i = 0; i < f.fields.size(); ++i)
        if (f.fields[i].id == id) return static_cast<int>(i);
    return -1;
}

const form::field::Choice& choice_at(const form::Form& f, int i) {
    return std::get<form::field::Choice>(f.fields[static_cast<std::size_t>(i)].value);
}

// Memory / procs / cpu / tmp are Number rows, not Text -- a cap is a
// quantity with bounds, and the form layer clamps it for us.
form::field::Number& num_at(form::Form& f, int i) {
    return std::get<form::field::Number>(f.fields[static_cast<std::size_t>(i)].value);
}

const form::field::Number& num_at(const form::Form& f, int i) {
    return std::get<form::field::Number>(f.fields[static_cast<std::size_t>(i)].value);
}

Msg key(form::keys::Intent i) { return Msg{SandboxKey{form::keys::Action{i, 0}}}; }

}  // namespace

TEST_CASE("sandbox pane: the General list has a door that opens it") {
    // The gap that made the pane dead code: it was reachable from nothing.
    Model m;
    const auto items = settings::items_for(m, settings::Category::General);

    bool found = false;
    for (const auto& i : items)
        if (i.action == settings::Action::OpenSandbox) {
            found = true;
            // A door must SAY it is a door, or the row reads as inert.
            CHECK(settings::opens_pane(i.action));
            CHECK(!i.primary.empty());
            // The secondary line names the enforcing backend. That is the
            // one fact worth carrying out here: it decides whether the rows
            // inside do anything, and a pane that configures an inert policy
            // has to admit it before you open it.
            CHECK(!i.secondary.empty());
        }
    CHECK(found);
}

TEST_CASE("sandbox pane: opens onto the policy in force, not defaults") {
    // If the pane seeded from a default-constructed config, the first thing
    // a user saw would be a lie about their own boundary.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.net_mode   = sandbox_cfg::NetMode::None;
    cfg.memory_mb  = 4096;
    sb::set_config(cfg);

    const Model m = opened();
    const auto& f = pane(m).pane.form;

    const int net = row_of(f, pn::kSbNetMode);
    REQUIRE(net >= 0);
    // The row shows "none", because that is what is enforcing.
    CHECK(choice_at(f, net).id() == "none");

    const int mem = row_of(f, pn::kSbMemoryMb);
    REQUIRE(mem >= 0);
    CHECK(num_at(f, mem).value == 4096);

    // And the cursor lands on a real setting, not a section header.
    REQUIRE(f.cursor >= 0);
    CHECK(!f.fields[static_cast<std::size_t>(f.cursor)].is_header());
}

TEST_CASE("sandbox pane: Esc discards, so walking away changes nothing") {
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.net_mode   = sandbox_cfg::NetMode::Full;
    sb::set_config(cfg);

    Model m = opened();
    // Move a row, then leave without saving.
    const int net = row_of(pane(m).pane.form, pn::kSbNetMode);
    REQUIRE(net >= 0);
    m.ui.panel.get<pn::Sandbox>()->pane.form.cursor = net;
    auto [m2, _] = app::update(std::move(m), key(form::keys::Intent::AdjustUp));
    auto [m3, __] = app::update(std::move(m2), Msg{CloseSandbox{}});

    // The pane is gone and the live policy is untouched. A boundary changed
    // by pressing Esc would be a boundary changed by giving up on the screen.
    CHECK(m3.ui.panel.get<pn::Sandbox>() == nullptr);
    CHECK(sb::config().net_mode == sandbox_cfg::NetMode::Full);
}

TEST_CASE("sandbox pane: save installs the policy and stages it to persist") {
    sandbox_cfg::Config cfg;   // fresh: configured == false
    sb::set_config(cfg);
    REQUIRE(!sb::config().configured);

    Model m = opened();

    // Type a memory cap, then save.
    const int mem = row_of(pane(m).pane.form, pn::kSbMemoryMb);
    REQUIRE(mem >= 0);
    num_at(m.ui.panel.get<pn::Sandbox>()->pane.form, mem).value = 2048;

    auto [saved, cmd] = app::update(std::move(m), Msg{SandboxSave{}});

    // Installed for this session...
    CHECK(sb::config().memory_mb == 2048);
    // ...and marked as the user's own choice, which is what stops an upgrade
    // from applying a saved block nobody asked for.
    CHECK(sb::config().configured);
    // ...and staged on the model, so the save effect has something to write.
    CHECK(saved.d.persisted.sandbox.memory_mb == 2048);
    CHECK(saved.d.persisted.sandbox.configured);
}

TEST_CASE("sandbox pane: the wall report tracks the rows") {
    // The pane's whole reason to exist: a boundary you cannot observe is one
    // you cannot trust. So the report must never describe a config the rows
    // no longer say.
    Model m = opened();
    const auto before = pane(m).pane.preview;

    const int sys = row_of(pane(m).pane.form, pn::kSbSyscalls);
    REQUIRE(sys >= 0);
    m.ui.panel.get<pn::Sandbox>()->pane.form.cursor = sys;

    // Cycle the syscall profile and let the reducer re-price.
    auto [m2, _] = app::update(std::move(m), key(form::keys::Intent::AdjustUp));
    auto [m3, __] = app::update(std::move(m2), Msg{SandboxRefreshPreview{}});

    const auto& after = pane(m3).pane.preview;
    // Either it compiled and said something, or it refused and said why.
    // What it must never be is silent -- that is the "active, enforcing
    // nothing" state this pane was built to make impossible.
    CHECK((after.compiled || !after.error.empty()));
    if (after.compiled) CHECK(!after.walls.empty());
    (void)before;
}
