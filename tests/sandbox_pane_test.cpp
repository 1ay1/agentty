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

// Host facts for a capable machine, with the RUNNING engine stated explicitly.
//
// Defaults to "running what the config selects", because most cases are about
// a row's content rather than about the selection/reality split. The cases
// that ARE about that split pass `running` by hand -- and that asymmetry is
// the point: claiming a wall is up is now something a test has to ask for.
pn::HostFacts facts_for(const sandbox_cfg::Config& cfg,
                        bool claybin_available = true,
                        std::uint32_t landlock_abi = 10) {
    pn::HostFacts f;
    f.claybin_available = claybin_available;
    f.landlock_abi = landlock_abi;
    f.sandbox_active = true;
    f.running = cfg.backend;
    return f;
}

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
        // The POSTURE row is an action, not a value: it is applied by the
        // reducer when it moves and deliberately not read back (see
        // read_sandbox_form). Moving it here changes nothing by design, and
        // its real behaviour has its own two cases above.
        if (fld.id == pn::kSbPosture) continue;

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

// ── Postures ─────────────────────────────────────────────────────────────
//
// The pane had 28 individually-correct rows and no way to express intent. Every
// row was real and enforced; assembling a security boundary out of 28
// primitives is still the wrong question, and a control nobody can reason about
// is a control nobody touches -- which for a security setting means off.
//
// The presets are only safe if two properties hold, and both are checked here:
// the label can never be wrong (it is DERIVED, not stored), and picking one
// cannot stomp a row the user edited afterwards.

TEST_CASE("sandbox posture: apply then detect round-trips for every preset") {
    // The property the whole feature rests on. detect_posture() asks
    // apply_posture() rather than re-encoding what a posture means in a second
    // place -- so this also guards the real hazard: ADD a field to Config, and
    // if detect had its own hand-written comparison it would start claiming
    // "Hardened" for a config that is not.
    for (auto p : {sandbox_cfg::Posture::Permissive,
                   sandbox_cfg::Posture::Balanced,
                   sandbox_cfg::Posture::Hardened,
                   sandbox_cfg::Posture::Airgapped}) {
        sandbox_cfg::Config base;
        const auto applied = sandbox_cfg::apply_posture(base, p);
        CHECK(sandbox_cfg::detect_posture(applied) == p);
    }
}

TEST_CASE("sandbox posture: the four presets are genuinely different") {
    // A preset list where two entries mean the same thing is a menu that lies
    // about having four options.
    sandbox_cfg::Config base;
    const auto perm = sandbox_cfg::apply_posture(base, sandbox_cfg::Posture::Permissive);
    const auto bal  = sandbox_cfg::apply_posture(base, sandbox_cfg::Posture::Balanced);
    const auto hard = sandbox_cfg::apply_posture(base, sandbox_cfg::Posture::Hardened);
    const auto air  = sandbox_cfg::apply_posture(base, sandbox_cfg::Posture::Airgapped);

    CHECK(!(perm == bal));
    CHECK(!(bal == hard));
    CHECK(!(hard == air));

    // And they are ordered the way their names claim. Permissive really has no
    // filter; airgapped really has no network. A preset called "airgapped" that
    // left the network on would be the worst bug in this file.
    CHECK(perm.syscall_mode == sandbox_cfg::SyscallMode::Off);
    CHECK(bal.net_mode == sandbox_cfg::NetMode::Full);
    CHECK(hard.syscall_mode == sandbox_cfg::SyscallMode::Strict);
    CHECK(air.net_mode == sandbox_cfg::NetMode::None);
}

TEST_CASE("sandbox posture: a preset keeps the user's paths and engine") {
    // A posture is a statement about how tight the walls are. Wiping the
    // project-specific path list because someone tried Hardened would make the
    // presets hostile -- you would lose work by exploring.
    sandbox_cfg::Config base;
    base.read_paths  = {"/opt/weird-sdk"};
    base.write_paths = {"/var/cache/project"};
    base.deny_paths  = {"/home/me/.cargo/credentials"};
    base.backend     = sandbox_cfg::LinuxBackend::Claybin;

    for (auto p : {sandbox_cfg::Posture::Permissive,
                   sandbox_cfg::Posture::Hardened,
                   sandbox_cfg::Posture::Airgapped}) {
        const auto out = sandbox_cfg::apply_posture(base, p);
        CHECK(out.read_paths == base.read_paths);
        CHECK(out.write_paths == base.write_paths);
        CHECK(out.deny_paths == base.deny_paths);
        // The engine especially: picking Hardened must not silently switch
        // engines on a host where claybin cannot start. The pane locks and
        // reports that instead.
        CHECK(out.backend == base.backend);
    }
}

TEST_CASE("sandbox posture: permissive still masks secrets and refuses handoffs") {
    // Permissive exists so a user whose toolchain breaks has somewhere to go
    // that is not `--sandbox off`. It would be worthless if it also gave up the
    // two protections that cost nothing in compatibility.
    sandbox_cfg::Config base;
    const auto perm = sandbox_cfg::apply_posture(base, sandbox_cfg::Posture::Permissive);

    // Secrets stay masked REGARDLESS: a mask is a bind, it breaks nothing.
    CHECK(perm.mask_scan_depth > 0);
    // And the handoff gate is a different question from the walls (§14) -- a
    // loose sandbox is a choice, letting the agent author your git hooks as a
    // side effect of that choice is not one anybody made.
    CHECK(perm.handoff == sandbox_cfg::HandoffPolicy::Refuse);
}

TEST_CASE("sandbox posture: balanced is exactly the unconfigured default") {
    // So a user who picks Balanced explicitly gets the same boundary as one who
    // never opened the pane. If these drift, the pane's own default row is
    // describing a policy nobody runs.
    sandbox_cfg::Config fresh;             // configured = false
    const auto bal = sandbox_cfg::apply_posture(fresh, sandbox_cfg::Posture::Balanced);

    sandbox_cfg::Config expected = fresh;
    expected.configured = true;            // the only legitimate difference
    CHECK(bal == expected);

    // And an untouched config already reads as Balanced, so the pane opens on a
    // name rather than on "Custom".
    CHECK(sandbox_cfg::detect_posture(fresh) == sandbox_cfg::Posture::Balanced);
}

TEST_CASE("sandbox posture: editing one row off a preset reports Custom") {
    // The honesty property. A STORED posture field would still say "Hardened"
    // here, and the pane would be claiming a boundary the config no longer
    // describes -- the same lie as "sandbox: active".
    auto hard = sandbox_cfg::apply_posture(sandbox_cfg::Config{},
                                           sandbox_cfg::Posture::Hardened);
    REQUIRE(sandbox_cfg::detect_posture(hard) == sandbox_cfg::Posture::Hardened);

    hard.memory_mb = 2048;   // one row, by hand
    CHECK(sandbox_cfg::detect_posture(hard) == sandbox_cfg::Posture::Custom);
}

TEST_CASE("sandbox posture: selecting Custom changes nothing") {
    // Custom is a label the row REPORTS, not a command. Applying it has to be a
    // no-op or the row would destroy the config it is describing.
    const auto hard = sandbox_cfg::apply_posture(sandbox_cfg::Config{},
                                                 sandbox_cfg::Posture::Hardened);
    const auto after = sandbox_cfg::apply_posture(hard, sandbox_cfg::Posture::Custom);
    CHECK(after == hard);
}

