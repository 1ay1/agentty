#pragma once
// agentty::scope — the config-resolution algebra.
//
// One place answers "where does this piece of configuration live, who placed
// it there, and am I allowed to execute what it says." Every config concern
// (memory, skills, agents, commands, MCP plugins, hooks) used to hand-roll
// its own answer with a different, subtly-incompatible shape:
//
//   • memory   Scope{User,Project} → one path; Project gated on writability.
//   • skills   a 6-root ladder, first-name-wins shadow, project ▷ user.
//   • agents   the same ladder, built by hand a second time.
//   • commands the same ladder, built by hand a third time.
//   • MCP      resolve_config(): env ▷ project ▷ user, ONE winning file, and
//              a coarse env-var "trust" gate (AGENTTY_MCP_ALLOW_PROJECT).
//   • hooks    a content-hash approval store (the ONE correct trust model).
//
// Those are five partial functions pretending to be total — `config_path(bool
// project)`, `resolve_config()` returning an empty path on miss, `Scope` enums
// that forget where a value came from. This header replaces them with the
// shapes the rest of the tree already speaks: total functions returning
// `std::expected` (see maya/core/expected.hpp — "Rust has Result<T,E>; we
// have algebraic types composed monadically"), sum types over `enum class :
// uint8_t`, and provenance carried as a value so illegal states — config with
// no source, trust inferred from locus — are unrepresentable by construction.
//
// PURITY CONTRACT (this is a TEA codebase). Resolution is a pure function of
// an explicitly-passed `Env`. Nothing here dips into std::getenv or the cwd:
// the edge resolves those once into an `Env`, and every resolver is then a
// deterministic fold a reducer can exercise in a test with a fabricated Env.
//
// See docs/design/plugin-model.md for the consumer that motivated this, and
// the migration note at the bottom for the order features adopt it.

#include <cstdint>
#include <algorithm>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <expected>

namespace agentty::scope {

namespace fs = std::filesystem;

// ── Locus: WHOSE configuration, as a precedence lattice ───────────────────
// The declaration order IS the resolution order — most authoritative first.
// Precedence lives here, once, so no feature re-hardcodes "env then project
// then user". `rank()` turns the order into a comparable strength.
//
//   Explicit   an env-pointed file ($AGENTTY_*_CONFIG) — the user aimed a
//              command-line/environment arrow at it; most authoritative.
//   Local      project-private and UNCOMMITTED (e.g. .agentty/*.local.*,
//              gitignored) — "my machine, this repo, not shared". Reserved
//              now so the algebra is complete; wired into a resolver only
//              when a real second consumer exists (see migration note).
//   Project    committed, shared with the team (<workspace>/.agentty/…).
//   User       global to this user, across every workspace (~/.agentty/…).
enum class Locus : std::uint8_t { Explicit, Local, Project, User };

// Strength = position in the precedence order. Lower rank wins a tie.
[[nodiscard]] constexpr int rank(Locus l) noexcept {
    return static_cast<int>(l);
}

[[nodiscard]] constexpr std::string_view to_string(Locus l) noexcept {
    switch (l) {
        case Locus::Explicit: return "explicit";
        case Locus::Local:    return "local";
        case Locus::Project:  return "project";
        case Locus::User:     return "user";
    }
    return "user";  // total: unreachable, but no partiality at the boundary
}

[[nodiscard]] constexpr std::optional<Locus> parse_locus(std::string_view s) noexcept {
    if (s == "explicit") return Locus::Explicit;
    if (s == "local")    return Locus::Local;
    if (s == "project")  return Locus::Project;
    if (s == "user")     return Locus::User;
    return std::nullopt;  // the ONE partial boundary — at the string edge
}

// ── Dialect: the directory CONVENTION, orthogonal to locus ────────────────
// A source's tribe. `.agentty` is native; `.agents`/`.claude` are interop
// conventions agentty reads for drop-in compatibility. Declaration order is
// the within-locus shadow tiebreak (native shadows interop). This is a
// SEPARATE axis from Locus precisely so the 6-root ladder is a product
// (Locus × Dialect) generated once, not four hand-written arrays.
enum class Dialect : std::uint8_t { Agentty, Agents, Claude };

[[nodiscard]] constexpr std::string_view dir_name(Dialect d) noexcept {
    switch (d) {
        case Dialect::Agentty: return ".agentty";
        case Dialect::Agents:  return ".agents";
        case Dialect::Claude:  return ".claude";
    }
    return ".agentty";
}

// ── Source: a resolved, tagged origin — the carrier of the fold ───────────
// A value that knows its own provenance. Constructed ONLY by the resolver
// (via plan()), so you cannot fabricate configuration with no source: the
// "wrote the edit to the wrong file" bug class becomes structurally
// impossible, because the file to write back to is `Source::base`, never a
// re-derived config_path(bool).
struct Source {
    Locus    locus{Locus::User};
    Dialect  dialect{Dialect::Agentty};
    fs::path base;              // the concrete <…>/.agentty dir this maps to
    bool     writable = false;  // memory's Project-writability gate, general

