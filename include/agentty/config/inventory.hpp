#pragma once
// agentty::config — the inventory: every place agentty reads config from or
// writes bytes to, declared once, in one list.
//
// ── The model, in four lines ─────────────────────────────────────────────
//
//     TWO ROOTS.          ~/.agentty          follows the HUMAN
//                         <project>/.agentty  follows the CODE
//
//     ONE ANCHOR EACH.    $AGENTTY_HOME       moves the user root
//                         $AGENTTY_RAG_DIR    moves the project root's bulk
//
// Everything agentty stores is a leaf under one of those two roots. That is
// the whole storage model, and the goal is to keep it sayable in one breath.
//
// ── Why this file exists ─────────────────────────────────────────────────
//
// agentty has two storage primitives and they are both good:
//
//     scope  (scope/scope.hpp)   where do I READ config from   — precedence
//     dirs   (dirs/dirs.hpp)     where do I WRITE bytes to      — lifecycle
//
// What it did not have is a way for a USER to see either one. Six read
// locations per portable feature, two write roots, seven environment
// variables — and no command that answers "where does my MCP config come
// from" or "why is agentty ignoring the file I just edited".
//
// That gap is what issues #58 and #60 are really about. Both are the same
// person asking the same thing twice: FEWER THINGS TO TRACK. Six locations
// are fine when you can see them; six invisible ones are the problem.
//
// ── The one rule that makes this trustworthy ─────────────────────────────
//
// A tool that describes the program is worthless if it can drift from the
// program. A WRONG explanation is worse than none, because now you believe
// it.
//
// So this file does not DESCRIBE anything. It OWNS the declarations, and the
// features consume them:
//
//     config::kMcpLayout  ──┬──>  bridge.cpp reads servers with it
//                           └──>  `agentty config mcp` prints it
//
// One constant, two readers. There is no second copy to disagree with the
// first, so the printed ladder cannot be a lie about the real one.
//
// Same lesson as the dialect fix: that bug was two decision sites for one
// decision — plan() said six sources, a filter downstream read three. The
// fix was not to document the filter, it was to delete the second site.
//
// ── On environment variables: the model needs THREE ──────────────────────
//
// Seven exist. Only three carry the model:
//
//     $AGENTTY_HOME        the user root            — anchor
//     $AGENTTY_RAG_DIR     the project root's bulk  — anchor
//     $AGENTTY_MCP_CONFIG  "read exactly this file" — a different axis
//                                                     (scope's Explicit locus)
//
// The other three — $AGENTTY_THREADS_DIR, $AGENTTY_CACHE_DIR,
// $AGENTTY_LOGS_DIR — each move ONE leaf of the user root that
// $AGENTTY_HOME already moves wholesale. They are narrower conveniences,
// not part of the model, and `agentty config env` says so rather than
// presenting seven equals.
//
// They stay because they shipped in 0.9.19 and removing a released variable
// breaks a working setup with no error message. But NOTHING NEW gets added
// on that axis: a new storage category is a leaf under an existing root, so
// the anchor already relocates it. The counter-pressure is real and worth
// naming — every category looks like it deserves its own variable, and
// user_root.hpp's rule is the answer: a category gets one only if it grows
// unboundedly AND you would plausibly put it on a different device from its
// siblings. Threads/cache/logs failed that test in hindsight; `cache/`
// under the project root will fail it too.
//
// ── What this deliberately does NOT do yet ───────────────────────────────
//
// It does not report TRUST. scope has the right model (trust_of + content
// hashes, the MCPoison fix) but MCP still hand-rolls its own gate in
// bridge.cpp, so there are currently two answers and printing either one
// would describe behaviour that does not exist. That is the exact sin this
// file is built to prevent, so it abstains until the two are one.

#include "agentty/dirs/dirs.hpp"
#include "agentty/scope/scope.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace agentty::config {

// ── Read-side declarations ───────────────────────────────────────────────
//
// Each constant is THE definition for its feature. The feature reads it;
// this file prints it. Nothing constructs a second one.
//
// Dialect breadth is the per-feature choice on Layout::dialects: kPortable
// where the same bytes are valid in another tool, kNativeOnly (the default)
// for a format we own.

// MCP servers. Native only: `mcp.json` is not an .agents/.claude convention
// — those tools do not put that file there — and the schema is ours.
inline constexpr scope::Layout kMcpLayout{
    .leaf = "mcp.json", .explicit_env = "AGENTTY_MCP_CONFIG"};

// Skills, agents, commands. Portable: the same markdown-with-frontmatter
// file is valid in another tool, so reading theirs is free compatibility.
inline constexpr scope::Layout kSkillsLayout{
    .leaf = "skills", .dialects = scope::kPortable};
inline constexpr scope::Layout kAgentsLayout{
    .leaf = "agents", .dialects = scope::kPortable};
inline constexpr scope::Layout kCommandsLayout{
    .leaf = "commands", .dialects = scope::kPortable};

