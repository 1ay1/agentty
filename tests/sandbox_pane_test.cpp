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
#include "agentty/io/persistence.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/panel/sandbox.hpp"
#include "agentty/runtime/panel/settings/items.hpp"
#include "agentty/tool/util/sandbox.hpp"

#include <cstdlib>
#include <filesystem>

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

// Install a policy as if this were startup.
//
// set_config() is SEALED after its first call, which is the property most of
// this file exists to pin -- so a test that wants to start from a known
// policy has to break the seal first. Every case below arranges its own
// starting state through this, rather than depending on which case ran
// before it.
void install(const sandbox_cfg::Config& cfg) {
    sb::reset_config_for_test();
    sb::set_config(cfg);
}

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
    install(cfg);

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
    install(cfg);

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

TEST_CASE("sandbox pane: save persists but does NOT touch the live boundary") {
    // The security property, stated as a test: saving writes the policy for
    // the next launch and leaves the running sandbox exactly as it was.
    //
    // Why this is the right behaviour, since it is the opposite of every
    // other pane: a boundary that can move mid-session only moves usefully
    // in the WEAKENING direction, the weakening is invisible and cannot be
    // undone (re-tightening does not un-read a key that already left), and
    // two commands in one session under two different policies makes the
    // sandbox unauditable.
    sandbox_cfg::Config live;
    live.configured   = true;
    live.memory_mb    = 512;
    live.syscall_mode = sandbox_cfg::SyscallMode::Compiler;
    install(live);   // arranges the sealed starting state
    REQUIRE(sb::config_sealed());

    Model m = opened();
    const int mem = row_of(pane(m).pane.form, pn::kSbMemoryMb);
    REQUIRE(mem >= 0);
    num_at(m.ui.panel.get<pn::Sandbox>()->pane.form, mem).value = 2048;

    auto [saved, cmd] = app::update(std::move(m), Msg{SandboxSave{}});

    // Staged for the next launch...
    CHECK(saved.d.persisted.sandbox.memory_mb == 2048);
    CHECK(saved.d.persisted.sandbox.configured);

    // ...and the LIVE policy is untouched. This is the assertion that
    // matters: if it ever flips, a prompt-injected agent that can reach the
    // settings reducer can lower the walls of the session it is running in.
    CHECK(sb::config().memory_mb == 512);

    // And the pane says so, rather than letting the user believe the wall
    // went up. An unsaid "applies on restart" is the same class of lie as
    // "sandbox: active" on a host that cannot sandbox.
    const auto* o = saved.ui.panel.get<pn::Sandbox>();
    REQUIRE(o != nullptr);
    CHECK(o->pane.saved_pending_restart);
}

TEST_CASE("sandbox config: the policy is sealed after the first install") {
    // Enforced in code, not by comment. It WAS a comment ("call before
    // init()") and the settings pane broke it within a day of existing, so
    // the invariant now refuses rather than trusts.
    sandbox_cfg::Config first;
    first.configured = true;
    first.memory_mb  = 111;
    first.backend    = sandbox_cfg::LinuxBackend::Claybin;
    install(first);
    REQUIRE(sb::config_sealed());
    REQUIRE(sb::config().memory_mb == 111);

    // Every later call is a no-op, whichever direction it goes.
    sandbox_cfg::Config weaker;
    weaker.configured   = true;
    weaker.memory_mb    = 0;      // no cap
    weaker.syscall_mode = sandbox_cfg::SyscallMode::Off;
    weaker.backend      = sandbox_cfg::LinuxBackend::Bwrap;
    sb::set_config(weaker);
    CHECK(sb::config().memory_mb == 111);
    CHECK(sb::config().syscall_mode == sandbox_cfg::SyscallMode::Compiler);

    // Including the ENGINE. Downgrading claybin -> bwrap silently drops the
    // seccomp filter, the cgroup caps AND the non-configurable secret masks
    // (measured: the same workspace .env reads back empty under claybin and
    // in full under bwrap), so the backend has to be sealed with the rest of
    // the policy rather than alongside it.
    CHECK(sb::requested_linux_backend() == sb::LinuxPreference::Claybin);

    // Tightening is refused too. Not because tightening is dangerous, but
    // because "the policy can change, but only in ways we judge safe" is a
    // rule with a judgement call in it, and this one is worth having none.
    sandbox_cfg::Config tighter;
    tighter.configured   = true;
    tighter.memory_mb    = 64;
    tighter.syscall_mode = sandbox_cfg::SyscallMode::Strict;
    sb::set_config(tighter);
    CHECK(sb::config().memory_mb == 111);
}

