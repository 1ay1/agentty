#pragma once
// Shared filesystem helpers. Tool implementations need normalized paths,
// binary detection, and a predictable "which directories to skip during
// traversal" list — centralised here so the rules are consistent across
// grep / glob / list_dir / find_definition.

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "agentty/tool/registry.hpp"   // ToolError + factories

namespace agentty::tools::util {

namespace fs = std::filesystem;

// Forward declarations — WorkspacePath sits below ToolError-using factories.
class WorkspacePath;

// Read an entire file as a binary blob. Returns "" on open failure (callers
// that need to distinguish missing vs empty should stat first).
//
// Two overloads: the `WorkspacePath` form is the workspace-checked entry
// the model-facing tools should use; the `fs::path` form is the
// unchecked escape hatch for paths the runtime owns (credentials,
// thread persistence, memory store under ~/.agentty/). Routing through
// a typed parameter keeps the boundary visible at the call site —
// reviewers can see at a glance which form a tool picked.
[[nodiscard]] std::string read_file(const fs::path& p);
[[nodiscard]] std::string read_file(const WorkspacePath& p);

// Write content atomically-ish (truncate + write + flush). Returns the
// empty string on success, or a human-readable error otherwise. Keeps tool
// lambdas terse while still forcing callers to surface failures.
//
// See `read_file` above for the WorkspacePath vs fs::path overload split.
[[nodiscard]] std::string write_file(const fs::path& p, std::string_view content);
[[nodiscard]] std::string write_file(const WorkspacePath& p, std::string_view content);

// Normalise a user-supplied path. Accepts forward slashes on Windows
// (the model frequently produces them), strips surrounding whitespace and
// quotes, and returns an absolute path relative to cwd when not already
// absolute — so error messages name an unambiguous location.
[[nodiscard]] fs::path normalize_path(std::string_view s);

// Strong typedef for an already-normalised filesystem path. The only way
// to construct one is from a raw string via `NormalizedPath{"..."}`, which
// calls `normalize_path` — so "did I already normalize this?" is answered
// by the type. Passed by value (cheap: holds a single fs::path).
struct NormalizedPath {
    fs::path value;

    explicit NormalizedPath(std::string_view raw) : value(normalize_path(raw)) {}

    [[nodiscard]] const fs::path& path() const noexcept { return value; }
    [[nodiscard]] std::string string() const { return value.string(); }
    [[nodiscard]] bool empty() const noexcept { return value.empty(); }
};

// ── Workspace boundary ──────────────────────────────────────────────────
// Every filesystem-touching tool refuses paths outside this root. Set
// once at startup (main.cpp from cwd, or from the --workspace CLI flag);
// query freely from tool implementations. The default before
// `set_workspace_root` is called is the process's cwd at first call —
// safe for tests and standalone helper use.
//
// The boundary is the simplest sandbox layer: it doesn't stop a model
// from running shell commands that walk anywhere, but it does stop the
// fast path of "model casually `read`s ~/.ssh/id_rsa or `write`s to
// /etc/hosts". Pair with bash gating + a future OS-native sandbox
// (sandbox-exec / bwrap / firejail) for a defense-in-depth story.
void set_workspace_root(fs::path root);

[[nodiscard]] const fs::path& workspace_root();

// True if `target` is at-or-under the workspace root after canonicalising
// both sides. Symlink escape is blocked: a link inside the workspace that
// points to /etc would resolve to /etc and fail the prefix check. Uses
// weakly_canonical so a not-yet-existing path (e.g. write target) is
// still checked correctly against its existing parent components.
[[nodiscard]] bool is_within_workspace(const fs::path& target);

// Construct a NormalizedPath that's been workspace-checked in one shot.
// Tools call:
//     auto p = util::make_workspace_path(*raw, "read");
//     if (!p) return std::unexpected(p.error());
// `tool_name` only appears in the error message and is purely cosmetic.
[[nodiscard]] std::expected<struct NormalizedPath, ToolError>
make_workspace_path(std::string_view raw, std::string_view tool_name);

// ── WorkspacePath ───────────────────────────────────────────
// A NormalizedPath that carries a *type-level* proof of workspace
// containment. The only public way to obtain one is through
// `WorkspacePath::checked` (or the gated builder factories below),
// which delegate to is_within_workspace(). Once a function accepts
// `const WorkspacePath&`, reviewers know the containment check has
// already happened — forgetting it is a compile error, not a missing
// runtime gate.
//
// Today's tools already route every fs path through make_workspace_path
// before any IO; this type makes that discipline a property of the type
// system instead of a code-review convention. New fs APIs should accept
// `WorkspacePath`; only the runtime's own non-workspace paths (under
// ~/.agentty/ for threads/memory/credentials) bypass it via the
// `fs::path` overloads of read_file/write_file.
class WorkspacePath {
    NormalizedPath inner_;
    // Private; only the factory friends can mint one. No public ctor =
    // no way to skip the containment check.
    explicit WorkspacePath(NormalizedPath n) noexcept : inner_(std::move(n)) {}

