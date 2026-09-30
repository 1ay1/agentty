// agentty has TWO sandbox implementations, and both must agree.
//
//   agentty::tools::util::sandbox  — lifecycle hooks, external ACP agents
//   mcp::tools::util::sandbox      — shell, diagnostics, git, process_start
//
// They are initialized separately (main.cpp, then wire_mcp_runtime) and each
// probes independently. Porting a backend into one and not the other is the
// failure this pins: the banner would describe one engine while every hook
// ran under another — the same shape as issue #21, where "sandbox: active"
// was printed on a host with no working sandbox.
//
// So: whatever backend one selects, the other must select too, under every
// environment the user can set.

#include "agtest.hpp"

#include "agentty/tool/util/sandbox.hpp"
#include "agentty/tool/mcp_tools_bridge.hpp"
#include <mcp/tools/util/sandbox.hpp>

#include <cstdlib>
#include <string>

namespace ag = agentty::tools::util::sandbox;
namespace mc = mcp::tools::util::sandbox;

namespace {

// The two enums are distinct types with the same meaning; compare by name so
// a reordering of either cannot make a mismatch look like agreement.
std::string name_of(ag::Backend b) {
    switch (b) {
        case ag::Backend::None:        return "none";
        case ag::Backend::Bwrap:       return "bwrap";
        case ag::Backend::SandboxExec: return "sandbox-exec";
    }
    return "?";
}
std::string name_of(mc::Backend b) {
    switch (b) {
        case mc::Backend::None:        return "none";
        case mc::Backend::Bwrap:       return "bwrap";
        case mc::Backend::SandboxExec: return "sandbox-exec";
    }
    return "?";
}

}  // namespace

TEST_CASE("sandbox: wiring the runtime hands mcp OUR sandbox") {
    // The hook is the mechanism the parity check below now rests on, so it
    // gets its own case: if wire_mcp_runtime ever stops installing it, this
    // fails with a clear cause instead of the parity case failing with a
    // confusing one.
    //
    // The gap it closes: mcp-cpp probes for its OWN backend, and it cannot
    // reach claybin (standalone library, our submodule). So before the hook,
    // `shell` ran under mcp's bwrap while agentty's hooks and the settings
    // pane used claybin -- two engines, one banner, and a pane configuring a
    // boundary the tool the user actually invokes never read.
    agentty::tools::wire_mcp_runtime("auto");
    CHECK(mc::has_host_sandbox());

    // And "off" must NOT install it: with no sandbox requested there is
    // nothing to delegate, and a hook that declined every call would just be
    // an indirection.
    agentty::tools::wire_mcp_runtime("off");
    CHECK(!mc::has_host_sandbox());
}

TEST_CASE("sandbox: both implementations know the same backends") {
    // A backend added to one side only is the bug. This is a compile-time
    // check in practice — a backend missing from either enum won't build in
    // name_of — but assert the mapping too, so a silent renumbering is caught.
    CHECK(name_of(ag::Backend::Bwrap)       == name_of(mc::Backend::Bwrap));
    CHECK(name_of(ag::Backend::SandboxExec) == name_of(mc::Backend::SandboxExec));
    CHECK(name_of(ag::Backend::None)        == name_of(mc::Backend::None));
}

TEST_CASE("sandbox: the two implementations select the SAME backend") {
    // Both probe the live host. Whatever this machine supports, they must
    // agree — otherwise hooks and tools run under different confinement and
    // the status banner describes only one of them.
    (void)ag::init(ag::Mode::Auto);
    // Through the REAL bridge, not mc::init directly: wire_mcp_runtime is
    // what a running agentty calls, and it is where the host sandbox gets
    // installed. Calling mc::init here would test a path production never
    // takes and miss the very wiring this case exists to check.
    agentty::tools::wire_mcp_runtime("auto");

    // agentty installs itself as mcp's HOST SANDBOX, which settles this
    // invariant by construction rather than by coincidence: mcp does not
    // pick an engine at all, it hands every command to ours. Two independent
    // probes agreeing is luck that holds until the two probes drift; one
    // engine cannot disagree with itself.
    //
    // That delegation is also the only way the settings pane can mean
    // anything. mcp-cpp is a standalone library and claybin is OUR submodule,
    // so mcp cannot reach claybin on its own -- without the hook the `shell`
    // tool ran under mcp's built-in bwrap while the pane configured claybin,
    // and one banner described both. Which is exactly the issue #21 shape
    // this file exists to pin.
    if (mc::has_host_sandbox()) {
        // Delegation is in force. Nothing left to compare: the tools run in
        // whatever ag:: selected, whatever that is.
        CHECK(ag::is_active() == mc::is_active());
        return;
    }

    // No host sandbox (a build or platform where we install none): fall back
    // to requiring the two independent probes to have landed in the same
    // place.
    const auto a = name_of(ag::detected_backend());
    const auto b = name_of(mc::detected_backend());
    if (a != b)
        std::printf("MISMATCH: hooks/ACP use %s, tools use %s\n",
                    a.c_str(), b.c_str());
    CHECK(a == b);

    // And they must agree on whether a sandbox is active at all, which is
    // what the startup banner reports.
    CHECK(ag::is_active() == mc::is_active());
}
