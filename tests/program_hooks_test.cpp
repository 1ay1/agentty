// The jaal hooks AgenttyApp must actually expose.
//
// Every one of these is OPTIONAL in jaal: detected with a `requires` test, so
// a signature the kernel can't call is not an error — it reads as "this
// program doesn't have that hook" and the kernel quietly does without it.
//
// That is exactly how the jaal migration broke startup. `init` kept the old
// runtime's `static std::pair<Model, Cmd> init()` instead of jaal's
// `Cmd init(Model&)`, so `has_init` was false, the kernel value-initialised
// the Model, and everything init() loads — saved settings, the thread list,
// the active provider — was built and thrown away. It presented as agentty
// remembering nothing across restarts and showing the first-run starter card
// on every launch, with no diagnostic anywhere.
//
// A compile-time assert is the only thing that catches this class of bug:
// nothing about it fails at runtime, it just silently does less.
#include "agtest.hpp"

#include "agentty/runtime/app/program.hpp"

#include <maya/host/sources.hpp>
#include <maya/host/terminal.hpp>

using P = agentty::app::AgenttyApp;

// ── init ─────────────────────────────────────────────────────────────────
// THE regression. Must be callable as jaal calls it: fills in place, returns
// the first Cmd.
static_assert(requires(P::Model& m) {
                  { P::init(m) } -> std::convertible_to<P::Cmd>;
              },
              "AgenttyApp::init must be `Cmd init(Model&)`. jaal detects init "
              "with a requires-test, so a different signature silently means "
              "'no init' — the kernel hands the app a blank Model and every "
              "saved setting and thread is dropped at startup.");

// ── the rest of the contract ─────────────────────────────────────────────
static_assert(jaal::Program<P>,
              "AgenttyApp must satisfy jaal::Program.");
static_assert(maya::Program<P>,
              "AgenttyApp must satisfy maya::Program (jaal::Program + view()).");
static_assert(jaal::Subscribing<P>,
              "AgenttyApp::subscribe must be `Sub subscribe(const Model&)`; "
              "otherwise the app runs with NO event sources — no keys, no "
              "timers — and simply sits there.");
static_assert(jaal::HasVisualHash<P>,
              "AgenttyApp::visual_hash must be `std::uint64_t (const Model&)`; "
              "without it the host re-runs view() every wakeup.");
static_assert(jaal::HasNeedsWarmup<P>,
              "AgenttyApp::needs_warmup must be `bool (const Model&)`.");

// The host that actually runs it must be able to carry out every effect in
// the app's Cmd row. `require_host_for` names the offender when it can't.
TEST_CASE("program: agentty's Cmd row is runnable by maya's terminal host") {
    jaal::require_host_for<maya::terminal_host<P>, P>();
    CHECK(true);   // reaching here means the static checks above all passed
}