TEST_CASE("sandbox pane: the posture row writes every other row") {
    // Through the REDUCER, because that is where a preset is applied: picking
    // one is an action, and only the reducer knows an action happened. See the
    // kSbPosture arm in update/sandbox.cpp for why readback cannot do it.
    sandbox_cfg::Config cfg;   // balanced
    cfg.configured = true;
    install(cfg);

    Model m = opened();
    const int p = row_of(pane(m).pane.form, pn::kSbPosture);
    REQUIRE(p >= 0);
    m.ui.panel.get<pn::Sandbox>()->pane.form.cursor = p;

    // Drive the row to Hardened from wherever it actually starts, rather than
    // assuming an index. The pane seeds from `m.d.persisted` (so a fresh Model
    // opens on its defaults, not on what install() sealed), and a test that
    // hard-codes "one step down" silently tests a different preset the moment
    // that seed changes.
    const auto start = std::get<form::field::Choice>(
        pane(m).pane.form.fields[static_cast<std::size_t>(p)].value).index;
    const int want = static_cast<int>(sandbox_cfg::Posture::Hardened);
    // AdjustUp is +1 on a Choice (form_keys.cpp), so stepping toward a HIGHER
    // index is Up, not Down.
    for (int i = start; i < want; ++i) {
        auto [next, _] = app::update(std::move(m), key(form::keys::Intent::AdjustUp));
        m = std::move(next);
    }
    for (int i = start; i > want; --i) {
        auto [next, _] = app::update(std::move(m), key(form::keys::Intent::AdjustDown));
        m = std::move(next);
    }

    // The row really is where we drove it -- otherwise the checks below would
    // pass or fail for a reason that has nothing to do with presets.
    REQUIRE(std::get<form::field::Choice>(
        pane(m).pane.form.fields[static_cast<std::size_t>(p)].value).index == want);

    const auto out = pn::read_sandbox_form(pane(m).pane.form, cfg);
    // The preset wrote the OTHER rows, which is the whole point.
    CHECK(out.syscall_mode == sandbox_cfg::SyscallMode::Strict);
    CHECK(out.memory_mb == 8192);
    CHECK(sandbox_cfg::detect_posture(out) == sandbox_cfg::Posture::Hardened);
}

TEST_CASE("sandbox pane: a preset does not re-stamp a row edited after it") {
    // The failure this design exists to avoid, and the test that caught it.
    //
    // read_sandbox_form runs on EVERY keystroke and every reprice. The first
    // version applied the posture there, comparing the row against
    // detect_posture() -- which cannot work, because the row still displays the
    // posture the config USED to be. So editing Memory after choosing Hardened
    // put 8192 straight back on the next repaint: the user's edit vanishing
    // with no message, which is worse than refusing it.
    //
    // Applying in the reducer instead makes this exact, because the reducer
    // knows WHICH row moved.
    auto hard = sandbox_cfg::apply_posture(sandbox_cfg::Config{},
                                           sandbox_cfg::Posture::Hardened);
    hard.backend = sandbox_cfg::LinuxBackend::Claybin;
    install(hard);

    auto form = pn::build_sandbox_form(hard, facts_for(hard));
    // The row shows Hardened, as it should.
    const int p = row_of(form, pn::kSbPosture);
    REQUIRE(p >= 0);
    CHECK(std::get<form::field::Choice>(form.fields[static_cast<std::size_t>(p)].value).index
          == static_cast<int>(sandbox_cfg::Posture::Hardened));

    // Now edit a resource row by hand, exactly as the user would.
    const int mem = row_of(form, pn::kSbMemoryMb);
    REQUIRE(mem >= 0);
    std::get<form::field::Number>(form.fields[static_cast<std::size_t>(mem)].value).value = 2048;

    // Read back REPEATEDLY -- the repaint loop. The edit must survive all of
    // them, and the posture must settle on Custom rather than fighting back.
    auto out = pn::read_sandbox_form(form, hard);
    for (int i = 0; i < 3; ++i) out = pn::read_sandbox_form(form, out);

    CHECK(out.memory_mb == 2048);
    CHECK(sandbox_cfg::detect_posture(out) == sandbox_cfg::Posture::Custom);
}

TEST_CASE("sandbox pane: the posture row scores the walls") {
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;
    install(cfg);

    auto form = pn::build_sandbox_form(cfg, facts_for(cfg));
    const auto preview = pn::preview_sandbox(cfg);
    pn::annotate_sandbox_form(form, preview, cfg, pn::EngineStatus::InForce);

    const int p = row_of(form, pn::kSbPosture);
    REQUIRE(p >= 0);
    const auto& origin = form.fields[static_cast<std::size_t>(p)].origin;
    CHECK(!origin.empty());
#if defined(__linux__)
    if (preview.compiled) {
        // "N/M strong" -- the number is what makes two postures comparable at a
        // glance, where four safe-sounding names are not.
        CHECK(origin.find('/') != std::string::npos);
        CHECK(origin.find("strong") != std::string::npos);
    }
#endif
}

TEST_CASE("sandbox pane: a cap nobody asked for is not reported as a gap") {
    // From a screenshot: the footer read `resource.memory: none (none) ·
    // resource.cpu: none (none)` on a policy where nothing was wrong. Those
    // rows ship at 0 = "no cap", deliberately -- sandbox_config.hpp says the
    // right ceiling is a property of the machine and picking one for someone
    // else's laptop turns a working build into an OOM kill.
    //
    // Reporting them beside real degradations is how a wall report teaches the
    // user to ignore it.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;
    cfg.memory_mb = 0;      // no cap wanted
    cfg.cpu_percent = 0;
    cfg.max_procs = 0;

    const auto preview = pn::preview_sandbox(cfg);
#if defined(__linux__)
    if (preview.compiled) {
        for (const auto& w : preview.walls) {
            if (w.name == "resource.memory" || w.name == "resource.cpu" ||
                w.name == "resource.pids")
                CHECK(w.not_requested);
        }
    }
#endif

    // And asking for one flips it back: the flag tracks the REQUEST, not the
    // capability, so a cap that was asked for and failed is still a gap.
    cfg.memory_mb = 4096;
    const auto asked = pn::preview_sandbox(cfg);
#if defined(__linux__)
    if (asked.compiled) {
        for (const auto& w : asked.walls)
            if (w.name == "resource.memory") CHECK_FALSE(w.not_requested);
    }
#endif
}

TEST_CASE("sandbox pane: an unset cap row says so instead of showing none") {
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;
    cfg.memory_mb = 0;

    auto form = pn::build_sandbox_form(cfg, facts_for(cfg));
    const auto preview = pn::preview_sandbox(cfg);
    pn::annotate_sandbox_form(form, preview, cfg, pn::EngineStatus::InForce);

    const int mem = row_of(form, pn::kSbMemoryMb);
    REQUIRE(mem >= 0);
    const auto& origin = form.fields[static_cast<std::size_t>(mem)].origin;
#if defined(__linux__)
    if (preview.compiled) {
        // "none (none)" is accurate and useless -- it reads as a failure. The
        // row has to say the cap is unset, which is a choice.
        CHECK(origin.find("no cap") != std::string::npos);
        CHECK(origin.find("none") == std::string::npos);
    }
#endif
}

TEST_CASE("sandbox pane: empty path rows say they are empty") {
    // These rows used to be a Text field holding "a, b, c", and an empty one
    // rendered as a blank line that read as broken. They are Picks now -- they
    // show a count and open the list editor -- so the emptiness has to be
    // visible in the PICK's placeholder instead.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;
    cfg.read_paths.clear();
    cfg.write_paths.clear();
    cfg.deny_paths.clear();

    auto form = pn::build_sandbox_form(cfg, facts_for(cfg));
    pn::annotate_sandbox_form(form, pn::preview_sandbox(cfg), cfg,
                              pn::EngineStatus::InForce);

    for (auto id : {pn::kSbReadPaths, pn::kSbWritePaths, pn::kSbDenyPaths}) {
        const int i = row_of(form, id);
        REQUIRE(i >= 0);
        const auto& fld = form.fields[static_cast<std::size_t>(i)];
        const auto* p = std::get_if<form::field::Pick>(&fld.value);
        REQUIRE(p != nullptr);
        // Empty list => no count, and a placeholder that says how to add.
        CHECK(p->label.empty());
        CHECK(!p->placeholder.empty());
    }

    // And a NON-empty list reports its size rather than its contents: three
    // truncated paths in a narrow column tell you less than "3 paths" while
    // looking like they tell you more.
    cfg.read_paths = {"/opt/a", "/opt/b"};
    auto filled = pn::build_sandbox_form(cfg, facts_for(cfg));
    const int i = row_of(filled, pn::kSbReadPaths);
    REQUIRE(i >= 0);
    const auto* p = std::get_if<form::field::Pick>(
        &filled.fields[static_cast<std::size_t>(i)].value);
    REQUIRE(p != nullptr);
    CHECK(p->label == "2 paths");
}

// ── The path list editor ────────────────────────────────────────
//
// Comma-separated text was a serialisation format pretending to be an
// interface. These pin the list behaving like a list.

