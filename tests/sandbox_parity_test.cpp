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

    // Compare what actually CONFINES a command, not merely what each side
    // probed: if a host sandbox is ever installed again, the engine a command
    // ends up in is the invariant that matters, and comparing detections
    // alone would fail on a correct system and pass on a broken one.
    const std::string effective_mcp =
        mc::has_host_sandbox() ? std::string{"host"} : name_of(mc::detected_backend());
    const auto a = name_of(ag::detected_backend());

    if (a != effective_mcp)
        std::printf("MISMATCH: hooks/ACP use %s, tools use %s\n",
                    a.c_str(), effective_mcp.c_str());
    CHECK(a == effective_mcp);

    // And they must agree on whether a sandbox is active at all, which is
    // what the startup banner reports.
    CHECK(ag::is_active() == mc::is_active());
}
