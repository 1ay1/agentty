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
    (void)mc::init(mc::Mode::Auto);

    const auto a = name_of(ag::detected_backend());
    const auto m = name_of(mc::detected_backend());
    if (a != m)
        std::printf("MISMATCH: hooks/ACP use %s, tools use %s\n",
                    a.c_str(), m.c_str());
    CHECK(a == m);

    // And they must agree on whether a sandbox is active at all, which is
    // what the startup banner reports.
    CHECK(ag::is_active() == mc::is_active());
}