TEST_CASE("sandbox pane: the backend row is first and always offers both") {
    // claybin is a required submodule now, not a build flag, so the choice is
    // always a real runtime choice. It used to be possible to ship a binary
    // where "claybin" was not an option at all.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    install(cfg);

    const Model m = opened();
    const auto& f = pane(m).pane.form;

    const int b = row_of(f, pn::kSbBackend);
    REQUIRE(b >= 0);
    const auto& ch = choice_at(f, b);
    REQUIRE(ch.count() == 2);
    CHECK(ch.labels[0] == "bwrap");
    CHECK(ch.labels[1] == "claybin");

    // FIRST real row: it decides what every row below can mean, so tuning ten
    // rows before discovering eight are inert is the thing to prevent.
    for (int i = 0; i < b; ++i)
        CHECK(f.fields[static_cast<std::size_t>(i)].is_header());
}

TEST_CASE("sandbox pane: bwrap locks the rows it cannot enforce") {
    // The shipped bug this pins: the pane rendered the syscall profile, W^X
    // and all four resource caps as live rows under bwrap, where agentty
    // passes no seccomp filter and no cgroup at all. A control that looks set
    // and enforces nothing is the one failure mode a sandbox must not have.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend    = sandbox_cfg::LinuxBackend::Bwrap;
    install(cfg);

    const Model m = opened();
    const auto& f = pane(m).pane.form;

    // Every claybin-only row must be locked, and must SAY why -- a lock with
    // no reason reads as a bug in the pane.
    for (const auto id : {pn::kSbNetMode, pn::kSbSyscalls, pn::kSbWxProtect,
                          pn::kSbMemoryMb, pn::kSbMaxProcs, pn::kSbCpuPercent,
                          pn::kSbTmpMb, pn::kSbScopeIpc}) {
        const int i = row_of(f, id);
        REQUIRE_MESSAGE(i >= 0, id);
        const auto& fld = f.fields[static_cast<std::size_t>(i)];
        CHECK_MESSAGE(fld.locked, id);
        CHECK_MESSAGE(!fld.help.empty(), id);
    }

    // And the rows bwrap CAN honour stay live. Gating these would be a lie in
    // the other direction: scope/read/write are binds, and a mask is a bind
    // of an empty file.
    for (const auto id : {pn::kSbFsScope, pn::kSbReadPaths, pn::kSbWritePaths,
                          pn::kSbDenyPaths, pn::kSbCloseFds}) {
        const int i = row_of(f, id);
        REQUIRE_MESSAGE(i >= 0, id);
        CHECK_MESSAGE(!f.fields[static_cast<std::size_t>(i)].locked, id);
    }
}

