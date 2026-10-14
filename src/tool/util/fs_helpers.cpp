#include "agentty/tool/util/fs_helpers.hpp"
#include "agentty/tool/util/disk_files.hpp"
#include "agentty/util/home_dir.hpp"

#include <maya/runtime.hpp>
#include <mcp/tools/util/fs_helpers.hpp>   // project_root(): one rule for both layers

#include <algorithm>
#include <fstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace agentty::tools::util {


fs::path normalize_path(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))  s.remove_suffix(1);
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"')
                          || (s.front() == '\'' && s.back() == '\''))) {
        s.remove_prefix(1);
        s.remove_suffix(1);
    }
    // Tilde expansion. The model writes paths the way a shell would
    // (`~/projects/foo`); without expansion they round-trip into
    // `<cwd>/~/projects/foo`, which doesn't exist, and the tool either
    // returns NotFound or — on filesystems that handle the bizarre
    // prefix slowly — sits on stat() long enough to hit the watchdog.
    // The shell convention: bare `~` or `~/...` expands to $HOME.
    // (`~user/...` per-user expansion isn't supported here; we'd need
    // getpwnam_r for that and the model's never seen it work.)
    std::string expanded;
    if (!s.empty() && s.front() == '~' && (s.size() == 1 || s[1] == '/')) {
        // Home root via the unified resolver ($HOME on POSIX/MSYS2,
        // $USERPROFILE on native Windows) — a bare getenv("HOME") left `~/…`
        // unexpanded on native Windows (HOME usually unset), turning the arg
        // into `<project_root>/~/…` and failing the tool with NotFound.
        const fs::path home = agentty::util::home_dir();
        if (!home.empty()) {
            expanded = home.string();
            expanded.append(s.data() + 1, s.size() - 1);
            s = expanded;
        }
    }
    fs::path p{s};
    if (!p.is_absolute()) {
        // Resolve relative to the ACTIVE PROJECT (cwd clamped inside the
        // access boundary), NOT the tool thread's cwd via fs::absolute()
        // and NOT the raw boundary. fs::absolute() anchored to whatever
        // cwd the worker thread happened to have, which could fail the
        // workspace-containment check for a path the model reasonably
        // expected to be in-bounds; the raw boundary would turn
        // `src/foo.cpp` into `/src/foo.cpp` under `--workspace /`.
        // project_root() is the right anchor and, by default, equals the
        // boundary so common launches are unchanged.
        p = project_root() / p;
    }
    return p.lexically_normal();
}

namespace {

// Function-local static so the default value is the cwd at first call,
// regardless of static-initialisation order. Tests that don't call
// set_workspace_root() still get a sensible default.
fs::path& mutable_workspace_root() {
    static fs::path root = [] {
        std::error_code ec;
        auto cwd = fs::current_path(ec);
        return ec ? fs::path{"/"} : cwd;
    }();
    return root;
}

} // namespace

void set_workspace_root(Io, fs::path root) {
    std::error_code ec;
    // On Windows, weakly_canonical can fail on junctions/reparse points.
    // If canonicalisation fails, use the path as-is (it's already absolute
    // or we normalize it below). This degrades gracefully rather than
    // rejecting valid workspace roots.
    auto canon = fs::weakly_canonical(root, ec);
    auto resolved = ec ? std::move(root) : std::move(canon);
    // Ensure it's absolute; on Windows some bad canonicalisations might
    // return relative paths in error cases.
    if (!resolved.is_absolute()) {
        std::error_code aec;
        auto abs = fs::absolute(resolved, aec);
        if (!aec) resolved = std::move(abs);
    }
    // The mcp-cpp tools read this same root: the tool state agentty hands
    // them takes the boundary from here on every call.
    mutable_workspace_root() = std::move(resolved);
}

const fs::path& workspace_root() {
    return mutable_workspace_root();
}

fs::path project_root() {
    // mcp-cpp's rule (the cwd, clamped inside the boundary), over our
    // boundary, so agentty-native and mcp-served tools resolve relative paths
    // to the identical directory.
    static DiskFiles disk;   // stateless: one is enough
    return ::mcp::tools::util::project_root(
        ::mcp::tools::util::bounds_from(disk, workspace_root(), {}));
}

