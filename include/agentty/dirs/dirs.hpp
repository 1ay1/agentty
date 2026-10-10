#pragma once
// agentty::dirs — WHERE agentty's bytes live, and HOW LONG they live there.
//
// The write-side peer of agentty::scope.
//
// scope made config LOOKUP total: one algebra (Locus × Dialect), provenance
// on every hit, no caller re-deriving precedence. It answers "where do I
// READ this from?".
//
// Nothing answered "where do I WRITE this to?", and that half grew the way
// config did before scope: one decision at a time, at each call site. The
// result, measured:
//
//   - memory clamps its project path to project_root()
//   - rag uses raw fs::current_path() at FIVE separate sites
//   - skills deliberately passes "." and carries a NOTE explaining why it
//     disagrees with memory
//
// A comment explaining why one consumer disagrees with another is the shape
// of a missing primitive.
//
// And nothing had a LIFECYCLE at all. user_root's resolve_subdir() answers
// `where`; no function answered `for how long`. That is why a 47 MB .ragdb
// from a superseded naming scheme can sit on disk forever (#62) — the leak
// is not an oversight in the rag code, it is that nothing ever asked.
//
// ── The contract, and why it is shaped like scope's ──────────────────────
//
// A caller hands over a Spec: which root, what leaf, which override var,
// how long it lives. dirs knows NOTHING about who its callers are — no enum
// of features, no filenames baked in here. That would invert the dependency
// and turn this primitive into a registry of everything downstream, which
// is precisely the property that makes scope::Layout work.
//
//     agentty::scope  :  Layout  →  read roots, by precedence
//     agentty::dirs   :  Spec    →  one write root, with a lifecycle
//
// Total, not partial: every resolution yields a path AND its provenance, or
// a typed error. Never an empty path that each caller re-interprets — that
// pattern is what scope-model.md calls "partial functions pretending to be
// total", and it is how the MCP "toggled the wrong file" bug happened.

#include <chrono>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace agentty::dirs {

namespace fs = std::filesystem;

// ── Root: whose tree, and therefore what it follows ──────────────────────
// Two values, not five. The long note in util/user_root.hpp argues why
// agentty has ONE user root rather than XDG's four; the same reasoning caps
// this enum. A root exists here only when it answers a genuinely different
// question about WHAT THE DATA FOLLOWS.
enum class Root {
    // ~/.agentty — follows the HUMAN. Settings, credentials, threads,
    // learned memory, logs: things that should be identical regardless of
    // which checkout you are standing in.
    User,

    // <anchor>/.agentty — follows the CODE. Retrieval indexes, feedback:
    // things that are meaningless away from the tree that produced them and
    // must not be shared between unrelated checkouts.
    Project,
};

// ── Lifecycle: declared WITH the path, on purpose ────────────────────────
// This field is the structural fix for #62. Putting retention in the Spec
// means ADDING A STORE FORCES THE DECISION — you cannot introduce a new
// directory without saying, at the declaration site, whether anything ever
// reclaims it. A default of "forever" is then a choice someone made rather
// than a question nobody asked.
struct Lifecycle {
    // Can this be deleted without losing anything the user cannot get back?
    //
    // This is the axis that separates us from blobs::gc, and it matters
    // more than it looks. blobs::gc is CAUTIOUS because being wrong
    // destroys conversation history that no amount of recomputation
    // restores. A .ragdb is rebuildable, so being wrong costs one
    // re-index. Same mark-and-sweep mechanism, opposite risk posture — and
    // a shared GC gets that wrong unless the distinction is a field rather
    // than a convention.
    bool rebuildable = false;

    // Keep the N newest matching variants, sweep older ones. 0 ⇒ keep all.
    //
    // Exists because of a real tension in the rag naming scheme: the
    // embedder-identity tag in the filename is CORRECT (it stops us serving
    // vectors from an incompatible space) but it makes the filename set
    // unbounded. keep_last=1 means A/B-ing two embedders does not force a
    // full rebuild every time you switch back, while still bounding growth.
    unsigned keep_last = 0;

    // Grace window before anything is eligible. Mirrors blobs::gc's reason
    // for having one: another agentty process may be mid-save, and a
    // concurrent process may legitimately be running a different config.
    std::chrono::seconds min_age{0};

    [[nodiscard]] constexpr bool sweeps() const noexcept {
        return rebuildable && keep_last > 0;
    }
};

// ── Spec: what a feature declares about its own storage ──────────────────
struct Spec {
    Root root = Root::User;

    // The leaf under the resolved root. A subdirectory name ("threads",
    // "rag"); dirs only ever joins it on.
    //
    // EMPTY means "the root itself", for a caller that owns its own
    // filenames and just wants the directory (rag appends
    // rag_docs.<embedder-tag>.ragdb). Resolution special-cases it so the
    // result has no trailing separator — `root / ""` names the same
    // directory but compares UNEQUAL to `root`, which silently breaks any
    // "did this path move?" comparison.
    std::string_view leaf;

    // Override variable, AGENTTY_<NAME>_DIR. Every directory has one.
    std::string_view env = {};

    // Force 0700 on the resolved directory. The default is inherited from
    // the root, which is enough for the default path and NOT enough for an
    // override that may point at a 0755 directory the user already made.
    bool owner_only = false;

    Lifecycle life{};

    // One line on how long things live here, for `agentty config`. Only
    // used when nothing sweeps the dir, since then the Lifecycle can't say.
    std::string_view retention = {};
};

// ── Origin: provenance, for the same reason scope tracks Source ──────────
// A user who set a variable and did not get it deserves to know, and a
// Storage pane cannot honestly report "where are my bytes" from a bare
// path. Keeping this on the result is what makes the fallback case
// debuggable instead of mysterious.
enum class Origin {
    Default,            // <root>/<leaf>
    Override,           // $env named it, and it worked
    OverrideFellBack,   // $env named it, it was unusable, we warned + defaulted
};