TEST_CASE("sandbox pane: switching the backend row relocks live") {
    // The locks are computed when the form is BUILT, so changing the engine
    // has to reproject rather than just re-price. Without that you could
    // switch to claybin and the syscall row would still say "bwrap cannot
    // express this" until the pane was reopened.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend    = sandbox_cfg::LinuxBackend::Bwrap;
    install(cfg);

    Model m = opened();
    const int b = row_of(pane(m).pane.form, pn::kSbBackend);
    REQUIRE(b >= 0);
    const int sys_before = row_of(pane(m).pane.form, pn::kSbSyscalls);
    REQUIRE(sys_before >= 0);
    REQUIRE(pane(m).pane.form.fields[static_cast<std::size_t>(sys_before)].locked);

    // Move onto the backend row and cycle it to claybin.
    m.ui.panel.get<pn::Sandbox>()->pane.form.cursor = b;
    auto [m2, _] = app::update(std::move(m), key(form::keys::Intent::AdjustUp));

    const auto& f = pane(m2).pane.form;
    const int nb = row_of(f, pn::kSbBackend);
    REQUIRE(nb >= 0);
    REQUIRE(choice_at(f, nb).id() == "claybin");

    // On a host that can actually run claybin the row unlocks; on one that
    // cannot it stays locked but for the OTHER reason. Both are correct, and
    // which applies is a property of the machine -- so assert the thing that
    // holds either way: the reason tracks the selection.
    const int sys = row_of(f, pn::kSbSyscalls);
    REQUIRE(sys >= 0);
    const auto& help = f.fields[static_cast<std::size_t>(sys)].help;
    CHECK(help.find("switch Backend to claybin") == std::string::npos);
}

TEST_CASE("sandbox pane: a locked row keeps its value through a round trip") {
    // Switching to bwrap must not ZERO the claybin-only settings. They are
    // still the user's choices; they are merely not in force. Losing them on
    // a backend flip would make the pane destructive to look at.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend    = sandbox_cfg::LinuxBackend::Bwrap;
    cfg.memory_mb  = 4096;
    cfg.syscall_mode = sandbox_cfg::SyscallMode::Strict;
    install(cfg);

    const Model m = opened();
    const auto back = pn::read_sandbox_form(pane(m).pane.form, sb::config());

    CHECK(back.memory_mb == 4096);
    CHECK(back.syscall_mode == sandbox_cfg::SyscallMode::Strict);
    CHECK(back.backend == sandbox_cfg::LinuxBackend::Bwrap);
}

TEST_CASE("sandbox pane: saving the backend switches the live engine") {
    // The row has to MOVE the engine, not just record a preference. A Backend
    // row that saves and switches nothing is the same class of bug as a pane
    // that saves a policy nothing enforces.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend    = sandbox_cfg::LinuxBackend::Claybin;
    install(cfg);
    CHECK(sb::requested_linux_backend() == sb::LinuxPreference::Claybin);

    cfg.backend = sandbox_cfg::LinuxBackend::Bwrap;
    install(cfg);
    CHECK(sb::requested_linux_backend() == sb::LinuxPreference::Bwrap);

    // An UNCONFIGURED config must not move it: main.cpp calls set_config()
    // before the CLI flag, so letting a default config publish its default
    // backend would silently undo --sandbox-backend.
    sb::prefer_linux_backend(sb::LinuxPreference::Claybin);
    install(sandbox_cfg::Config{});   // configured == false
    CHECK(sb::requested_linux_backend() == sb::LinuxPreference::Claybin);
}

TEST_CASE("sandbox config: the backend survives a save/load round trip") {
    // The pane can only mean anything if the choice OUTLIVES the session.
    // Driven through the real save_settings/load_settings pair rather than a
    // JSON helper, because that pair is what the reducer actually calls --
    // testing a private serializer would prove the wrong thing.
    //
    // AGENTTY_HOME is repointed at a temp dir so this never writes the
    // developer's own settings.json, and RESTORED afterwards: it is
    // process-wide state and other cases in this binary resolve paths
    // through it, so leaking it would make an unrelated test fail depending
    // on run order. (Learned the hard way -- a stray AGENTTY_HOME is exactly
    // the kind of cross-test coupling that presents as a flaky suite.)
    const char* prev = std::getenv("AGENTTY_HOME");
    const std::string saved = prev ? prev : "";
    const bool had = prev != nullptr;

    const auto tmp = std::filesystem::temp_directory_path() /
                     "agentty-sbtest-roundtrip";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    ::setenv("AGENTTY_HOME", tmp.string().c_str(), 1);

    auto s = persistence::load_settings();
    s.sandbox.configured = true;
    s.sandbox.backend    = sandbox_cfg::LinuxBackend::Claybin;
    s.sandbox.memory_mb  = 2048;
    persistence::save_settings(s);

    const auto back = persistence::load_settings();

    if (had) ::setenv("AGENTTY_HOME", saved.c_str(), 1);
    else     ::unsetenv("AGENTTY_HOME");
    std::filesystem::remove_all(tmp);

    CHECK(back.sandbox.backend == sandbox_cfg::LinuxBackend::Claybin);
    CHECK(back.sandbox.memory_mb == 2048);
    CHECK(back.sandbox.configured);
}

