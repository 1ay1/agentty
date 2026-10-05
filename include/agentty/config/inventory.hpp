#pragma once
// agentty::config — every place agentty reads config from or writes bytes to,
// declared once.
//
// The model: two roots, one anchor each.
//
//     ~/.agentty           follows the HUMAN   $AGENTTY_HOME
//     <project>/.agentty   follows the CODE    $AGENTTY_PROJECT_DIR
//
// Everything else is a leaf under one of those, so the anchor moves it.
//
// Each constant below is THE definition for its feature. The feature reads
// it and `agentty config` prints it, so the printed answer cannot drift from
// the real one. config_inventory_test fails the build if any other file
// declares its own Layout or Spec.

#include "agentty/dirs/dirs.hpp"
#include "agentty/scope/scope.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace agentty::config {

// ── Read: what each feature looks for, and where ─────────────────────────
//
// Layout::dialects is the per-feature choice: kPortable where the same bytes
// are valid in another tool, kNativeOnly (the default) for a format we own.

// mcp.json is not an .agents/.claude convention and the schema is ours.
inline constexpr scope::Layout kMcpLayout{
    .leaf = "mcp.json", .explicit_env = "AGENTTY_MCP_CONFIG"};

// Markdown with frontmatter — valid in other tools, so reading theirs is
// free compatibility.
inline constexpr scope::Layout kSkillsLayout{
    .leaf = "skills", .dialects = scope::kPortable};
inline constexpr scope::Layout kAgentsLayout{
    .leaf = "agents", .dialects = scope::kPortable};
inline constexpr scope::Layout kCommandsLayout{
    .leaf = "commands", .dialects = scope::kPortable};

inline constexpr scope::Layout kMemoryLayout{.leaf = "memory.jsonl"};

// Does acting on this content spawn a process? Only MCP does, and that is
// what makes a cloned repo's config Pending until a human vouches for its
// exact bytes. A function rather than a Layout field: scope owns the read
// algebra and must not learn what callers do with the bytes.
[[nodiscard]] constexpr bool executable(std::string_view name) noexcept {
    return name == "mcp";
}

// ── Write: where bytes land, and for how long ────────────────────────────

inline constexpr dirs::Spec kThreadsSpec{
    .root = dirs::Root::User, .leaf = "threads",
    .env = "AGENTTY_THREADS_DIR", .owner_only = true};
inline constexpr dirs::Spec kCacheSpec{
    .root = dirs::Root::User, .leaf = "cache", .env = "AGENTTY_CACHE_DIR"};
inline constexpr dirs::Spec kLogsSpec{
    .root = dirs::Root::User, .leaf = "logs", .env = "AGENTTY_LOGS_DIR"};

// Derived: a sweep may reclaim it. keep_last=1 spares the previous
// embedder's index so switching backends doesn't force a full rebuild.
inline constexpr dirs::Spec kRagSpec{
    .root = dirs::Root::Project, .leaf = "cache",
    .life = {.rebuildable = true, .keep_last = 1,
             .min_age = std::chrono::seconds{3600}}};

// Accumulated from use, nothing regenerates it, so never swept. A separate
// leaf from the indexes because the retention differs.
inline constexpr dirs::Spec kFeedbackSpec{
    .root = dirs::Root::Project, .leaf = "state",
    .life = {}};

// Both roots resolve their anchor in dirs::root_for, so no Spec declares
// one — which keeps a new category from advertising itself as an anchor.
[[nodiscard]] constexpr bool moves_whole_root(const dirs::Spec&) noexcept {
    return false;
}
[[nodiscard]] constexpr std::string_view anchor_env(dirs::Root r) noexcept {
    return r == dirs::Root::User ? "AGENTTY_HOME" : "AGENTTY_PROJECT_DIR";
}

// ── Approval stores ──────────────────────────────────────────────────────
//
// Content hashes a human vouched for. Under the user root in state/, so a
// cloned repo can never vouch for itself. Named here because
// "mcp_approvals.json" used to be spelled in four files, and a store whose
// readers and writer disagree silently trusts nothing.
inline constexpr std::string_view kSkillsApprovals = "skills_approved.json";
inline constexpr std::string_view kHooksApprovals  = "hooks_approved.json";
inline constexpr std::string_view kMcpApprovals    = "mcp_approvals.json";

// ── One concern, both halves ─────────────────────────────────────────────
//
// Pointers, not values: a copy would be a second definition.
struct Entry {
    std::string_view     name;    // what you type: `agentty config mcp`
    std::string_view     what;    // one line, for the table
    const scope::Layout* read  = nullptr;   // null ⇒ not read as config
    const dirs::Spec*    write = nullptr;   // null ⇒ writes nothing
};

[[nodiscard]] std::span<const Entry> inventory() noexcept;
[[nodiscard]] const Entry* find(std::string_view name) noexcept;

// ── The resolved view ────────────────────────────────────────────────────
//
// Folded from the same functions the resolvers fold — scope::plan for the
// ladder, dirs::resolve_dry for the write path. Nothing here re-derives a
// location.

struct ReadRow {
    scope::Locus   locus{};
    scope::Dialect dialect{};
    std::string    path;
    bool           exists = false;
    // Set only for an executable concern with a present file. Computed by
    // the same trust_of the spawn gate calls.
    std::string    trust;
};

// A dialect this feature does not read. Derived as the set difference
// against the layout's own `dialects`, so it cannot disagree with what IS
// read.
struct SkippedDialect {
    scope::Dialect dialect{};
    std::string    dir;          // ".claude", ".agents"
};

// Another tool's config, found on this machine and deliberately not read:
// these vary in file name and nesting, so they are not a dialect, and
// reading executable config we don't own would let someone else's edit
// change what we spawn. Printed with the command that adopts it.
struct ForeignSource {
    std::string tool;       // "junie", "claude", …
    std::string path;
    std::string adopt_with;
};

struct WriteRow {
    std::string      path;
    dirs::Origin     origin = dirs::Origin::Default;
    std::string_view env;        // this spec's own variable, if any
    std::string_view anchor;     // the variable that moves its whole root
    bool             env_moves_root = false;
    bool             rebuildable = false;
    unsigned         keep_last = 0;
    std::string      error;      // non-empty when the spec would not resolve

    // Measured. Counted only for a leaf this spec owns, so two specs sharing
    // a directory don't double the total.
    std::uintmax_t   bytes = 0;
    bool             measured = false;   // false when the dir does not exist
    bool             shared_dir = false; // counted by a sibling spec
    bool             partial = false;    // some of it was unreadable
};

struct Report {
    std::string_view            name;
    std::string_view            what;
    std::vector<ReadRow>        reads;
    std::vector<SkippedDialect> skipped;
    std::vector<ForeignSource>  foreign;
    std::string_view            explicit_env;
    std::optional<WriteRow>     write;
};

[[nodiscard]] Report describe(const Entry&);

//   agentty config          every concern, one line each
//   agentty config mcp      one ladder, including what is NOT read
//   agentty config env      the model
int cmd_config(std::span<const std::string> argv);

}  // namespace agentty::config