TEST_CASE("sandbox list: round-trips its entries") {
    const std::vector<std::string> paths{"/opt/sdk", "/var/cache/x"};
    auto ed = pn::build_sandbox_list(pn::kSbReadPaths, "Also readable", "help",
                                     paths, /*numeric=*/false);

    // One row per entry, plus the trailing blank that IS the add affordance.
    CHECK(ed.form.fields.size() == paths.size() + 1);
    CHECK(pn::read_sandbox_list(ed) == paths);
}

TEST_CASE("sandbox list: a blank entry is an absent entry") {
    // This is what makes add and remove the same gesture: typing into the
    // trailing blank adds, and clearing a line removes. No separate delete key
    // to discover, and no way to end up storing an empty path.
    auto ed = pn::build_sandbox_list(pn::kSbReadPaths, "t", "h",
                                     {"/a", "/b", "/c"}, false);
    // Clear the middle one.
    std::get<form::field::Text>(ed.form.fields[1].value).value.clear();
    const std::vector<std::string> want{"/a", "/c"};
    CHECK(pn::read_sandbox_list(ed) == want);
}

TEST_CASE("sandbox list: entries are trimmed") {
    // A path with a stray space does not error, it silently fails to match --
    // the exact failure mode the comma format had, and the reason this editor
    // exists.
    auto ed = pn::build_sandbox_list(pn::kSbReadPaths, "t", "h", {"x"}, false);
    std::get<form::field::Text>(ed.form.fields[0].value).value = "  /opt/sdk  ";
    const std::vector<std::string> want{"/opt/sdk"};
    CHECK(pn::read_sandbox_list(ed) == want);
}

TEST_CASE("sandbox list: a port list validates on readback") {
    auto ed = pn::build_sandbox_list(pn::kSbPorts, "Allowed ports", "h",
                                     {"443", "80"}, /*numeric=*/true);
    CHECK(ed.numeric);
    // TEXT rows, even for ports. maya's Number control has no caret field at
    // all, so a list you type into cannot use one -- "I don't see a caret in
    // the port list" was exactly that. The digit rule moves to readback.
    CHECK(std::holds_alternative<form::field::Text>(ed.form.fields[0].value));

    const std::vector<std::string> want{"443", "80"};
    CHECK(pn::read_sandbox_list(ed) == want);

    // Blank is absent, same rule as a path.
    std::get<form::field::Text>(ed.form.fields[0].value).value.clear();
    const std::vector<std::string> after{"80"};
    CHECK(pn::read_sandbox_list(ed) == after);
}

TEST_CASE("sandbox list: a non-port is dropped, not guessed at") {
    // Enforced on readback now that the widget cannot enforce it. Dropping
    // beats clamping: silently turning "8O80" (letter O) into 8080 would be
    // inventing a rule the user never asked for, on a security control.
    auto ed = pn::build_sandbox_list(pn::kSbPorts, "t", "h",
                                     {"443", "8O80", "99999", "0", "0443"},
                                     true);
    // 8O80 (letter O), 99999 (out of range) and 0 are dropped. "0443"
    // normalises to 443, which is then a DUPLICATE of the first entry and
    // collapses -- so one port survives, not two.
    const std::vector<std::string> want{"443"};
    CHECK(pn::read_sandbox_list(ed) == want);
}

TEST_CASE("sandbox list: opening one from the pane carries the current values") {
    // End to end: the Pick row hands off, and the editor is seeded from the
    // config the FORM describes rather than from the saved policy -- so an
    // unsaved posture change is not silently discarded by editing a list.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;
    cfg.read_paths = {"/opt/sdk"};
    install(cfg);

    Model m = opened();
    auto [m2, _] = app::update(std::move(m),
                               Msg{SandboxEditList{std::string{pn::kSbReadPaths}}});

    const auto* ed = m2.ui.panel.get<pn::SandboxList>();
    REQUIRE(ed != nullptr);
    CHECK(ed->pane.row_id == pn::kSbReadPaths);
    CHECK_FALSE(ed->pane.numeric);
    const std::vector<std::string> want{"/opt/sdk"};
    CHECK(pn::read_sandbox_list(ed->pane) == want);
}

TEST_CASE("sandbox list: closing commits back into the pane") {
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;
    cfg.read_paths.clear();
    install(cfg);

    Model m = opened();
    auto [m2, _] = app::update(std::move(m),
                               Msg{SandboxEditList{std::string{pn::kSbReadPaths}}});

    // Type a path into the trailing blank row.
    {
        auto* ed = m2.ui.panel.get<pn::SandboxList>();
        REQUIRE(ed != nullptr);
        std::get<form::field::Text>(ed->pane.form.fields.back().value).value =
            "/opt/sdk";
    }

    auto [m3, __] = app::update(std::move(m2), Msg{SandboxListClose{}});

    // Back on the sandbox pane, with the value in the config and the row
    // showing a count.
    const auto* o = m3.ui.panel.get<pn::Sandbox>();
    REQUIRE(o != nullptr);

    // Read back against the pane's WORKING config, not the sealed policy: the
    // path lists live outside the form (the row is a Pick showing a count), so
    // read_sandbox_form passes them through from its base. Using sb::config()
    // here would be asking the OLD policy what the new paths are.
    const auto out = pn::read_sandbox_form(o->pane.form, o->pane.working);
    const std::vector<std::string> want{"/opt/sdk"};
    CHECK(out.read_paths == want);

    // And it is already on disk. There is no ^S any more -- persisting and
    // applying were two different things, and only applying had to wait for a
    // restart (see the autosave note in the reducer).
    CHECK(m3.d.persisted.sandbox.read_paths == want);
}

TEST_CASE("sandbox list: typing builds the entry in order") {
    // Reported as "a always suffixed to list": typing "abc" came out reversed.
    //
    // Growing the list rebuilds the form (a new blank row has to appear after
    // the one you just filled), and build_sandbox_list makes FRESH Text fields
    // whose caret is 0. The value survived the rebuild and the caret did not,
    // so every keystroke after the first inserted at the start.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;
    cfg.read_paths.clear();
    install(cfg);

    Model m = opened();
    auto [m1, _] = app::update(std::move(m),
                               Msg{SandboxEditList{std::string{pn::kSbReadPaths}}});
    m = std::move(m1);

    // Put the cursor on the trailing blank row and type.
    {
        auto* ed = m.ui.panel.get<pn::SandboxList>();
        REQUIRE(ed != nullptr);
        ed->pane.form.cursor =
            static_cast<int>(ed->pane.form.fields.size()) - 1;
    }
    // The real key sequence: translate() emits TypeToEdit for the first
    // printable (it starts the session) and Insert for the rest. Sending
    // TypeToEdit three times would test a sequence the router never produces --
    // the later two would hit the !editing() guard and be dropped.
    {
        auto [next, __] = app::update(
            std::move(m),
            Msg{SandboxListKey{form::keys::Action{form::keys::Intent::TypeToEdit, U'a'}}});
        m = std::move(next);
    }
    for (char32_t ch : {U'b', U'c'}) {
        auto [next, __] = app::update(
            std::move(m),
            Msg{SandboxListKey{form::keys::Action{form::keys::Intent::Insert, ch}}});
        m = std::move(next);
    }

    const auto* ed = m.ui.panel.get<pn::SandboxList>();
    REQUIRE(ed != nullptr);
    const auto values = pn::read_sandbox_list(ed->pane);
    REQUIRE(values.size() == 1);
    CHECK(values[0] == "abc");   // not "cba"
}

TEST_CASE("sandbox list: Esc leaves the editor even from a live row") {
    // Reported as "esc doesn't work always". While a row is EDITING, apply()
    // turns Esc into LeaveField, which never reports `close` -- so the press
    // only ended the edit and the editor stayed open with nothing visibly
    // different.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;
    cfg.read_paths = {"/opt/sdk"};
    install(cfg);

    Model m = opened();
    auto [m1, _] = app::update(std::move(m),
                               Msg{SandboxEditList{std::string{pn::kSbReadPaths}}});
    m = std::move(m1);

    // Start editing the first entry.
    auto [m2, __] = app::update(
        std::move(m),
        Msg{SandboxListKey{form::keys::Action{form::keys::Intent::TypeToEdit, U'x'}}});
    m = std::move(m2);
    {
        const auto* ed = m.ui.panel.get<pn::SandboxList>();
        REQUIRE(ed != nullptr);
        REQUIRE(ed->pane.form.editing());   // the session really started
    }

    // Two Escs, always. The first ends the edit AND dismisses any suggestions
    // it opened -- they are one layer from the user's side. The second leaves
    // the editor. That the count does NOT vary with whether you had typed is
    // the fix: "esc doesn't work always" was a press being swallowed twice in
    // some states and once in others.
    for (int i = 0; i < 2; ++i) {
        auto [next, ___] = app::update(
            std::move(m),
            Msg{SandboxListKey{form::keys::Action{form::keys::Intent::Close, 0}}});
        m = std::move(next);
    }

    CHECK(m.ui.panel.get<pn::SandboxList>() == nullptr);
    CHECK(m.ui.panel.get<pn::Sandbox>() != nullptr);   // back on the pane
}