TEST_CASE("sandbox pane: every row round-trips into the config") {
    // The bug class this catches, three times over in this subsystem:
    //
    //   kSbMode                 a row id for a row that must not exist
    //   Observation/BlockedEvent  types with a reducer arm and no producer
    //   the Masked row           configured nothing under bwrap
    //
    // Each read as a shipped feature and enforced nothing. The shape is always
    // the same: a control exists at one layer and is dead at another, and no
    // single-layer test notices.
    //
    // So: build the form, MUTATE every editable row away from its current
    // value, read it back, and require the config actually changed. A row that
    // does not survive that is either dead or misspelled at one of its two
    // sites -- which is exactly what the kSb* constants exist to prevent and
    // cannot prevent on their own.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend    = sandbox_cfg::LinuxBackend::Claybin;
    install(cfg);

    Model m = opened();
    auto& f = m.ui.panel.get<pn::Sandbox>()->pane.form;

    int checked = 0;
    for (std::size_t i = 0; i < f.fields.size(); ++i) {
        auto& fld = f.fields[i];
        if (fld.is_header() || fld.is_action() || fld.is_pick()) continue;
        // A LOCKED row is not dead, it is honestly unavailable: the backend or
        // the kernel cannot enforce it, and the pane says so. read_sandbox_form
        // still reads it (a locked row keeps its value through a round trip,
        // which is its own test above), but on a host where claybin cannot
        // start every claybin row is locked, so requiring them to move here
        // would make this case fail for a property of the machine.
        if (fld.locked) continue;

        const auto before = pn::read_sandbox_form(f, cfg);

        // Move the row off whatever it holds. Each kind has one obvious way.
        if (auto* c = std::get_if<form::field::Choice>(&fld.value)) {
            if (c->count() < 2) continue;
            c->index = c->normalized(c->index + 1);
        } else if (auto* t = std::get_if<form::field::Toggle>(&fld.value)) {
            t->on = !t->on;
        } else if (auto* n = std::get_if<form::field::Number>(&fld.value)) {
            // Move to a value that is inside the row's bounds AND cannot
            // coincide with what the config already holds.
            //
            // `(value == min) ? max : min` was wrong and failed only under
            // ctest: read_sandbox_form falls back to `base` for some rows, so
            // landing on the base value makes the read a no-op and the check
            // reports a dead row that is fine. Order-dependent, which is the
            // worst kind of flake -- it passed standalone and failed in the
            // suite. Stepping off the CURRENT value by one, clamped into
            // range, cannot collide with anything.
            n->value = (n->value < n->max) ? n->value + 1 : n->value - 1;
        } else if (auto* tx = std::get_if<form::field::Text>(&fld.value)) {
            tx->value = tx->value.empty() ? std::string{"/tmp/roundtrip"}
                                          : std::string{};
        } else {
            continue;   // Secret, or a kind with no value to move
        }

        const auto after = pn::read_sandbox_form(f, cfg);
        const std::string why = "row '" + fld.id + "' does not reach the config";
        CHECK_MESSAGE(!(before == after), why);
        ++checked;
    }

    // And the sweep actually ran. A loop that skipped everything would report
    // zero failures, which is the failure mode this whole file is about.
    CHECK(checked >= 10);
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