bool is_within_workspace(const fs::path& target) {
    if (target.empty()) return false;
    std::error_code ec;
    // weakly_canonical resolves what exists and leaves the rest as-is —
    // perfect for write targets where the file doesn't exist yet but the
    // parent directory does, and it resolves symlinks so an in-workspace
    // link pointing at /etc fails the prefix check.
    // On Windows, this can fail on junctions/reparse points. If it fails,
    // retry on the parent (common write-target case: file doesn't exist yet
    // but the directory does). If even that fails, try a parent-relative
    // approach as a last resort.
    auto canon_target = fs::weakly_canonical(target, ec);
    if (ec) {
        // weakly_canonical failed (e.g. a parent component is a dangling
        // symlink, or EACCES walking the chain, or a Windows reparse point).
        // Do NOT fall back to a plain absolute() — that skips symlink
        // resolution and would let a crafted link escape the boundary.
        // Retry on the parent dir (the common write-target case: file doesn't
        // exist yet but the directory does, and canonicalising it still
        // resolves symlinks), then re-attach the filename.
        std::error_code pec;
        auto parent = target.parent_path();
        if (!parent.empty()) {
            auto canon_parent = fs::weakly_canonical(parent, pec);
            if (!pec && !canon_parent.empty()) {
                canon_target = canon_parent / target.filename();
                ec.clear();
            }
        }
        // If even parent retry failed, fall back to lexically_normal of the
        // absolute path (no symlink resolution, but at least consistent).
        if (ec) {
            std::error_code aec;
            auto abs = fs::absolute(target, aec);
            if (!aec) {
                canon_target = abs.lexically_normal();
                ec.clear();
            } else {
                return false;   // can't even get absolute; deny
            }
        }
    }
    auto canon_root = workspace_root();   // already canonicalised on set
    // Component-wise prefix check. Plain string startsWith would let
    // /home/user/project-other through when root is /home/user/project.
    auto rt = canon_root.begin();
    auto tt = canon_target.begin();
    for (; rt != canon_root.end() && tt != canon_target.end(); ++rt, ++tt) {
        if (*rt != *tt) return false;
    }
    return rt == canon_root.end();
}

// ── Read-only allowlist roots ───────────────────────────────────────────
namespace {

// Canonicalised roots the READ gate also accepts (skill directories
// under ~/.agentty/skills etc.). Reads come from tool threads, registration
// from the skills scanner. A snapshot: a handful of roots, read on every
// gate check, appended rarely — no lock, so the gate needs no capability.
using Roots = std::vector<std::string>;
[[nodiscard]] maya::snapshot<Roots>& read_roots() {
    static maya::snapshot<Roots> r;
    return r;
}

// Component-wise "target under root" — same shape as the workspace
// prefix check above; factored so both gates share the symlink-safe
// canonicalisation rules.
[[nodiscard]] bool under_root(const fs::path& canon_target, const fs::path& root) {
    auto rt = root.begin();
    auto tt = canon_target.begin();
    for (; rt != root.end() && tt != canon_target.end(); ++rt, ++tt)
        if (*rt != *tt) return false;
    return rt == root.end();
}

} // namespace

void allow_read_root(Io, const fs::path& root) {
    std::error_code ec;
    auto canon = fs::weakly_canonical(root, ec);
    // On Windows, weakly_canonical can fail on junctions. If it fails,
    // fall back to lexically_normal(absolute(root)). This provides a
    // stable cache key even if we can't resolve symlinks.
    if (ec) {
        std::error_code aec;
        auto abs = fs::absolute(root, aec);
        if (aec) return;   // can't even get absolute; bail
        canon = abs.lexically_normal();
    }
    if (canon.empty()) return;
    read_roots().update(
        [](const Roots& roots, const std::string& c) {
            if (std::find(roots.begin(), roots.end(), c) != roots.end()) return roots;
            Roots next = roots;
            next.push_back(c);
            return next;
        },
        canon.string());
}

std::vector<fs::path> read_roots_now() {
    const auto roots = read_roots().load();
    std::vector<fs::path> out;
    out.reserve(roots->size());
    for (const auto& r : *roots) out.emplace_back(r);
    return out;
}

void allow_fork_transcripts(Io io, const fs::path& threads_dir,
                            const std::vector<std::string>& paths) {
    std::error_code ec;
    const auto dir = fs::weakly_canonical(threads_dir, ec);
    if (ec || dir.empty()) return;
    constexpr std::string_view kSuffix = ".transcript.md";
    for (const auto& raw : paths) {
        if (raw.empty()) continue;
        std::error_code fec;
        const auto file = fs::weakly_canonical(fs::path{raw}, fec);
        if (fec || !fs::is_regular_file(file, fec)) continue;
        // One level down in the threads dir, and only a transcript. A note
        // edited to point anywhere else opens nothing.
        if (file.parent_path() != dir) continue;
        const auto name = file.filename().string();
        if (name.size() <= kSuffix.size()
            || name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0)
            continue;
        allow_read_root(io, file);
    }
}