TEST_CASE("sandbox list: the caret starts at the END of an existing entry") {
    // Reported as "the caret starts at the front". A Text field defaults its
    // cursor to 0, so moving onto an entry that already read "/opt/sdk" and
    // typing put the character BEFORE the path.
    auto ed = pn::build_sandbox_list(pn::kSbReadPaths, "t", "h",
                                     {"/opt/sdk"}, false);
    const auto& t = std::get<form::field::Text>(ed.form.fields[0].value);
    CHECK(t.cursor == t.value.size());
}

TEST_CASE("sandbox list: backspacing to empty keeps the row and the session") {
    // Reported as "backspace doesn't work sometimes", and the "sometimes" was
    // the tell: it failed exactly when a row went empty.
    //
    // The grow logic used to REBUILD the form whenever the entry count
    // disagreed with the row count. Backspacing a row to empty made it absent
    // from read_sandbox_list(), so the rebuild deleted the row being edited --
    // focus moved, the edit session ended, and the next backspace had no field
    // to act on.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;
    cfg.read_paths = {"ab"};
    install(cfg);

    Model m = opened();
    auto [m1, _] = app::update(std::move(m),
                               Msg{SandboxEditList{std::string{pn::kSbReadPaths}}});
    m = std::move(m1);

    const std::size_t rows_before =
        m.ui.panel.get<pn::SandboxList>()->pane.form.fields.size();

    // Enter the field, then backspace the value away entirely.
    auto [m2, __] = app::update(
        std::move(m),
        Msg{SandboxListKey{form::keys::Action{form::keys::Intent::TypeToEdit, U'c'}}});
    m = std::move(m2);
    for (int i = 0; i < 3; ++i) {
        auto [next, ___] = app::update(
            std::move(m),
            Msg{SandboxListKey{form::keys::Action{form::keys::Intent::Backspace, 0}}});
        m = std::move(next);
    }

    const auto* ed = m.ui.panel.get<pn::SandboxList>();
    REQUIRE(ed != nullptr);
    // The row survived every keystroke, so backspace always had a target.
    CHECK(ed->pane.form.fields.size() == rows_before);
    CHECK(ed->pane.form.editing());
    // And it really is empty now -- the keystrokes landed.
    CHECK(std::get<form::field::Text>(ed->pane.form.fields[0].value).value.empty());
}

TEST_CASE("sandbox list: ^X removes the focused entry") {
    // "no way to remove the entry". Clearing a line works (blanks are dropped
    // on commit) but is not discoverable, so ^X -- which the form layer already
    // routes as ResetField and leaves to the pane -- deletes the row.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;
    cfg.read_paths = {"/a", "/b", "/c"};
    install(cfg);

    Model m = opened();
    auto [m1, _] = app::update(std::move(m),
                               Msg{SandboxEditList{std::string{pn::kSbReadPaths}}});
    m = std::move(m1);

    // Remove the middle one.
    m.ui.panel.get<pn::SandboxList>()->pane.form.cursor = 1;
    auto [m2, __] = app::update(
        std::move(m),
        Msg{SandboxListKey{form::keys::Action{form::keys::Intent::ResetField, 0}}});
    m = std::move(m2);

    const auto* ed = m.ui.panel.get<pn::SandboxList>();
    REQUIRE(ed != nullptr);
    const std::vector<std::string> want{"/a", "/c"};
    CHECK(pn::read_sandbox_list(ed->pane) == want);

    // The labels renumber, so the list does not read 1, 2, 2.
    CHECK(ed->pane.form.fields[0].label == "1.");
    CHECK(ed->pane.form.fields[1].label == "2.");
    // And the trailing add row is still there -- it is the affordance, not an
    // entry, so it must never be what ^X deletes.
    CHECK(ed->pane.form.fields.back().label == "+");
}

TEST_CASE("sandbox list: ^X cannot delete the add row") {
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;
    cfg.read_paths = {"/a"};
    install(cfg);

    Model m = opened();
    auto [m1, _] = app::update(std::move(m),
                               Msg{SandboxEditList{std::string{pn::kSbReadPaths}}});
    m = std::move(m1);

    // Park on the trailing "+" row and try to remove it.
    {
        auto* ed = m.ui.panel.get<pn::SandboxList>();
        ed->pane.form.cursor =
            static_cast<int>(ed->pane.form.fields.size()) - 1;
    }
    auto [m2, __] = app::update(
        std::move(m),
        Msg{SandboxListKey{form::keys::Action{form::keys::Intent::ResetField, 0}}});
    m = std::move(m2);

    const auto* ed = m.ui.panel.get<pn::SandboxList>();
    REQUIRE(ed != nullptr);
    CHECK(ed->pane.form.fields.back().label == "+");
    const std::vector<std::string> want{"/a"};
    CHECK(pn::read_sandbox_list(ed->pane) == want);
}

TEST_CASE("sandbox pane: the ports row opens even when Access is not `ports`") {
    // It used to be LOCKED unless the mode was already `ports`, which made it
    // unreachable: lock_row makes activate() return Nothing, so Enter did not
    // open the editor and there was no way to fill in ports at all. The row
    // told you to set Access first and would not let you act.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;
    cfg.net_mode = sandbox_cfg::NetMode::Full;   // NOT ports
    install(cfg);

    auto form = pn::build_sandbox_form(cfg, facts_for(cfg));
    const int p = row_of(form, pn::kSbPorts);
    REQUIRE(p >= 0);
    CHECK_FALSE(form.fields[static_cast<std::size_t>(p)].locked);
}

TEST_CASE("sandbox pane: adding a port switches Access to `ports`") {
    // Adding a port is an unambiguous statement of intent, so it sets the mode
    // rather than sitting inert under `full`. Otherwise the user fills in a
    // port list that does nothing and nothing says why.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;
    cfg.net_mode = sandbox_cfg::NetMode::Full;
    cfg.allow_ports.clear();
    install(cfg);

    Model m = opened();
    auto [m1, _] = app::update(std::move(m),
                               Msg{SandboxEditList{std::string{pn::kSbPorts}}});
    m = std::move(m1);

    {
        auto* ed = m.ui.panel.get<pn::SandboxList>();
        REQUIRE(ed != nullptr);
        REQUIRE(ed->pane.numeric);
        std::get<form::field::Text>(ed->pane.form.fields.back().value).value = "8080";
    }
    auto [m2, __] = app::update(std::move(m), Msg{SandboxListClose{}});

    const auto* o = m2.ui.panel.get<pn::Sandbox>();
    REQUIRE(o != nullptr);
    CHECK(o->pane.working.net_mode == sandbox_cfg::NetMode::Ports);
    const std::vector<std::uint16_t> want{8080};
    CHECK(o->pane.working.allow_ports == want);
}

TEST_CASE("sandbox pane: emptying the port list leaves `ports` mode") {
    // The inverse, and it matters more: `ports` with an empty list is a policy
    // that denies ALL network, which is never what deleting the last port
    // meant. Falling back to the default beats leaving a trap.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;
    cfg.net_mode = sandbox_cfg::NetMode::Ports;
    cfg.allow_ports = {443};
    install(cfg);

    Model m = opened();
    auto [m1, _] = app::update(std::move(m),
                               Msg{SandboxEditList{std::string{pn::kSbPorts}}});
    m = std::move(m1);

    // Clear the one entry (blank is absent, same as a path).
    {
        auto* ed = m.ui.panel.get<pn::SandboxList>();
        REQUIRE(ed != nullptr);
        std::get<form::field::Text>(ed->pane.form.fields[0].value).value.clear();
    }
    auto [m2, __] = app::update(std::move(m), Msg{SandboxListClose{}});

    const auto* o = m2.ui.panel.get<pn::Sandbox>();
    REQUIRE(o != nullptr);
    CHECK(o->pane.working.allow_ports.empty());
    CHECK(o->pane.working.net_mode != sandbox_cfg::NetMode::Ports);
}

