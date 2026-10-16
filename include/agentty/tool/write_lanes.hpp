#pragma once
// agentty/tool/write_lanes.hpp — who may write which file right now.
//
// One process-wide table of "files a tool is writing", taken by the ONE tool
// caller, tool::DynamicDispatch::execute_with, for every call whose effects
// include WriteFs. Nothing else calls a tool body (tools::ToolBody can only
// be run with a key the dispatcher makes), so no path can write around it:
// the parent's scheduled batch, every `task` subagent, ACP, an abandoned job
// and an MCP client over `agentty mcp-serve` all pass through here.
//
// What a call claims:
//   * the files it names (path / file_path / ..., and both ends of a move),
//     canonicalised, so `./a.c`, `a.c` and `/ws/a.c` are one lane;
//   * the WHOLE workspace when it writes but names no file (git_commit,
//     git_stash, an MCP tool, a plugin): it can touch anything, so it waits
//     for every writer and every writer waits for it.
// Lanes nest by prefix: a writer of `src/` and a writer of `src/a.c` overlap.
//
// A claim is all-or-nothing (no partial hold), so two calls that each want
// the other's file -- move A->B beside move B->A -- can't deadlock: neither
// holds anything while it waits. The wait is cancellable and never holds a
// lock: a call that is cancelled while queued gives up and fails cleanly.
//
// Reads take no lane. A write lands with write_atomic (temp + rename), so a
// concurrent reader sees the old file or the new one, never a torn one.

#include <string>
#include <vector>

#include <maya/runtime.hpp>
#include <nlohmann/json.hpp>

namespace agentty::tools {
struct CallContext;
}

namespace agentty::tools::write_lanes {

// The lanes a WriteFs call needs: its canonical paths, or {kWholeWorkspace}
// when it names none. Relative paths resolve against the call's project,
// then its workspace, then the process workspace.
inline constexpr const char* kWholeWorkspace = "\x01*whole-workspace*";
[[nodiscard]] std::vector<std::string> lanes_for(std::string_view tool,
                                                 const nlohmann::json& args,
                                                 const CallContext& ctx);

// Whether two lanes collide (identical, one under the other, or either is
// the whole workspace).
[[nodiscard]] bool overlap(std::string_view a, std::string_view b) noexcept;

// Holds a set of lanes for one call; releases them however it ends.
class Hold {
public:
    // Waits until no held lane overlaps `lanes`, then claims them all at
    // once. Gives up (held() == false) if `ctx` is cancelled while waiting.
    Hold(const maya::unlocked& cap, std::vector<std::string> lanes, const CallContext& ctx);
    ~Hold();
    Hold(const Hold&)            = delete;
    Hold& operator=(const Hold&) = delete;

    [[nodiscard]] bool held() const noexcept { return lanes_.empty() || held_; }

private:
    const maya::unlocked&    cap_;
    std::vector<std::string> lanes_;
    bool                     held_ = false;
};

// Test hook: how many lanes are held right now.
[[nodiscard]] std::size_t held_count(const maya::unlocked& cap);

}  // namespace agentty::tools::write_lanes