bool is_read_allowlisted(const fs::path& target) {
    if (target.empty()) return false;
    std::error_code ec;
    auto canon = fs::weakly_canonical(target, ec);
    if (ec) {
        // weakly_canonical failed (bad junction, etc.). Fall back to
        // lexically_normal(absolute()). Reads need the file to exist at
        // comparison time, so no parent retry like is_within_workspace.
        std::error_code aec;
        auto abs = fs::absolute(target, aec);
        if (aec) return false;   // can't canonicalise; deny
        canon = abs.lexically_normal();
    }
    const auto roots = read_roots().load();
    for (const auto& r : *roots)
        if (under_root(canon, fs::path{r})) return true;
    return false;
}

std::expected<NormalizedPath, ToolError>
make_workspace_path(std::string_view raw, std::string_view tool_name) {
    NormalizedPath p{raw};
    if (!is_within_workspace(p.path())) {
        // Helpful, actionable message: name the offending path, the
        // active root, and the two ways out (restart in a wider dir or
        // pass --workspace). The model can read this and either ask the
        // user to widen the scope or pick a different path.
        return std::unexpected(ToolError::out_of_workspace(
            "tool '" + std::string{tool_name} + "' refused: '"
            + p.string() + "' is outside the workspace root '"
            + workspace_root().string() + "'. "
            "Restart agentty in a parent directory or pass "
            "--workspace <dir> to widen the scope."));
    }
    return p;
}

// ── WorkspacePath factories ───────────────────────────────────────
std::expected<WorkspacePath, ToolError>
make_workspace_path_checked(std::string_view raw, std::string_view tool_name) {
    auto np = make_workspace_path(raw, tool_name);
    if (!np) return std::unexpected(std::move(np.error()));
    return WorkspacePath{std::move(*np)};
}

std::expected<WorkspacePath, ToolError>
promote_to_workspace_path(NormalizedPath p, std::string_view tool_name) {
    if (!is_within_workspace(p.path())) {
        return std::unexpected(ToolError::out_of_workspace(
            "tool '" + std::string{tool_name} + "' refused: '"
            + p.string() + "' is outside the workspace root '"
            + workspace_root().string() + "'."));
    }
    return WorkspacePath{std::move(p)};
}

std::expected<WorkspacePath, ToolError>
make_readable_path_checked(std::string_view raw, std::string_view tool_name) {
    NormalizedPath p{raw};
    if (is_within_workspace(p.path()) || is_read_allowlisted(p.path()))
        return WorkspacePath{std::move(p)};
    return std::unexpected(ToolError::out_of_workspace(
        "tool '" + std::string{tool_name} + "' refused: '"
        + p.string() + "' is outside the workspace root '"
        + workspace_root().string() + "' and not under any skill "
        "directory. Restart agentty in a parent directory or pass "
        "--workspace <dir> to widen the scope."));
}

// ── Checked read/write delegates ────────────────────────────────
std::string read_file(const WorkspacePath& p) {
    return read_file(p.path());
}

std::string write_file(const WorkspacePath& p, std::string_view content) {
    return write_file(p.path(), content);
}

bool should_skip_dir(std::string_view name) noexcept {
    static const std::vector<std::string_view> skip = {
        ".git", "node_modules", "build", "target", "__pycache__",
        ".cache", "vendor", "dist", "out", ".next", ".venv",
        "cmake-build-debug", "cmake-build-release", ".idea", ".vscode",
        "_deps", "third_party", "thirdparty", "3rdparty", "external",
        ".mypy_cache", ".pytest_cache", ".tox", ".gradle", "Pods",
        "bazel-bin", "bazel-out", "DerivedData", ".terraform",
    };
    for (auto s : skip) if (name == s) return true;
    // Prefix families: out-of-source CMake build trees (build/, build-*,
    // cmake-build-*) are full of generated + fetched (_deps) files that
    // would otherwise flood the @-file picker / symbol index. Kept in sync
    // with mcp-cpp's should_skip_dir.
    if (name.starts_with("build-") || name.starts_with("cmake-build"))
        return true;
    return false;
}

