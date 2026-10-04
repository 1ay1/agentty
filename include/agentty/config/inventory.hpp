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
// ── What this file reports about TRUST ───────────────────────────────────
//
// It reports it, and that became honest only once MCP's hand-rolled gate was
// folded into scope::trust_of. While there were TWO answers — scope's
// content-hash model, and a `bool project_local` threaded through bridge.cpp
// — printing either would have described behaviour that does not exist. That
// is the exact sin this file is built to prevent, so it abstained until the
// two became one.
//
// Trust is a property of a SOURCE, not of a concern, which is why it is a
// column on the read ladder rather than a field up here: `executable`
// decides whether a locus needs vouching, and the per-source answer is then
// scope::trust_of's.

#include "agentty/dirs/dirs.hpp"
#include "agentty/scope/scope.hpp"

#include <cstdint>
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
//
// EXECUTABLE: acting on this content spawns processes. A config that rode in
// on a clone is therefore Pending until the human vouches for its exact
// bytes (scope::trust_of + the content-hash approvals store — the MCPoison
// re-gate). Nothing else in this inventory executes, which is why this is
// the only row that carries the flag.
inline constexpr scope::Layout kMcpLayout{
    .leaf = "mcp.json", .explicit_env = "AGENTTY_MCP_CONFIG"};

// Does acting on this concern's content EXECUTE something?
//
// A function of the concern, not a field on the Layout: scope owns the
// read algebra and must not grow a notion of what its callers do with the
// bytes. Only MCP spawns.
[[nodiscard]] constexpr bool executable(std::string_view name) noexcept {
    return name == "mcp";
}

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

// Does this spec's variable move the WHOLE root, or just this leaf?
//
// Derived from the root, never a field, so a new category cannot
// accidentally advertise itself as an anchor.
//
// Today the honest answer is NO for every spec. $AGENTTY_HOME is the user
// root's anchor, and no Spec declares it — it is resolved one level down, by
// util::user_root(). The PROJECT root has no anchor at all: $AGENTTY_RAG_DIR
// relocates the retrieval indexes and the feedback TSV, and nothing else —
// `memory.jsonl` stays at <project>/.agentty regardless. Reporting it as a
// root anchor would be exactly the kind of confident wrong answer this file
// exists to prevent.
//
// That asymmetry is the remaining gap in the storage model, written up in
// docs/design/dot-agentty.md: the user root has categories to hang an
// override off, and the project root is flat, which is why RAG_DIR had to be
// invented as a one-off instead of falling out of (Root, Lifecycle).
[[nodiscard]] constexpr bool moves_whole_root(const dirs::Spec&) noexcept {
    return false;
}

// The variable that moves this root wholesale, or empty when none does.
[[nodiscard]] constexpr std::string_view anchor_env(dirs::Root r) noexcept {
    return r == dirs::Root::User ? "AGENTTY_HOME" : std::string_view{};
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

    // Empty unless the concern is executable AND the file is present.
    // "trusted" / "needs approval" / "blocked: <why>" — computed by the same
    // scope::trust_of the spawn gate calls, so the word printed here is the
    // word the connect loop acted on.
    std::string    trust;
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

// A FOREIGN config that exists on this machine and is deliberately not read.
//
// Not a dialect: these vary in file name and nesting, not just directory, so
// they cannot be a value on scope's Locus × Dialect product. And reading
// executable config we do not own would let another tool's edit change what
// we spawn, silently. Naming them here — with the one command that adopts
// them — is what turns "agentty ignores my servers" from an issue someone
// files into a line they already read. That is #60.
struct ForeignSource {
    std::string tool;       // "junie", "claude", …
    std::string path;
    std::string adopt_with; // the exact command to run
};

struct WriteRow {
    std::string      path;
    dirs::Origin     origin = dirs::Origin::Default;
    std::string_view env;        // this spec's own variable, if any
    std::string_view anchor;     // the variable that moves its whole root, if any
    bool             env_moves_root = false;
    bool             rebuildable = false;
    unsigned         keep_last = 0;
    std::string      error;      // non-empty when the spec would not resolve

    // Bytes on disk, measured. This is the column #58 was really asking for
    // — "store all non-configuration data on a different path" is a decision
    // nobody can make without knowing which categories are big. Settings are
    // KB and threads are GB, and until now the only way to learn that was
    // `du`.
    //
    // Counted ONLY for a leaf this spec owns. Two specs that share a
    // directory (rag and feedback both resolve to <project>/.agentty) must
    // not each report the whole tree, or the totals read as double.
    std::uintmax_t   bytes = 0;
    bool             measured = false;   // false when the dir does not exist
    bool             shared_dir = false; // size belongs to a sibling spec
};

struct Report {
    std::string_view            name;
    std::string_view            what;
    std::vector<ReadRow>        reads;
    std::vector<SkippedDialect> skipped;
    std::vector<ForeignSource>  foreign;   // present, not read, adoptable
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