    [[nodiscard]] bool operator==(const Source&) const = default;
};

// T tagged with WHERE it came from. Every resolved item carries its Source so
// a picker can badge "[project]" / "[user·claude]" and an editor can target
// the exact file — with no bespoke provenance plumbing per feature.
template <class T>
struct Tagged {
    T      value;
    Source source;
};

// ── Env: the resolved edge, passed in by value (purity contract) ──────────
// The ONLY place ambient state (HOME, cwd/workspace, $AGENTTY_*_CONFIG) is
// read is where an Env is built — at the process edge. Resolvers take it by
// const&, so they stay pure and a test constructs a fabricated Env to drive
// any resolution deterministically.
struct Env {
    fs::path                home;         // ~ (empty ⇒ User/…/interop skipped)
    // The ACTIVE PROJECT dir (util::project_root(): the launch cwd clamped
    // inside the access boundary), NOT the widenable workspace_root(). This is
    // where Project/Local config anchors, so `read` under `--workspace /`
    // still resolves repo-relative and memory doesn't land at /.agentty.
    // Empty or "/" ⇒ no usable Project locus (matches memory's guard).
    fs::path                project_root;
    std::optional<fs::path> explicit_config;  // $AGENTTY_<FEATURE>_CONFIG file

    // True when Project storage is actually writable here (project_root is a
    // real, writable dir — not "/" or a read-only mount). Generalises the
    // dir_path_writable check memory does before OFFERING project scope.
    bool project_writable = false;
};

// ── Layout: what a feature stores, described BY the feature ───────────────
// Scope knows nothing about who its callers are — no enum of features, no
// filenames, no env vars baked in here (that would invert the dependency and
// turn this primitive into a registry of everything downstream). A caller
// hands scope a `Layout`: the leaf it wants under each root, and whether it's
// a single file (override-resolved) or a collection dir (union-resolved).
// That's the whole contract — scope lays out roots and folds; the feature
// owns its identity.
struct Layout {
    // The leaf under each <base>/<dialect-dir>: a filename for a single-file
    // concern ("memory.jsonl", "mcp.json"), or a subdir for a collection
    // ("skills", "agents"). scope only ever joins it onto a base.
    std::string_view leaf;

    // An optional Explicit-locus override: if this env var names an existing
    // file, it becomes the highest-precedence source. Empty ⇒ no escape hatch.
    std::string_view explicit_env = {};
};

// ── Trust: first-class, bound to CONTENT — never inferred from Locus ──────
// The MCPoison lesson (CVE-2025-54136) stated as a type: Cursor pinned trust
// to a server's NAME, so swapping the command under an approved name kept the
// approval — silent persistent RCE. Here trust is a function of (source,
// content-hash, approvals), and approvals are keyed by content hash and
// persisted OUTSIDE any committed file, so a cloned repo cannot approve its
// own executable config.
struct Trusted {};                              // user placed it, or approved
struct Pending { std::string content_sha; };    // needs a deliberate approval
struct Blocked { std::string reason; };          // refused (e.g. bad workspace)
using Trust = std::variant<Trusted, Pending, Blocked>;

// The content-hash approval store (hooks' hooks_approved.json, generalised).
// Lives under the USER root so a repo can never vouch for itself.
struct Approvals {
    [[nodiscard]] bool approved(std::string_view content_sha) const noexcept;
    void               approve(std::string_view content_sha);