TEST_CASE("sandbox pane: the form teaches itself") {
    // The standard: a user should be able to learn this pane FROM the pane.
    // Nobody arrives knowing what W^X is, what `toolchain` grants, or what
    // turning Scope IPC off costs them -- and a security control you cannot
    // reason about is one you leave at its default forever, which makes every
    // row below the first one dead weight.
    //
    // So this is a completeness check, not a spelling check: every settable
    // row explains itself, and every ENUM OPTION explains itself separately,
    // because "which of these three" is a different question from "what is
    // this row".
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;
    install(cfg);

    const auto form = pn::build_sandbox_form(cfg, facts_for(cfg));

    int rows = 0;
    for (const auto& f : form.fields) {
        if (f.is_header()) continue;
        ++rows;

        // Every row says what it is. A label alone is a name, not an
        // explanation -- "W^X" means nothing to someone who has not met it.
        //
        // Messages are built COMPLETELY before the macro: doctest streams its
        // argument into a MessageBuilder, so any `+` inside the call tries to
        // concatenate onto the builder instead of onto a string.
        const std::string who = "row '" + f.id + "'";
        const std::string no_help = who + " has no help text";
        CHECK_MESSAGE(!f.help.empty(), no_help);

        // Every option of a pick-one row says what IT does. Without this the
        // row explains the question and leaves the answers as bare words:
        // "toolchain / minimal / host-readable" names three things and
        // defines none of them.
        if (const auto* c = std::get_if<form::field::Choice>(&f.value)) {
            const std::string miss = who + " is missing per-option hints";
            CHECK_MESSAGE(c->hints.size() == c->labels.size(), miss);
            for (std::size_t i = 0; i < c->hints.size(); ++i) {
                const std::string blank =
                    who + " option " + std::to_string(i) + " has an empty hint";
                CHECK_MESSAGE(!c->hints[i].empty(), blank);
            }
        }
    }
    // And the sweep actually ran -- a loop that skipped everything would
    // report zero failures, which is the failure mode this file is about.
    CHECK(rows >= 15);
}

TEST_CASE("sandbox pane: a toggle says what its CURRENT state does") {
    // A toggle renders as nothing but on/off, so a single help line can only
    // describe the row, never the choice. Without state-dependent help the
    // only way to learn what turning something off costs is to turn it off --
    // the wrong way round for a security control.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;

    cfg.wx_protect = true;
    install(cfg);
    const auto on = pn::build_sandbox_form(cfg, facts_for(cfg));
    const int i = row_of(on, pn::kSbWxProtect);
    REQUIRE(i >= 0);
    const auto help_on = on.fields[static_cast<std::size_t>(i)].help;

    // build_sandbox_form is pure over (config, facts), so flipping the config
    // and rebuilding is the whole test -- no reinstall needed.
    cfg.wx_protect = false;
    const auto off = pn::build_sandbox_form(cfg, facts_for(cfg));
    const auto help_off = off.fields[static_cast<std::size_t>(i)].help;

    // The two states read differently, and each names its own consequence.
    CHECK(help_on != help_off);
    CHECK(help_on.find("on:") != std::string::npos);
    CHECK(help_off.find("off:") != std::string::npos);
}

TEST_CASE("sandbox list: backspace works on the FIRST press after opening") {
    // Reported as "backspace on the selected row doesn't work first".
    //
    // Two bugs stacked. The router did not translate Backspace while browsing
    // at all, so the key was lost before apply() ever saw it; and even once it
    // arrived, apply() requires an edit session and the editor opens in
    // Browsing. Typing a character auto-enters and Backspace did not, so the
    // two ways of starting to edit the same row disagreed.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;
    cfg.read_paths = {"abc"};
    install(cfg);

    Model m = opened();
    auto [m1, _] = app::update(std::move(m),
                               Msg{SandboxEditList{std::string{pn::kSbReadPaths}}});
    m = std::move(m1);

    // Exactly ONE press, straight after opening, with no edit started.
    auto [m2, __] = app::update(
        std::move(m),
        Msg{SandboxListKey{form::keys::Action{form::keys::Intent::Backspace, 0}}});
    m = std::move(m2);

    const auto* ed = m.ui.panel.get<pn::SandboxList>();
    REQUIRE(ed != nullptr);
    const std::vector<std::string> want{"ab"};
    CHECK(pn::read_sandbox_list(ed->pane) == want);
    // And it left a live session, so the NEXT press continues rather than
    // needing its own wake-up.
    CHECK(ed->pane.form.editing());
}

TEST_CASE("sandbox list: backspace on an empty row is harmless") {
    // The form layer's reason for not auto-entering on delete keys is that a
    // stray Backspace must never destroy bytes the user cannot see a caret in.
    // That still has to hold here: an empty row has nothing to delete, so the
    // press must not start a session or disturb anything.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;
    cfg.read_paths.clear();
    install(cfg);

    Model m = opened();
    auto [m1, _] = app::update(std::move(m),
                               Msg{SandboxEditList{std::string{pn::kSbReadPaths}}});
    m = std::move(m1);

    const std::size_t rows =
        m.ui.panel.get<pn::SandboxList>()->pane.form.fields.size();

    auto [m2, __] = app::update(
        std::move(m),
        Msg{SandboxListKey{form::keys::Action{form::keys::Intent::Backspace, 0}}});
    m = std::move(m2);

    const auto* ed = m.ui.panel.get<pn::SandboxList>();
    REQUIRE(ed != nullptr);
    CHECK_FALSE(ed->pane.form.editing());
    CHECK(ed->pane.form.fields.size() == rows);
    CHECK(pn::read_sandbox_list(ed->pane).empty());
}

TEST_CASE("sandbox list: duplicates collapse, keeping the first position") {
    // Two identical binds are one bind and two identical ports are one port,
    // but a duplicate is easy to create by accident (type a path, scroll, type
    // it again) and keeping both makes the row's count disagree with what the
    // policy actually does.
    auto ed = pn::build_sandbox_list(pn::kSbReadPaths, "t", "h",
                                     {"/a", "/b", "/a", "/c", "/b"}, false);
    const std::vector<std::string> want{"/a", "/b", "/c"};
    CHECK(pn::read_sandbox_list(ed) == want);
}

TEST_CASE("sandbox pane: a path in two lists is flagged, not silently resolved") {
    // The compiler cannot catch this: a grant and a mask are both valid
    // mounts, so which wins comes down to emission order. The user asked for
    // two incompatible things and the policy quietly picked one.
    //
    // The pane REPORTS rather than resolves, deliberately -- "masked" and
    // "writable" are equally plausible readings of the same two rows, so
    // picking a winner would be inventing an intent.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;
    cfg.write_paths = {"/opt/shared"};
    cfg.deny_paths  = {"/opt/shared"};
    install(cfg);

    auto form = pn::build_sandbox_form(cfg, facts_for(cfg));
    pn::annotate_sandbox_form(form, pn::preview_sandbox(cfg), cfg,
                              pn::EngineStatus::InForce);

    const int d = row_of(form, pn::kSbDenyPaths);
    REQUIRE(d >= 0);
    const auto& err = form.fields[static_cast<std::size_t>(d)].error;
    CHECK(!err.empty());
    CHECK(err.find("/opt/shared") != std::string::npos);
    CHECK(err.find("writable") != std::string::npos);
}