bool is_binary_file(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    // Can't open (permission denied, transient lock, race-deleted): NOT
    // a binary verdict. Returning `true` here made grep/glob silently
    // skip the file and made read/edit report a bogus "cannot read
    // binary file". Return `false` so the caller's real read runs and
    // surfaces the actual open error instead of masking it.
    if (!ifs) return false;
    char buf[512];
    ifs.read(buf, sizeof(buf));
    auto n = ifs.gcount();
    for (int i = 0; i < n; ++i)
        if (buf[i] == '\0') return true;
    return false;
}

// ── File snapshot cache ────────────────────────────────────────────────────
namespace {

// The snapshot each tool last saw, by canonical path. FileSnapshot holds a
// file_time_type, which jaal can't see inside, so the cache keeps the ticks
// and rebuilds the snapshot on the way out.
struct SeenFile {
    // The clock's own tick type, not int64_t: libstdc++ uses a 64-bit rep,
    // libc++ (clang on macOS) uses __int128 -- narrowing that into int64_t
    // in a brace-init is a hard error there.
    fs::file_time_type::rep mtime = 0;
    std::uintmax_t size = 0;
    std::uint64_t  content_hash = 0;
};
// A snapshot: one entry per file the agent touched this session (hundreds at
// most), written once per read/write tool call and read by the staleness
// gate. Copy-on-write is cheap at that size and keeps these noexcept helpers
// lock-free, so no capability reaches the tool shells that call them.
using SeenMap = std::unordered_map<std::string, SeenFile>;
[[nodiscard]] maya::snapshot<SeenMap>& file_cache() {
    static maya::snapshot<SeenMap> c;
    return c;
}

// Canonicalise without requiring the file to exist (write target might
// not exist yet at the moment we record). Falls back to the lexically-
// normal absolute form when weakly_canonical errors out — we still get
// a stable key per file across calls, just not symlink-aware. On Windows,
// weakly_canonical can fail on junctions/reparse points; degrading to
// lexically_normal(absolute()) is safer than returning empty or crashing.
[[nodiscard]] std::string canon_key(const fs::path& p) noexcept {
    std::error_code ec;
    auto canon = fs::weakly_canonical(p, ec);
    if (!ec) return canon.string();
    // Fallback 1: if p is already absolute, use its lexically normal form
    if (p.is_absolute()) return p.lexically_normal().string();
    // Fallback 2: make it absolute, then lexically normal
    auto abs = fs::absolute(p, ec);
    if (!ec) return abs.lexically_normal().string();
    // Fallback 3: return as-is (shouldn't happen, but defensive)
    return p.string();
}

} // namespace

void record_file_seen(const fs::path& path,
                      fs::file_time_type mtime,
                      std::uintmax_t size,
                      std::uint64_t content_hash) noexcept {
    auto key = canon_key(path);
    if (key.empty()) return;
    try {
        file_cache().update(
            [](const SeenMap& m, const std::string& k, const SeenFile& v) {
                SeenMap next = m; next[k] = v; return next;
            },
            std::move(key), SeenFile{mtime.time_since_epoch().count(), size, content_hash});
    } catch (...) { /* best-effort bookkeeping; noexcept contract */ }
}

std::optional<FileSnapshot> last_seen_file(const fs::path& path) noexcept {
    auto key = canon_key(path);
    if (key.empty()) return std::nullopt;
    const auto m = file_cache().load();
    auto it = m->find(key);
    if (it == m->end()) return std::nullopt;
    const SeenFile& seen = it->second;
    return FileSnapshot{fs::file_time_type{fs::file_time_type::duration{seen.mtime}},
                        seen.size, seen.content_hash};
}

StaleVerdict staleness_of(const fs::path& path) noexcept {
    auto snap = last_seen_file(path);
    if (!snap) return StaleVerdict::Unknown;
    std::error_code ec;
    // On Windows, last_write_time/file_size can fail on junctions/reparse
    // points. If we can't stat the current file, we can't compare; return
    // Unknown (treat as potentially stale to be safe).
    auto cur_mtime = fs::last_write_time(path, ec);
    if (ec) return StaleVerdict::Unknown;
    auto cur_size  = fs::file_size(path, ec);
    if (ec) cur_size = 0;   // default to 0 on error; still compare with snap
    // mtime granularity is filesystem-dependent (HFS+ 1 s, ext4 ns, FAT
    // 2 s). Compare strictly: any difference — forward OR backward —
    // counts as stale. Size mismatch is also a stale signal even when
    // mtime didn't change (some tools touch-without-updating-mtime).
    if (snap->mtime == cur_mtime && snap->size == cur_size)
        return StaleVerdict::Fresh;
    return StaleVerdict::Stale;
}

} // namespace agentty::tools::util
