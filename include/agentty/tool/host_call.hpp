#pragma once
// What mcp-cpp's Call::host points at for a call agentty's dispatcher made.
// mcp-cpp hands it back untouched to agentty's own services (Exec, skills,
// subagents). Header-only so the exec layer can read it without the bridge.

namespace agentty::tools {
struct CallContext;
namespace util::handoff { struct TrustedSnapshot; }

struct HostCall {
    const CallContext*                    ctx = nullptr;
    // The workspace snapshot the dispatcher took to watch a shell-like call.
    // The sandbox reuses it instead of walking the tree again. Null: not taken.
    const util::handoff::TrustedSnapshot* trusted = nullptr;
};

// The HostCall behind Call::host, or an empty one for a call that didn't come
// through the dispatcher (an MCP client over mcp-serve).
[[nodiscard]] inline const HostCall& host_call(const void* host) noexcept {
    static constexpr HostCall kNone{};
    return host ? *static_cast<const HostCall*>(host) : kNone;
}

}  // namespace agentty::tools