TEST_CASE("sandbox pane: a relative path is flagged") {
    // It compiles and it quietly does not do what it reads like: the sandbox
    // resolves it against the CHILD's working directory, not the pane's, so
    // `build` usually matches nothing at all.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;
    cfg.read_paths = {"build"};
    install(cfg);

    auto form = pn::build_sandbox_form(cfg, facts_for(cfg));
    pn::annotate_sandbox_form(form, pn::preview_sandbox(cfg), cfg,
                              pn::EngineStatus::InForce);

    const int r = row_of(form, pn::kSbReadPaths);
    REQUIRE(r >= 0);
    CHECK(form.fields[static_cast<std::size_t>(r)].error.find("relative")
          != std::string::npos);

    // An absolute path is fine, and so is ~ -- the shell expands it before the
    // sandbox ever sees it.
    cfg.read_paths = {"/opt/sdk", "~/.cargo"};
    auto ok = pn::build_sandbox_form(cfg, facts_for(cfg));
    pn::annotate_sandbox_form(ok, pn::preview_sandbox(cfg), cfg,
                              pn::EngineStatus::InForce);
    const int r2 = row_of(ok, pn::kSbReadPaths);
    REQUIRE(r2 >= 0);
    CHECK(ok.fields[static_cast<std::size_t>(r2)].error.empty());
}

TEST_CASE("sandbox list: browsing past an entry does not open suggestions") {
    // `completing` used to be computed from the row's TEXT alone, so arrowing
    // onto an existing entry popped the completion list open for a row the
    // user had not typed a character into -- and then stole their next Esc to
    // dismiss a list they never asked for, which reads as Esc being swallowed.
    //
    // Completion is an affordance for the thing you are WRITING. Browsing past
    // a value is not writing it.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;
    cfg.read_paths = {"src", "include"};
    install(cfg);

    Model m = opened();
    auto [m1, _] = app::update(std::move(m),
                               Msg{SandboxEditList{std::string{pn::kSbReadPaths}}});
    m = std::move(m1);

    // Move down a row -- no typing anywhere.
    auto [m2, __] = app::update(
        std::move(m),
        Msg{SandboxListKey{form::keys::Action{form::keys::Intent::MoveNext, 0}}});
    m = std::move(m2);

    const auto* ed = m.ui.panel.get<pn::SandboxList>();
    REQUIRE(ed != nullptr);
    CHECK_FALSE(ed->pane.form.editing());
    CHECK_FALSE(ed->pane.completing);
}

TEST_CASE("sandbox list: accepting a suggestion settles instead of looping") {
    // Tab writes the hit into the row and closes the list. But `completing` is
    // recomputed on the NEXT keystroke from the row text, and the picker's
    // query still held the half-typed prefix -- so the list reopened
    // immediately, now matching the full accepted path. Tab had the effect of
    // accepting a suggestion and being offered it straight back.
    //
    // Syncing the query to what was written is what makes accepting terminal.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;
    cfg.read_paths.clear();
    install(cfg);

    Model m = opened();
    auto [m1, _] = app::update(std::move(m),
                               Msg{SandboxEditList{std::string{pn::kSbReadPaths}}});
    m = std::move(m1);

    // Type into the trailing blank row to open suggestions.
    {
        auto* ed = m.ui.panel.get<pn::SandboxList>();
        REQUIRE(ed != nullptr);
        ed->pane.form.cursor =
            static_cast<int>(ed->pane.form.fields.size()) - 1;
    }
    auto [m2, __] = app::update(
        std::move(m),
        Msg{SandboxListKey{form::keys::Action{form::keys::Intent::TypeToEdit, U's'}}});
    m = std::move(m2);

    // Only meaningful if the workspace index actually produced candidates;
    // otherwise there is nothing to accept and nothing to assert.
    if (!m.ui.panel.get<pn::SandboxList>()->pane.completing) return;

    auto [m3, ___] = app::update(
        std::move(m),
        Msg{SandboxListKey{form::keys::Action{form::keys::Intent::Complete, 0}}});
    m = std::move(m3);

    const auto* ed = m.ui.panel.get<pn::SandboxList>();
    REQUIRE(ed != nullptr);
    CHECK_FALSE(ed->pane.completing);

    // The query now matches the accepted value, so the next repaint-driving
    // key cannot decide the user typed something and reopen the list.
    const auto* row = ed->pane.form.focused();
    REQUIRE(row != nullptr);
    const auto& t = std::get<form::field::Text>(row->value);
    CHECK(ed->pane.complete.query() == t.value);
    CHECK(t.cursor == t.value.size());
}

TEST_CASE("sandbox list: no key sequence leaves the editor inconsistent") {
    // A state sweep rather than another named case.
    //
    // This pane now has three overlapping modes (browsing, editing,
    // completing) and a dozen keys that each touch two of them. Every bug in
    // the last several rounds was a SEAM between two of those -- a flag that
    // outlived its cause, a key that meant different things depending on an
    // invisible bit -- and naming them one at a time only ever finds the ones
    // already reported.
    //
    // So: drive every intent from every reachable state and assert the
    // invariants that must hold no matter what.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;
    cfg.read_paths = {"/opt/a", "/opt/b"};
    install(cfg);

    using I = form::keys::Intent;
    const I keys[] = {
        I::MoveNext, I::MovePrev, I::MoveFirst, I::MoveLast,
        I::TypeToEdit, I::Insert, I::Backspace, I::DeleteForward,
        I::CaretLeft, I::CaretRight, I::CaretHome, I::CaretEnd,
        I::Complete, I::ResetField, I::Activate, I::AdjustUp, I::AdjustDown,
    };

    // A few deterministic orderings rather than a random walk: a seeded
    // shuffle would make a failure depend on a seed nobody has, and these
    // cover the interleavings the real bugs came from.
    for (std::size_t start = 0; start < std::size(keys); ++start) {
        Model m = opened();
        auto [m1, _] = app::update(
            std::move(m), Msg{SandboxEditList{std::string{pn::kSbReadPaths}}});
        m = std::move(m1);

        for (std::size_t step = 0; step < std::size(keys); ++step) {
            const auto k = keys[(start + step) % std::size(keys)];
            auto [next, __] = app::update(
                std::move(m),
                Msg{SandboxListKey{form::keys::Action{k, U'a'}}});
            m = std::move(next);

            const auto* ed = m.ui.panel.get<pn::SandboxList>();
            if (!ed) break;   // Esc-equivalent closed it; nothing left to check

            // 1. Suggestions imply an edit session. The converse is false (you
            //    can edit without suggestions), but a completion list floating
            //    over a row nobody is writing is what ate the user's Esc.
            if (ed->pane.completing)
                CHECK(ed->pane.form.editing());

            // 2. The cursor is always on a real row. An erase that forgot to
            //    clamp would index past the end on the next keystroke.
            CHECK(ed->pane.form.cursor >= 0);
            CHECK(ed->pane.form.cursor <
                  static_cast<int>(ed->pane.form.fields.size()));

            // 3. There is always exactly one trailing add row, and it is the
            //    last one. Lose it and the list can never grow again.
            REQUIRE(!ed->pane.form.fields.empty());
            CHECK(ed->pane.form.fields.back().label == "+");

            // 4. Entry labels stay in order: 1., 2., ... with no gaps. A
            //    removal that skipped renumbering reads as the list having
            //    lost an entry it still holds.
            for (std::size_t i = 0; i + 1 < ed->pane.form.fields.size(); ++i)
                CHECK(ed->pane.form.fields[i].label ==
                      std::to_string(i + 1) + ".");

            // 5. Every row is a Text row. The port list uses them too, and a
            //    stray Number would silently lose its caret.
            for (const auto& f : ed->pane.form.fields)
                CHECK(std::holds_alternative<form::field::Text>(f.value));
        }
    }
}

// ── "applies on restart" has to be a checked claim ─────────────────────
//
// The footer promised "saved · applies on restart" with NOTHING checking that
// it would. Every ingredient of the answer was already computed -- the compile
// result, the unenforceable list, the host probe -- and none of them was
// joined into the claim being made.
//
// Same bug as "sandbox: active" with no sandbox and as the subtitle naming
// walls that were not up, only pointed at the future instead of the present.

TEST_CASE("sandbox restart: a clean policy promises plainly") {
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;

    const auto facts = facts_for(cfg);
    const auto preview = pn::preview_sandbox(cfg);

    // Empty verdict == the restart really will deliver what the rows describe.
    // The footer may then make its promise without qualification.
    if (preview.compiled && preview.unenforceable.empty())
        CHECK(pn::restart_outcome(cfg, facts, preview).empty());
}