    friend std::expected<WorkspacePath, ToolError>
        make_workspace_path_checked(std::string_view raw,
                                    std::string_view tool_name);
    friend std::expected<WorkspacePath, ToolError>
        promote_to_workspace_path(NormalizedPath p,
                                  std::string_view tool_name);
    friend std::expected<WorkspacePath, ToolError>
        make_readable_path_checked(std::string_view raw,
                                   std::string_view tool_name);

public:
    [[nodiscard]] const fs::path&   path()   const noexcept { return inner_.path(); }
    [[nodiscard]] std::string       string() const          { return inner_.string(); }
    [[nodiscard]] bool              empty()  const noexcept { return inner_.empty(); }
    [[nodiscard]] const NormalizedPath& normalized() const noexcept { return inner_; }
};

// Workspace-checked factory. Same contract as make_workspace_path but
// yields a WorkspacePath instead of a NormalizedPath — use this in new
// code so the gate's success travels with the value.
[[nodiscard]] std::expected<WorkspacePath, ToolError>
make_workspace_path_checked(std::string_view raw, std::string_view tool_name);

// ── Read-only allowlist roots ───────────────────────────────────────────
// Skill directories (agentskills.io tier-3 resources) may live OUTSIDE
// the workspace (~/.agentty/skills/, ~/.agents/skills/). The spec's
// client-implementation guidance: allowlist skill directories for reads
// so the model can fetch bundled scripts/references without the
// boundary refusing them. The allowlist is READ-ONLY by construction —
// only `make_readable_path_checked` consults it; the write/edit gates
// (make_workspace_path_checked) never do, so an allowlisted root can't
// become a write escape.
//
// Registered by the skills scanner at discovery time. Idempotent;
// bounded by the skill cap (≤ 64×2 roots per scope).
void allow_read_root(const fs::path& root);

// True when `target` sits under a registered read-allowlist root
// (post-canonicalisation, symlink-escape checked like the workspace).
[[nodiscard]] bool is_read_allowlisted(const fs::path& target);

// Read-gate factory: passes when the path is within the workspace OR
// under a read-allowlist root. Use ONLY in read-side tools (`read`).
[[nodiscard]] std::expected<WorkspacePath, ToolError>
make_readable_path_checked(std::string_view raw, std::string_view tool_name);

// Promote an already-normalised path through the containment gate.
// Useful when the caller composed a NormalizedPath itself (e.g.
// resolving an attachment path against workspace_root()) and now
// wants the typed proof.
[[nodiscard]] std::expected<WorkspacePath, ToolError>
promote_to_workspace_path(NormalizedPath p, std::string_view tool_name);

// True for directory names we want recursive traversals (grep / glob /
// list_dir) to skip by default. Keeps the skip list in one place so tools
// stay in sync (e.g. adding `_deps` to every tool at once).
[[nodiscard]] bool should_skip_dir(std::string_view name) noexcept;

// Heuristic: scan the first 512 bytes for a NUL. Good enough to avoid
// grep'ing PNGs / executables / model weights into the prompt.
[[nodiscard]] bool is_binary_file(const fs::path& p);

// ── Per-file state cache ────────────────────────────────────────────────
// Shared across tools so edit/write can detect "the file changed since the
// model last looked at it". Keyed on canonical path; value is the mtime
// the tool saw plus a content-fingerprint hash so we catch sub-second
// edits that don't bump mtime (some filesystems have 1 s mtime resolution).
//
// Records are written by `read` after every successful read, and by
// `write` / `edit` after a successful mutation (so the next call from
// the same session sees the new state and doesn't false-alarm on its
// own change). Lookups never block on IO — the cache is purely an
// in-memory hint surface.
//
// The cache is process-wide and persists for the lifetime of the agent.
// A long-running session that keeps editing the same files only pays
// the stat cost once per file per change — every subsequent staleness
// check is a hash-table hit.
struct FileSnapshot {
    fs::file_time_type mtime{};
    std::uintmax_t     size = 0;
    std::uint64_t      content_hash = 0;   // FNV-1a of the bytes the tool saw
};

// Record that `path` was observed at `mtime` / `size` with the given
// content hash. `path` is canonicalised internally. Pass `0` for
// `content_hash` when the caller doesn't have the bytes handy (e.g.
// stat-only paths); staleness checks then degrade to mtime+size only.
void record_file_seen(const fs::path& path,
                      fs::file_time_type mtime,
                      std::uintmax_t size,
                      std::uint64_t content_hash) noexcept;

// Look up the snapshot the tools last saw for `path`. Returns nullopt
// when no tool has touched the file this session. `path` is canonicalised.
[[nodiscard]] std::optional<FileSnapshot> last_seen_file(const fs::path& path) noexcept;

// Compute FNV-1a 64-bit over a byte range. Inlineable; used by tools
// that have already read the file to record its hash in the snapshot.
// FNV-1a was picked over xxHash because it's branch-free, zero-alloc,
// and pulls in no dependencies — collision risk at 64 bits across one
// session's worth of files is negligible.
[[nodiscard]] inline std::uint64_t content_fnv1a(std::string_view bytes) noexcept {
    constexpr std::uint64_t kOffset = 0xcbf29ce484222325ULL;
    constexpr std::uint64_t kPrime  = 0x00000100000001b3ULL;
    std::uint64_t h = kOffset;
    for (unsigned char c : bytes) {
        h ^= static_cast<std::uint64_t>(c);
        h *= kPrime;
    }
    return h;
}

// Staleness classification. Computed by checking the file's current
// (mtime, size) and optionally content hash against the cached snapshot.
enum class StaleVerdict : std::uint8_t {
    Unknown,      // no prior snapshot — caller decides what to do
    Fresh,        // snapshot matches current on-disk state
    Stale,        // file changed since the snapshot was recorded
};

// Check whether `path` looks stale relative to its last snapshot.
// Stat-based (cheap): compares mtime + size. Returns Unknown when no
// snapshot exists or stat fails. For a stronger guarantee, the caller
// can additionally hash the file's current bytes and compare against
// the snapshot's `content_hash`.
[[nodiscard]] StaleVerdict staleness_of(const fs::path& path) noexcept;

} // namespace agentty::tools::util
