// agentty has TWO sandbox implementations, and both must agree.
//
//   agentty::tools::util::sandbox  — lifecycle hooks, external ACP agents
//   mcp::tools::util::sandbox      — shell, diagnostics, git, process_start
//
// They are initialized separately (main.cpp, then wire_mcp_runtime) and each
// probes independently. Porting a backend into one and not the other is the
// failure this pins: the banner would say "bastion" while every hook still
// ran under bwrap — the same shape as issue #21, where "sandbox: active"
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
        case ag::Backend::Bastion:     return "bastion";
    }
    return "?";
}
std::string name_of(mc::Backend b) {
    switch (b) {
        case mc::Backend::None:        return "none";
        case mc::Backend::Bwrap:       return "bwrap";
        case mc::Backend::SandboxExec: return "sandbox-exec";
        case mc::Backend::Bastion:     return "bastion";
    }
    return "?";
}

}  // namespace

TEST_CASE("sandbox: both implementations know the same backends") {
    // A backend added to one side only is the bug. This is a compile-time
    // check in practice — if either enum lacks Bastion, name_of won't build —
    // but assert the mapping too, so a silent renumbering is caught.
    CHECK(name_of(ag::Backend::Bastion) == "bastion");
    CHECK(name_of(mc::Backend::Bastion) == "bastion");
    CHECK(name_of(ag::Backend::Bwrap)   == name_of(mc::Backend::Bwrap));
    CHECK(name_of(ag::Backend::None)    == name_of(mc::Backend::None));
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

    // What actually CONFINES a command, which is not always what the local
    // probe detected. agentty links bastion; mcp-cpp is a standalone library
    // and cannot, so its probe finds only bwrap. The bridge closes that by
    // installing agentty's runner as mcp-cpp's host sandbox — so mcp-cpp
    // then executes under bastion while still REPORTING Backend::Bwrap from
    // its own detection.
    //
    // Comparing detected_backend() alone would therefore fail on a correctly
    // configured system and pass on a broken one. The invariant that matters
    // is the engine a command ends up in, so compare that: the host sandbox
    // when one is installed, else the detected backend.
    const std::string effective_mcp =
        mc::has_host_sandbox() ? std::string{"bastion"} : name_of(mc::detected_backend());
    const auto a = name_of(ag::detected_backend());

    if (a != effective_mcp)
        std::printf("MISMATCH: hooks/ACP use %s, tools use %s\n",
                    a.c_str(), effective_mcp.c_str());
    CHECK(a == effective_mcp);

    // And they must agree on whether a sandbox is active at all, which is
    // what the startup banner reports.
    CHECK(ag::is_active() == mc::is_active());
}
