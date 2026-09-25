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
#include "agentty/runtime/app/host.hpp"

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
//
// app::Host, not maya::terminal_host: agentty's row carries its own
// persistence effects (save_thread, write_file, …) on top of maya's
// terminal ones, and app::Host is the wrapper that handles both. Asserting
// maya's host here would be asserting something agentty never runs on.
TEST_CASE("program: agentty's Cmd row is runnable by its host") {
    jaal::require_host_for<agentty::app::Host<P>, P>();
    CHECK(true);   // reaching here means the static checks above all passed
}

// ── The host's OWN optional hooks ─────────────────────────────────────
//
// Same requires-means-absent trap as the program hooks above, one layer
// down — and it cost a real debugging session.
//
// jaal calls a host's attach() only `if constexpr (requires { host.attach(cx) })`,
// where cx is `host_context<TheHost>`. maya's terminal_host originally
// declared `attach(host_context<terminal_host>&)` — exact type. agentty's
// Host DERIVES from it to add persistence effects, so its context is
// `host_context<Host>`, the requires-test failed, and the kernel silently
// skipped attach(). The terminal's input was never registered with the
// reactor: agentty opened, painted, and accepted no keys. Nothing logged,
// nothing failed — it just sat there.
//
// Fixed by templating maya's hooks on the context type. These assert the
// property that fix provides, because nothing else would notice losing it.
using H = agentty::app::Host<P>;

static_assert(requires(H& h, jaal::host_context<H>& cx) { h.attach(cx); },
              "Host::attach must accept host_context<Host>. jaal detects it "
              "with a requires-test, so a signature it can't call means the "
              "terminal's input is never registered — the app runs but takes "
              "no keys, silently.");

static_assert(requires(H& h, jaal::host_context<H>& cx, jaal::readiness r) {
                  h.on_ready(cx, r);
              },
              "Host::on_ready must accept host_context<Host>, or input that "
              "IS registered is never read.");

static_assert(requires(H& h, jaal::host_context<H>& cx, jaal::sig s) {
                  h.on_signal(cx, s);
              },
              "Host::on_signal must accept host_context<Host>, or SIGWINCH "
              "never reaches the program and a resize is ignored.");