TEST_CASE("sandbox restart: a policy that will not compile says so") {
    // The hard no. claybin refuses rather than degrading, so there is no
    // "partly" here -- the next launch gets nothing from this config, and a
    // footer saying "applies on restart" would be flatly false.
    sandbox_cfg::Config cfg;
    cfg.configured = true;

    pn::Preview broken;
    broken.compiled = false;
    broken.error = "fabricated: this host cannot build it";

    const auto out = pn::restart_outcome(cfg, facts_for(cfg), broken);
    REQUIRE(!out.empty());
    CHECK(out.find("NOT apply") != std::string::npos);
    // And it carries the REASON, not just the refusal -- otherwise the user
    // has no way to act on it.
    CHECK(out.find("fabricated") != std::string::npos);
}

TEST_CASE("sandbox restart: an engine that cannot start names the fallback") {
    // Saving is still legitimate -- the config is portable and the user may be
    // on a different machine tomorrow. But the promise must not imply the
    // claybin walls will be up, and it has to say what WILL be enforced
    // instead, because "claybin cannot start" alone reads as "no sandbox".
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;

    pn::HostFacts facts;
    facts.claybin_available = false;        // the host refuses it
    facts.sandbox_active = true;
    facts.running = sandbox_cfg::LinuxBackend::Bwrap;

    pn::Preview ok;
    ok.compiled = true;

    const auto out = pn::restart_outcome(cfg, facts, ok);
    REQUIRE(!out.empty());
    CHECK(out.find("cannot start") != std::string::npos);
    CHECK(out.find("bwrap") != std::string::npos);
}

TEST_CASE("sandbox restart: a silently degraded capability is named") {
    // The quiet case, and the reason the function exists. It compiles, the
    // engine starts, and some wall still comes out weaker than asked. Without
    // this the user believes in the part that degraded -- which is the whole
    // failure mode this subsystem is built around.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;

    pn::Preview degraded;
    degraded.compiled = true;
    degraded.unenforceable.push_back("network.isolation needs landlock abi 4");

    const auto out = pn::restart_outcome(cfg, facts_for(cfg), degraded);
    REQUIRE(!out.empty());
    // It still applies -- so the verdict must not read as a refusal.
    CHECK(out.find("applies on restart") != std::string::npos);
    // But the gap is named.
    CHECK(out.find("network.isolation") != std::string::npos);
}

TEST_CASE("sandbox restart: the worst problem wins the one line the footer has") {
    // Ordering is not cosmetic. The footer shows ONE line, so when several
    // things are wrong the user needs the one that most invalidates the
    // promise: a policy that will not compile makes the unenforceable list
    // irrelevant, not merely less important.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;

    pn::Preview worst;
    worst.compiled = false;
    worst.error = "refused";
    worst.unenforceable.push_back("resource.cpu");

    pn::HostFacts facts;
    facts.claybin_available = false;   // ALSO cannot start
    facts.sandbox_active = true;

    const auto out = pn::restart_outcome(cfg, facts, worst);
    CHECK(out.find("NOT apply") != std::string::npos);
    // Not the lesser problems.
    CHECK(out.find("resource.cpu") == std::string::npos);
}

TEST_CASE("sandbox pane: the reducer keeps the restart verdict with the preview") {
    // End to end through the pane: the verdict is computed in reprice(), with
    // the preview that was built from the SAME config, so the two cannot
    // disagree about the policy they are describing. The view only reads it --
    // it cannot probe, and must not re-derive the config to find out.
    Model m = opened();
    const auto& p = pane(m).pane;

    // Whatever the verdict is, it must be CONSISTENT with the preview beside
    // it: a non-compiling policy cannot have an empty (all-clear) verdict.
    if (!p.preview.compiled)
        CHECK(!p.restart_note.empty());
    // And a clean compile with nothing unenforceable, on a host running the
    // selected engine, must not invent a problem.
    if (p.preview.compiled && p.preview.unenforceable.empty() &&
        pn::engine_status(sb::config(), p.facts) == pn::EngineStatus::InForce)
        CHECK(p.restart_note.empty());
}

// ── The subtitle must describe REALITY, not the selection ────────────────
//
// Reported from a screenshot: selecting claybin while running under bwrap
// instantly repainted the header to "claybin · landlock abi 10 · seccomp ·
// cgroup2". None of those walls existed -- the policy is sealed at startup, so
// the process was still bwrap and could not change.
//
// That is issue #21 re-entered through the front door, in the very pane built
// to prevent it. The root cause was that the running engine was never a fact
// the form could see: `SandboxPane::backend` held it and had no reader at all,
// so the subtitle was derived from the SELECTED engine plus host capability.
//
// These cases pin the fix at every altitude it could regress.

TEST_CASE("sandbox pane: selecting claybin under bwrap does not claim seccomp") {
    // The exact screenshot. The selection says claybin; reality says bwrap.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;

    pn::HostFacts facts;
    facts.claybin_available = true;          // the host COULD run it
    facts.landlock_abi = 10;
    facts.sandbox_active = true;
    facts.running = sandbox_cfg::LinuxBackend::Bwrap;   // but it is not running

    const auto form = pn::build_sandbox_form(cfg, facts);

    // The walls that do not exist must not be named in the present tense.
    CHECK(form.subtitle.find("seccomp") == std::string::npos);
    CHECK(form.subtitle.find("cgroup2") == std::string::npos);
    CHECK(form.subtitle.find("landlock") == std::string::npos);
    // What IS running has to be stated.
    CHECK(form.subtitle.find("bwrap") != std::string::npos);
    // And the pending selection, as a forecast rather than a fact.
    CHECK(form.subtitle.find("restart") != std::string::npos);
}

TEST_CASE("sandbox pane: running claybin DOES claim its walls") {
    // The honesty has to cut both ways: a pane that never credits claybin is
    // just as useless as one that always does.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;

    pn::HostFacts facts;
    facts.claybin_available = true;
    facts.landlock_abi = 10;
    facts.sandbox_active = true;
    facts.running = sandbox_cfg::LinuxBackend::Claybin;   // actually running

    const auto form = pn::build_sandbox_form(cfg, facts);
    CHECK(form.subtitle.find("claybin") != std::string::npos);
    CHECK(form.subtitle.find("seccomp") != std::string::npos);
    CHECK(form.subtitle.find("landlock abi 10") != std::string::npos);
    // Nothing pending, so no disclaimer.
    CHECK(form.subtitle.find("restart") == std::string::npos);
}

TEST_CASE("sandbox pane: an inactive sandbox never names an engine") {
    // `running` is meaningless when nothing is enforcing (probe failed, or
    // --sandbox off). Naming bwrap there would be the same lie in miniature.
    sandbox_cfg::Config cfg;
    cfg.configured = true;

    pn::HostFacts facts;
    facts.sandbox_active = false;
    facts.running = sandbox_cfg::LinuxBackend::Bwrap;

    const auto form = pn::build_sandbox_form(cfg, facts);
    CHECK(form.subtitle.find("unconfined") != std::string::npos);
    CHECK(form.subtitle.find("mount namespaces") == std::string::npos);
}

TEST_CASE("sandbox pane: describe_running ignores the config entirely") {
    // The structural guarantee, checked directly: this function takes no
    // config, so no future edit can make the running description depend on
    // what the user selected. Same facts, wildly different configs, identical
    // answer.
    pn::HostFacts facts;
    facts.claybin_available = true;
    facts.landlock_abi = 10;
    facts.sandbox_active = true;
    facts.running = sandbox_cfg::LinuxBackend::Bwrap;

    const auto baseline = pn::describe_running(facts);
    CHECK(baseline.find("bwrap") != std::string::npos);

    // Whatever the user picks, the running line cannot move.
    for (auto p : {sandbox_cfg::Posture::Hardened, sandbox_cfg::Posture::Airgapped,
                   sandbox_cfg::Posture::Permissive}) {
        auto cfg = sandbox_cfg::apply_posture(sandbox_cfg::Config{}, p);
        cfg.backend = sandbox_cfg::LinuxBackend::Claybin;
        CHECK(pn::describe_running(facts) == baseline);
        (void)cfg;
    }
}