// Learned memory. Our own JSONL; nobody else writes it.
inline constexpr scope::Layout kMemoryLayout{.leaf = "memory.jsonl"};

// ── Write-side declarations ──────────────────────────────────────────────
//
// Three leaves under the user root, one under the project root. Each mirrors
// what util::resolve_subdir() has always done — same variable, same leaf,
// same owner-only bit — and config_inventory_test asserts each resolves to
// the byte-identical path its existing accessor returns.

inline constexpr dirs::Spec kThreadsSpec{
    .root = dirs::Root::User, .leaf = "threads",
    .env = "AGENTTY_THREADS_DIR", .owner_only = true};
inline constexpr dirs::Spec kCacheSpec{
    .root = dirs::Root::User, .leaf = "cache", .env = "AGENTTY_CACHE_DIR"};
inline constexpr dirs::Spec kLogsSpec{
    .root = dirs::Root::User, .leaf = "logs", .env = "AGENTTY_LOGS_DIR"};

// Retrieval indexes. Root::Project — this storage follows the CODE — and
// rebuildable, which is what lets a future sweep be aggressive here where
// blobs::gc has to be careful.
inline constexpr dirs::Spec kRagSpec{
    .root = dirs::Root::Project, .leaf = "", .env = "AGENTTY_RAG_DIR",
    .life = {.rebuildable = true, .keep_last = 1,
             .min_age = std::chrono::seconds{3600}}};

// Retrieval feedback: the learning loop's tallies. Same root and variable as
// the indexes — it is the same kind of derived, project-scoped data — but
// NOT rebuildable: it accumulates from real usage and nothing regenerates
// it, so no sweep may ever collect it.
//
// Worth a row of its own precisely BECAUSE it shares a path with kRagSpec.
// "Same directory, opposite retention" is the kind of fact that silently
// becomes wrong, and `agentty config feedback` saying "kept indefinitely"
// next to rag's "rebuildable" is what keeps a future sweep honest.
inline constexpr dirs::Spec kFeedbackSpec{
    .root = dirs::Root::Project, .leaf = "", .env = "AGENTTY_RAG_DIR",
    .life = {}};   // never swept

// Is this spec's variable the ANCHOR for its root, or a narrower convenience?
//
// Derived, never a field. $AGENTTY_HOME moves the whole user root, so any
// User-root leaf variable is narrower than an anchor that already covers it.
// The project root has no anchor of its own, so its variable IS the anchor.
//
// Deriving it means the honest answer survives adding a category: a new
// User-root spec is automatically reported as narrower, and nobody has to
// remember to tag it.
[[nodiscard]] constexpr bool is_anchor(const dirs::Spec& s) noexcept {
    return s.root == dirs::Root::Project;
}

// The anchor for a root, as a user would type it.
[[nodiscard]] constexpr std::string_view anchor_env(dirs::Root r) noexcept {
    return r == dirs::Root::User ? "AGENTTY_HOME" : "AGENTTY_RAG_DIR";
}

// ── Entry: one concern, both halves ──────────────────────────────────────
//
// Pointers rather than values so this stays an aggregate pointing AT the
// constants above — not copies of them. A copy would be a second
// definition, which is the whole thing this file exists to avoid.
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
// Computed by folding the SAME functions the resolvers fold: scope::plan for
// the read ladder, dirs::resolve_dry for the write path. Nothing here
// re-derives a location.
struct ReadRow {
    scope::Locus   locus{};
    scope::Dialect dialect{};
    std::string    path;
    bool           exists = false;
};

// A dialect this feature does NOT read.
//
// Derived, not hardcoded: the set difference between the full dialect
// product and the layout's declared `dialects`. So the "not read" lines come
// from the same field that decides what IS read, and the two cannot
// disagree. Widening a feature removes its line here for free.
struct SkippedDialect {
    scope::Dialect dialect{};
    std::string    dir;          // ".claude", ".agents"
};

struct WriteRow {
    std::string      path;
    dirs::Origin     origin = dirs::Origin::Default;
    std::string_view env;        // this spec's own variable, if any
    std::string_view anchor;     // the variable that moves its whole root
    bool             env_is_anchor = false;
    bool             rebuildable = false;
    unsigned         keep_last = 0;
    std::string      error;      // non-empty when the spec would not resolve
};

struct Report {
    std::string_view            name;
    std::string_view            what;
    std::vector<ReadRow>        reads;
    std::vector<SkippedDialect> skipped;
    std::string_view            explicit_env;
    std::optional<WriteRow>     write;
};

[[nodiscard]] Report describe(const Entry&);

// CLI: `agentty config [concern|env]`.
//
//   agentty config          every concern, one line each
//   agentty config mcp      the full ladder, including what is NOT read
//   agentty config env      the three variables that carry the model
//
// Returns 0 on success, 1 on an unknown name.
int cmd_config(std::span<const std::string> argv);

}  // namespace agentty::config