struct Resolved {
    fs::path path;
    Origin   origin = Origin::Default;

    // The root `path` sits under, before `leaf` was joined. Carried so a
    // caller can resolve siblings without re-deriving the root.
    fs::path root;

    [[nodiscard]] bool overridden() const noexcept {
        return origin != Origin::Default;
    }
};

// ── Error: typed, because "empty path" is not an error report ────────────
struct Error {
    enum class Kind {
        NoRoot,          // user root unresolvable (no HOME) or no project anchor
        NotWritable,     // the anchor exists but we cannot write under it
        Unusable,        // both the override AND the default failed
    };
    Kind        kind = Kind::NoRoot;
    std::string detail;
};

[[nodiscard]] std::string_view to_string(Error::Kind) noexcept;

// ── Resolution ───────────────────────────────────────────────────────────

// Resolve (and create) the directory for `spec`.
//
// Override semantics are inherited verbatim from user_root's
// resolve_subdir(), because they were already right:
//
//   - $env wins when set and NON-EMPTY. An exported-but-blank variable is a
//     common shell accident and relocates nothing.
//   - A RELATIVE override resolves against the root, never the process CWD.
//     `AGENTTY_LOGS_DIR=logs2` has to mean ONE directory, not a different
//     one for every directory agentty is launched from.
//   - A failed override WARNS ONCE and falls back. Silently papering over
//     it with the default tells the user the wrong thing twice: the setting
//     did not take, and nothing said so.
[[nodiscard]] std::expected<Resolved, Error> resolve(const Spec& spec);

// Resolve without creating anything, and without warning. For probes and
// for UI that wants to report a location it is not about to write to.
[[nodiscard]] std::expected<Resolved, Error> resolve_dry(const Spec& spec);

// ── The project storage anchor ───────────────────────────────────────────
//
// NOT project_root(), and the difference is load-bearing.
//
// project_root() is documented as "the process cwd, clamped inside the
// access boundary". It does not walk up. That is RIGHT for its job —
// relative tool paths must track where the user is standing, so
// `read src/foo.cpp` lands in the launched project and not at /src/foo.cpp.
//
// It is wrong for storage, because it moves. Launch from a subdirectory and
// you get a SECOND index: `cd src/ && agentty` builds its own 82 MB
// <cwd>/.agentty because current_path() moved and nothing anchored it.
//
// So storage asks a different question and gets its own answer: the nearest
// enclosing directory holding a project marker, falling back to
// project_root() when there is none. Both are then subject to scope's
// usable_project_root() guard — "/" is agentty's unrestricted-access
// sentinel and never a place to scatter state.
//
// Deliberately leaving project_root() alone. Unifying them would reintroduce
// one bug or the other.
[[nodiscard]] fs::path project_anchor();

// Markers searched for, nearest-first. Order is not significance: the walk
// stops at the first directory containing ANY of them.
[[nodiscard]] std::span<const std::string_view> project_markers() noexcept;

// ── Sweep: the lifecycle, enforced ───────────────────────────────────────
//
// Reclaims superseded VARIANTS of a file whose name carries an identity
// tag. The motivating case is rag's `rag_code.<embedder>.ragdb`: the tag is
// correct (it stops us serving vectors from an incompatible space) but it
// makes the filename set unbounded, and nothing collected the losers — a
// 47 MB orphan from a retired naming scheme was still on disk (#62).
//
// Modelled on blobs::gc deliberately, including dry_run and min_age, with
// ONE axis deliberately different: Lifecycle::rebuildable. blobs::gc is
// cautious because deleting a blob destroys conversation history that no
// recomputation restores. A .ragdb costs one re-index. That asymmetry is
// why `sweeps()` demands rebuildable AND keep_last — a store that is not
// rebuildable is NEVER swept, no matter what keep_last says.
struct SweepStats {
    std::size_t examined = 0;
    std::size_t kept     = 0;
    std::size_t deleted  = 0;
    std::size_t too_new  = 0;   // held back by min_age
    std::size_t failed   = 0;   // >0 ⇒ something was unreadable; see below
    std::uintmax_t bytes_freed = 0;
};

// What to sweep, within the directory a Spec resolves to.
//
// `stem` + `suffix` bracket the variant tag: stem="rag_code.",
// suffix=".ragdb" matches `rag_code.<anything>.ragdb`. `keep` names the
// variants that must survive regardless of age or count — the live ones.
//
// Sidecars are swept with their principal: deleting `x.ragdb` and leaving
// `x.ragdb.meta.json` is how you get a meta that describes a file that is
// not there, which every loader then has to defend against.
struct SweepRequest {
    Spec             spec;
    std::string_view stem;
    std::string_view suffix;
    std::span<const std::string>  keep;      // live filenames, never deleted
    std::span<const std::string_view> sidecar_suffixes;  // e.g. ".meta.json"

    // Also delete an UNTAGGED `<stem-without-dot><suffix>` (e.g. a legacy
    // `rag_code.ragdb` with no embedder tag). Off by default because "a
    // name I do not recognise" is normally a reason to leave a file alone,
    // not to delete it.
    bool include_untagged = false;
};

// Sweep. A no-op returning {} unless spec.life.sweeps().
//
// SAFETY: if any directory entry cannot be examined, nothing is deleted and
// `failed` is non-zero. Same rule as blobs::gc — being wrong about what is
// present must cost disk, never data.
[[nodiscard]] SweepStats sweep(const SweepRequest& req, bool dry_run = false);

}  // namespace agentty::dirs