TEST_CASE("sandbox pane: engine_status separates wanted from running") {
    sandbox_cfg::Config cfg;
    cfg.configured = true;

    pn::HostFacts facts;
    facts.claybin_available = true;
    facts.sandbox_active = true;

    // bwrap selected, bwrap running.
    cfg.backend = sandbox_cfg::LinuxBackend::Bwrap;
    facts.running = sandbox_cfg::LinuxBackend::Bwrap;
    CHECK(pn::engine_status(cfg, facts) == pn::EngineStatus::InForce);

    // claybin selected, bwrap running -- the screenshot.
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;
    CHECK(pn::engine_status(cfg, facts) == pn::EngineStatus::AppliesOnRestart);

    // claybin selected, host refuses it. Outranks the comparison: "applies on
    // restart" would be a promise nothing can keep.
    facts.claybin_available = false;
    CHECK(pn::engine_status(cfg, facts) == pn::EngineStatus::CannotStart);
}

TEST_CASE("sandbox pane: forecast walls are marked per row, not just in the header") {
    // §15 moved the wall report onto the rows precisely because the footer is
    // what people skip. That makes each row a place the same lie could appear:
    // "strong via seccomp-bpf" in the present tense, beside a row, on a process
    // with no seccomp filter.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;

    pn::HostFacts facts;
    facts.claybin_available = true;
    facts.landlock_abi = 10;
    facts.sandbox_active = true;
    facts.running = sandbox_cfg::LinuxBackend::Bwrap;

    auto form = pn::build_sandbox_form(cfg, facts);
    const auto preview = pn::preview_sandbox(cfg);
    pn::annotate_sandbox_form(form, preview, cfg,
                              pn::engine_status(cfg, facts));

#if defined(__linux__)
    if (preview.compiled) {
        const int sys = row_of(form, pn::kSbSyscalls);
        REQUIRE(sys >= 0);
        const auto& origin = form.fields[static_cast<std::size_t>(sys)].origin;
        REQUIRE(!origin.empty());
        // It may still say what the wall WOULD be -- that is the forecast the
        // pane exists to show -- but it must not read as present tense.
        CHECK(origin.find("restart") != std::string::npos);
    }
#endif
}

// ── Per-row honesty ──────────────────────────────────────────────────────
//
// The wall report was correct and unread: twelve capabilities folded into one
// footer line, where the single `none` looked exactly like the eleven
// `strong`s beside it. These pin the fix -- the report reaching the ROW the
// user is editing, which is the only place the question gets asked.

TEST_CASE("sandbox pane: a row carries the wall that enforces it") {
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;
    install(cfg);

    auto form = pn::build_sandbox_form(cfg, facts_for(cfg));
    const auto preview = pn::preview_sandbox(cfg);
    pn::annotate_sandbox_form(form, preview, cfg, pn::EngineStatus::InForce);

#if defined(__linux__)
    if (preview.compiled) {
        // The mechanism, not just the strength. "strong" alone is the same
        // unfalsifiable claim as "sandbox: active"; "strong via landlock abi
        // 10" has its receipt attached, and that distinction is the entire
        // reason this annotation exists.
        const int sys = row_of(form, pn::kSbSyscalls);
        REQUIRE(sys >= 0);
        const auto& origin = form.fields[static_cast<std::size_t>(sys)].origin;
        CHECK(!origin.empty());
        CHECK(origin.find("via") != std::string::npos);
    }
#endif
    // A row with no capability behind it must not borrow a neighbour's wall:
    // "Also readable" is a bind list, and labelling it with filesystem.read
    // would be a claim about the wrong thing. It may still carry a plain
    // "none" to say the list is empty (see "empty path rows are labelled"),
    // which is a statement about ITS OWN value rather than about a wall.
    const int extra = row_of(form, pn::kSbReadPaths);
    REQUIRE(extra >= 0);
    const auto& extra_origin = form.fields[static_cast<std::size_t>(extra)].origin;
    CHECK(extra_origin.find("via") == std::string::npos);
    CHECK(extra_origin.find("strong") == std::string::npos);
}

TEST_CASE("sandbox pane: per-port network with no ports says it denies all") {
    // The dangerous shape: this COMPILES, and it does not mean what it reads
    // like. The user asked to allow a specific set and allowed nothing, so
    // claybin has nothing to refuse and the footer stays quiet. Only the row
    // can catch it.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;
    cfg.net_mode = sandbox_cfg::NetMode::Ports;
    cfg.allow_ports.clear();
    install(cfg);

    auto form = pn::build_sandbox_form(cfg, facts_for(cfg));
    pn::annotate_sandbox_form(form, pn::preview_sandbox(cfg), cfg, pn::EngineStatus::InForce);

    const int ports = row_of(form, pn::kSbPorts);
    REQUIRE(ports >= 0);
    CHECK(!form.fields[static_cast<std::size_t>(ports)].error.empty());

    // And it goes away once the config is coherent -- an error that never
    // clears is a label, and users learn to read past labels.
    cfg.allow_ports = {443};
    auto ok = pn::build_sandbox_form(cfg, facts_for(cfg));
    pn::annotate_sandbox_form(ok, pn::preview_sandbox(cfg), cfg, pn::EngineStatus::InForce);
    const int ports2 = row_of(ok, pn::kSbPorts);
    REQUIRE(ports2 >= 0);
    CHECK(ok.fields[static_cast<std::size_t>(ports2)].error.empty());
}

TEST_CASE("sandbox pane: turning the syscall filter off says brokering goes too") {
    // Coupling the user cannot infer: the broker IS the filter deciding at
    // runtime, so Off disables supervision of ptrace and kill as well. One row
    // silently disarming another is exactly what a settings pane must not do.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;
    cfg.syscall_mode = sandbox_cfg::SyscallMode::Off;
    install(cfg);

    auto form = pn::build_sandbox_form(cfg, facts_for(cfg));
    pn::annotate_sandbox_form(form, pn::preview_sandbox(cfg), cfg, pn::EngineStatus::InForce);

    const int sys = row_of(form, pn::kSbSyscalls);
    REQUIRE(sys >= 0);
    const auto& err = form.fields[static_cast<std::size_t>(sys)].error;
    CHECK(!err.empty());
    CHECK(err.find("brokering") != std::string::npos);
}

TEST_CASE("sandbox pane: loosening the handoff policy warns on the row") {
    // The one setting here that WIDENS the blast radius, and it does so
    // outside the sandbox where no wall reports on it -- the footer's wall
    // report structurally cannot mention it. So the row has to carry it.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;
    cfg.handoff = sandbox_cfg::HandoffPolicy::Allow;
    install(cfg);

    auto form = pn::build_sandbox_form(cfg, facts_for(cfg));
    pn::annotate_sandbox_form(form, pn::preview_sandbox(cfg), cfg, pn::EngineStatus::InForce);

    const int h = row_of(form, pn::kSbHandoff);
    REQUIRE(h >= 0);
    CHECK(!form.fields[static_cast<std::size_t>(h)].error.empty());

    // Refuse is the default and must be silent: a warning on the SAFE setting
    // trains the user to ignore the warning on the unsafe one.
    cfg.handoff = sandbox_cfg::HandoffPolicy::Refuse;
    auto safe = pn::build_sandbox_form(cfg, facts_for(cfg));
    pn::annotate_sandbox_form(safe, pn::preview_sandbox(cfg), cfg, pn::EngineStatus::InForce);
    const int h2 = row_of(safe, pn::kSbHandoff);
    REQUIRE(h2 >= 0);
    CHECK(safe.fields[static_cast<std::size_t>(h2)].error.empty());
}

TEST_CASE("sandbox pane: a policy that will not compile annotates no walls") {
    // The worst possible lie would be a confident per-row "strong" read off a
    // stale preview. When the compile fails, every annotated row must say it
    // does not know rather than repeat the last thing that worked.
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.backend = sandbox_cfg::LinuxBackend::Claybin;
    install(cfg);

    auto form = pn::build_sandbox_form(cfg, facts_for(cfg));
    pn::Preview broken;
    broken.compiled = false;
    broken.error = "fabricated: the host cannot build this";
    pn::annotate_sandbox_form(form, broken, cfg, pn::EngineStatus::InForce);

    for (auto id : {pn::kSbFsScope, pn::kSbNetMode, pn::kSbSyscalls}) {
        const int i = row_of(form, id);
        if (i < 0) continue;
        const auto& origin = form.fields[static_cast<std::size_t>(i)].origin;
        // Not a strength claim. Either empty or the em-dash placeholder, but
        // never "strong".
        CHECK(origin.find("strong") == std::string::npos);
    }
}