    std::vector<std::string> shas;  // sorted; small — a handful of entries
};

// Total: (source, content, approvals) → Trust. Explicit/User are implicitly
// Trusted (the human placed them). Project/Local carrying executable config
// start Pending and become Trusted only when THIS content hash is approved.
[[nodiscard]] Trust trust_of(const Source& src,
                             std::string_view content_sha,
                             const Approvals& approvals) noexcept;

// ── Domain error — the E in every std::expected below ─────────────────────
// agentty gives each subsystem its own error sum (OAuthError, HttpError,
// ToolError, io::fsm::DomainError); scope follows suit rather than borrowing
// maya::Error, so a caller pattern-matches on scope-specific failure kinds.
enum class ErrorKind : std::uint8_t {
    NoSource,       // no source in the plan produced a value (resolve_first)
    ParseFailed,    // a source existed but its bytes didn't parse
    NotWritable,    // an edit targeted a source whose storage isn't writable
    Io,             // filesystem read/write failed
};

struct Error {
    ErrorKind   kind;
    std::string message;
};

template <class T>
using Result = std::expected<T, Error>;

[[nodiscard]] inline std::unexpected<Error> fail(ErrorKind k, std::string msg) {
    return std::unexpected{Error{k, std::move(msg)}};
}

// ── plan(): the ordered source list for a layout, given an Env ────────────
// The single generator. Emits Sources in precedence order (Locus-major,
// Dialect-minor): Explicit? ▷ Project×{Agentty,Agents,Claude} ▷
// User×{Agentty,Agents,Claude}. Local is emitted only for layouts that opt
// in (none today — see note). Empty roots (no HOME, unwritable project) are
// simply not emitted, so downstream folds never special-case them.
[[nodiscard]] std::vector<Source> plan(const Layout& layout, const Env& env);

// ── The fold: two monoids over one source list ────────────────────────────
// The elegant core. Both resolvers walk plan(feature, env) in precedence
// order; they differ only in how they COMBINE what each source yields.
//
// `Load` reads ONE source: Source -> Result<std::optional<T>>. nullopt means
// "this source simply isn't present" (not an error); an Error means "present
// but broken" (e.g. ParseFailed) and short-circuits.

// OVERRIDE monoid — the first present source wins the whole value. The
// answer for single-file concerns: memory, hooks, and MCP-as-one-file.
// Returns NoSource if every source was absent.
template <class T, class Load>
[[nodiscard]] Result<Tagged<T>>
resolve_first(const Layout& layout, const Env& env, Load&& load) {
    for (const Source& src : plan(layout, env)) {
        Result<std::optional<T>> got = load(src);
        if (!got) return std::unexpected{std::move(got).error()};  // present but broken
        if (got->has_value())
            return Tagged<T>{std::move(**got), src};               // first hit wins
    }
    return fail(ErrorKind::NoSource, "no source provided a value");
}

// UNION monoid — merge every present source, first-key-wins shadow, each
// surviving item tagged with the Source it came from. The answer for
// collection concerns: skills, agents, commands (and MCP once it merges
// project+user servers instead of picking one file). `key` projects an item
// to its shadow key (a skill name, a server name); earlier sources shadow
// later ones on a key collision — which is exactly project ▷ user, native ▷
// interop, for free. A single broken source short-circuits (fail loud, don't
// silently drop half a config).
template <class T, class Key, class Load>
[[nodiscard]] Result<std::vector<Tagged<T>>>
resolve_union(const Layout& layout, const Env& env, Key&& key, Load&& load) {
    std::vector<Tagged<T>>   out;
    std::vector<std::string> seen;   // shadow keys already claimed by a winner
    for (const Source& src : plan(layout, env)) {
        Result<std::vector<T>> items = load(src);   // Load : Source -> Result<vector<T>>
        if (!items) return std::unexpected{std::move(items).error()};
        for (T& item : *items) {
            std::string k{key(item)};
            if (std::find(seen.begin(), seen.end(), k) != seen.end())
                continue;                            // shadowed by an earlier source
            seen.push_back(std::move(k));
            out.push_back(Tagged<T>{std::move(item), src});
        }
    }
    return out;
}

// Build the process-edge Env once (reads HOME / project_root / project
// writability). The ONE impure function here; kept tiny and out of the
// resolvers so everything above stays testable by value. The layout's
// explicit_env, if set and naming an existing file, is captured too.
[[nodiscard]] Env current_env(const Layout& layout);

}  // namespace agentty::scope

// ── Migration note ─────────────────────────────────────────────────────────
// Adopt behaviour-preserving, smallest-first, one feature per change:
//   1. memory   — the FIRST consumer (done). path_for(Scope) now builds an
//                 Env (owning its richer getpwuid_r home resolution) and
//                 filters plan(Layout{"memory.jsonl"}, env) to the requested
//                 locus × the native dialect. Behaviour identical; the
//                 project-clamp + writability guard now live in scope. This
//                 proved plan(); resolve_first lands when memory reads (vs.
//                 writes) merge across scopes.
//   2. skills/agents/commands — union. The four hand-written root arrays
//                 collapse into plan()+resolve_union; shadow semantics are
//                 preserved by the first-key-wins rule.
//   3. MCP      — LAST, and the only one that GAINS behaviour: switch to
//                 resolve_union (merge project+user servers), route edits to
//                 item.source.base (fixes the wrong-file bug), and replace the
//                 AGENTTY_MCP_ALLOW_PROJECT env gate with trust_of()+Approvals
//                 (fixes the MCPoison-class exposure). Land it only once (1)
//                 and (2) have shaken out the algebra in production.
//
// Locus::Local ships as a VALUE now (the algebra is complete + future-proof)
// but is wired into no resolver until a concrete second consumer appears —
// avoiding a lattice value that only one feature ever reads.
