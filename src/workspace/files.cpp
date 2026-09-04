#include "agentty/workspace/files.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "agentty/tool/util/fs_helpers.hpp"
#include "agentty/tool/util/subprocess.hpp"

namespace agentty {

namespace {
namespace fs = std::filesystem;

[[nodiscard]] inline char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

// ── Fuzzy scorer ────────────────────────────────────────────────────
//
// Subsequence match: every character of `needle` must appear in `hay`
// in order (case-insensitive), but not necessarily contiguously. The
// score rewards matches that *look* like what a human typing the
// query meant — basename hits, prefix hits, camelhump / path-segment
// starts, and consecutive runs. Returns INT_MIN if `needle` is not a
// subsequence of `hay`.
//
// Heuristics (loosely modelled on fzf v2 and VS Code's filematch):
//
//   + per matched char           +16
//   + consecutive run bonus       +18 per char after the first
//   + match starts a "word"       +30   (start-of-string, after '/', '_',
//                                       '-', '.', ' ', or lower→upper hump)
//   + match inside basename       +12   (last path segment after final '/')
//   + exact basename equals query +200
//   + basename starts with query  +120
//   + full path starts with query +60
//   - per skipped char in hay      -1   (mild penalty: shorter paths
//                                       with the same hit pattern win)
//   - depth penalty                -2 * (#'/'s in hay)
//
// All ints; no allocations on the hot path.
[[nodiscard]] inline std::string_view basename_view(std::string_view p) noexcept {
    auto slash = p.find_last_of("/\\");
    return slash == std::string_view::npos ? p : p.substr(slash + 1);
}

// Return the lowercase extension WITHOUT the leading dot ("cpp", "md",
// "" if none). Multi-dot names like "foo.tar.gz" return only the last
// segment ("gz") — matches what humans usually mean by "extension".
[[nodiscard]] inline std::string ext_of(std::string_view path) {
    auto base = basename_view(path);
    auto dot  = base.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == base.size())
        return {};
    std::string e;
    e.reserve(base.size() - dot - 1);
    for (std::size_t i = dot + 1; i < base.size(); ++i)
        e.push_back(ascii_lower(base[i]));
    return e;
}

// Per-file-class bias added to every score — makes source code surface
// above docs / configs / assets when the fuzzy match is otherwise
// comparable. Values are deliberately on the same order of magnitude as
// the matching bonuses (a strong filename hit can still beat the class
// bias) so this nudges, doesn't dominate.
//
//   +90  primary source     .c .cc .cpp .cxx .h .hpp .rs .go .py .ts .tsx
//                            .js .jsx .java .kt .swift .rb .php .lua .zig
//                            .scala .clj .ex .exs .ml .hs .dart .nim .v
//   +60  build / project    CMakeLists.txt .cmake Makefile .mk .bazel
//                            BUILD WORKSPACE .gradle .sbt Cargo.toml
//                            go.mod package.json pyproject.toml etc.
//   +30  docs / config      .md .rst .txt .json .yaml .yml .toml .ini
//                            .conf .xml .html .css .scss .sql .proto
//   − 0  unknown / generic
//   −50  assets / data      .png .jpg .jpeg .gif .webp .svg .ico .pdf
//                            .mp3 .mp4 .mov .wav .ogg .flac .zip .tar
//                            .gz .xz .7z .bin .iso .lock
//   −50  hidden dotfiles    basename starts with '.' (.bashrc, .gitignore)
//                            — power-users still get to them by typing the
//                            name; they just stop crowding the top.
[[nodiscard]] int class_bias(std::string_view path) noexcept {
    auto base = basename_view(path);
    int bias = 0;
    if (!base.empty() && base.front() == '.') bias -= 50;

    // Whole-filename specials (no extension, or filename IS the signal).
    auto eq_ci = [](std::string_view a, std::string_view b) noexcept {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
        return true;
    };
    if (eq_ci(base, "CMakeLists.txt") || eq_ci(base, "Makefile") ||
        eq_ci(base, "GNUmakefile")     || eq_ci(base, "BUILD")    ||
        eq_ci(base, "WORKSPACE")       || eq_ci(base, "Dockerfile") ||
        eq_ci(base, "Cargo.toml")      || eq_ci(base, "go.mod")   ||
        eq_ci(base, "package.json")    || eq_ci(base, "pyproject.toml") ||
        eq_ci(base, "pnpm-lock.yaml")  || eq_ci(base, "yarn.lock") ||
        eq_ci(base, "poetry.lock")     || eq_ci(base, "Gemfile")  ||
        eq_ci(base, "Rakefile"))
        return bias + 60;

    auto e = ext_of(path);
    if (e.empty()) return bias;

    // Ordered roughly by frequency in real codebases — the early returns
    // make the common case (source file) one strcmp deep.
    static constexpr std::string_view src[] = {
        "c",  "cc",  "cpp", "cxx", "c++", "h",  "hh", "hpp", "hxx", "h++",
        "rs", "go",  "py",  "ts",  "tsx", "js", "jsx", "mjs", "cjs",
        "java", "kt", "kts", "swift", "m", "mm",
        "rb", "php", "lua", "zig", "scala", "clj", "cljs", "cljc",
        "ex", "exs", "erl", "hrl", "ml", "mli", "hs", "dart",
        "nim", "v",  "sv", "vhd", "sh", "bash", "zsh", "fish", "ps1",
        "r",  "jl", "sol", "tf", "hcl",
    };
    for (auto s : src) if (e == s) return bias + 90;

    static constexpr std::string_view build[] = {
        "cmake", "mk", "make", "bazel", "bzl", "gradle", "sbt",
        "ninja",
    };
    for (auto s : build) if (e == s) return bias + 60;

    static constexpr std::string_view docs_cfg[] = {
        "md", "mdx", "rst", "txt", "adoc", "org",
        "json", "json5", "jsonc", "yaml", "yml", "toml", "ini", "conf",
        "cfg", "properties", "env", "xml", "html", "htm", "css", "scss",
        "sass", "less", "sql", "proto", "graphql", "gql",
    };
    for (auto s : docs_cfg) if (e == s) return bias + 30;

    static constexpr std::string_view assets[] = {
        "png", "jpg", "jpeg", "gif", "webp", "svg", "ico", "bmp", "tiff",
        "pdf", "psd", "ai",
        "mp3", "mp4", "mov", "avi", "mkv", "webm", "wav", "ogg", "flac",
        "m4a", "aac", "opus",
        "zip", "tar", "gz", "tgz", "xz", "bz2", "7z", "rar",
        "bin", "iso", "img", "dmg", "exe", "dll", "so", "dylib", "a",
        "o", "obj", "class", "jar", "war", "pyc", "pyo", "wasm",
        "lock", "sum", "midi", "mid",
    };
    for (auto s : assets) if (e == s) return bias - 50;

    return bias;
}

[[nodiscard]] inline bool is_word_boundary(std::string_view hay,
                                           std::size_t i) noexcept {
    if (i == 0) return true;
    char prev = hay[i - 1];
    char cur  = hay[i];
    if (prev == '/' || prev == '\\' || prev == '_' || prev == '-' ||
        prev == '.' || prev == ' ')
        return true;
    // camelhump: lower → upper transition
    if (prev >= 'a' && prev <= 'z' && cur >= 'A' && cur <= 'Z') return true;
    return false;
}

struct Scored {
    int         score;
    std::size_t idx;
};

// Returns INT_MIN if `needle` is not a subsequence of `hay`. `hay`
// and `needle_lower` are both expected lowercase ASCII; `hay_orig`
// is the original-case path used for camelhump detection.
[[nodiscard]] int fuzzy_score(std::string_view hay_orig,
                              std::string_view needle_lower) noexcept {
    if (needle_lower.empty()) return 0;
    if (needle_lower.size() > hay_orig.size()) return INT32_MIN;

    const auto base = basename_view(hay_orig);
    const std::size_t base_off = static_cast<std::size_t>(
        base.data() - hay_orig.data());

    // Fast-path bonuses (computed against lowercase basename / full path).
    int bonus = 0;
    {
        // Compare basename case-insensitively to needle.
        bool base_eq = base.size() == needle_lower.size();
        bool base_pref = base.size() >= needle_lower.size();
        bool path_pref = hay_orig.size() >= needle_lower.size();
        for (std::size_t k = 0; k < needle_lower.size(); ++k) {
            char nb = needle_lower[k];
            if (base_eq   && ascii_lower(base[k])     != nb) base_eq   = false;
            if (base_pref && ascii_lower(base[k])     != nb) base_pref = false;
            if (path_pref && ascii_lower(hay_orig[k]) != nb) path_pref = false;
            if (!base_eq && !base_pref && !path_pref) break;
        }
        if (base_eq)        bonus += 200;
        else if (base_pref) bonus += 120;
        if (path_pref)      bonus += 60;
    }

    // Greedy left-to-right subsequence walk. Greedy is suboptimal in
    // the general case (the optimal alignment is O(n*m) DP), but for
    // path strings of typical length the difference is invisible and
    // the greedy version is ~50× faster — easily fast enough to run
    // on every keystroke against thousands of paths.
    int score = 0;
    std::size_t hi = 0;  // index into hay
    std::size_t ni = 0;  // index into needle
    bool prev_matched = false;
    int  skipped = 0;

    while (ni < needle_lower.size() && hi < hay_orig.size()) {
        char nc = needle_lower[ni];
        char hc = ascii_lower(hay_orig[hi]);
        if (hc == nc) {
            score += 16;
            if (prev_matched)            score += 18;
            if (is_word_boundary(hay_orig, hi)) score += 30;
            if (hi >= base_off)          score += 12;
            prev_matched = true;
            ++ni;
            ++hi;
        } else {
            prev_matched = false;
            ++skipped;
            ++hi;
        }
    }
    if (ni < needle_lower.size()) return INT32_MIN;  // not a subsequence

    score -= skipped;  // mild penalty for "chars we walked past"

    // Depth penalty — given two equally good matches, prefer the
    // shallower path (`src/foo.cpp` over `build-x/.../foo.cpp`).
    int slashes = 0;
    for (char c : hay_orig) if (c == '/' || c == '\\') ++slashes;
    score -= 2 * slashes;

    // File-class bias — source code beats docs beats assets. See
    // class_bias() above for the table; values are tuned to nudge
    // tied matches without overriding a clearly better filename hit.
    score += class_bias(hay_orig);

    return score + bonus;
}

} // namespace

// ── Cache + async prewarm ───────────────────────────────────
// The file list is built ONCE per process. Historically the build ran
// synchronously on the first `@` keystroke — a recursive_directory_iterator
// over thousands of paths blocking the UI thread for the whole walk. Now:
//   • prewarm_workspace_files() kicks the walk on a detached thread at
//     startup, so by the time the user reaches for `@` the list is ready;
//   • the result is published behind a shared_ptr swapped under a mutex,
//     so readers never block and never see a half-built vector;
//   • files_ready() is a non-blocking probe the composer uses to open the
//     picker INSTANTLY (empty + "indexing…") when the warm hasn't landed.
namespace {
std::mutex& files_mu() { static std::mutex m; return m; }
std::shared_ptr<const std::vector<std::string>>& files_cache() {
    static std::shared_ptr<const std::vector<std::string>> c;
    return c;
}
std::atomic<bool>& files_building() { static std::atomic<bool> b{false}; return b; }

// Frecency: paths the user has referenced, most-recent-first-weighted.
// A tiny recency list (not a full frecency decay) — the last-referenced
// files are exactly what a follow-up `@` wants at the top.
std::mutex& frecency_mu() { static std::mutex m; return m; }
std::vector<std::string>& frecency_list() {
    static std::vector<std::string> v; return v;
}

// ── Git signals ──────────────────────────────────────────
// The single strongest "which file matters" signal is git: the files you
// have modified / staged / just touched ARE your working set. FFF and Zed
// both lead their pickers with these. We compute a status map once at
// prewarm (git status --porcelain + recent commit touch history) and fold
// it into ranking + row tags. path (workspace-relative) -> GitTag.
std::mutex& git_mu() { static std::mutex m; return m; }
std::unordered_map<std::string, GitTag>& git_status_map() {
    static std::unordered_map<std::string, GitTag> m; return m;
}
std::atomic<bool>& git_ready_flag() { static std::atomic<bool> b{false}; return b; }

void build_git_signals() {
    const auto root = tools::util::project_root();
    if (root.empty()) { git_ready_flag() = true; return; }
    std::unordered_map<std::string, GitTag> tags;

    // 1. Working-tree status. NEWLINE-separated (NOT -z: the subprocess
    //    scrubber eats NUL bytes), quotePath off so unicode paths survive.
    //    Porcelain v1 XY columns: X=index, Y=worktree.
    auto st = tools::util::run_argv_s(
        {"git", "-C", root.string(), "-c", "core.quotePath=false",
         "status", "--porcelain", "--untracked-files=all"},
        512 * 1024, std::chrono::seconds{8});
    if (st.started && st.exit_code == 0) {
        std::istringstream in(st.output);
        std::string line;
        while (std::getline(in, line)) {
            if (line.size() < 4) continue;
            char x = line[0], y = line[1];
            std::string path = line.substr(3);
            // Rename form "old -> new": keep the new path.
            if (auto arrow = path.find(" -> "); arrow != std::string::npos)
                path = path.substr(arrow + 4);
            GitTag tag = GitTag::None;
            if (x == '?' && y == '?')            tag = GitTag::Untracked;
            else if (x != ' ' && x != '?')       tag = GitTag::Staged;   // index change
            else if (y != ' ')                   tag = GitTag::Modified; // worktree change
            if (tag != GitTag::None) tags[path] = tag;
        }
    }

    // 2. Recent-commit touch history: files changed in the last ~20 commits
    //    are part of the current line of work even when clean now. Weaker
    //    than a live dirty status — only tag files not already flagged.
    auto touched = tools::util::run_argv_s(
        {"git", "-C", root.string(), "-c", "core.quotePath=false",
         "log", "--name-only", "--pretty=format:", "-n", "20"},
        1024 * 1024, std::chrono::seconds{8});
    if (touched.started && touched.exit_code == 0) {
        std::istringstream in(touched.output);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            tags.emplace(line, GitTag::RecentlyCommitted);   // emplace = no override
        }
    }

    {
        std::lock_guard<std::mutex> lk(git_mu());
        git_status_map() = std::move(tags);
    }
    git_ready_flag() = true;
}

std::vector<std::string> build_file_list(std::size_t cap) {
    std::vector<std::string> out;
    out.reserve(std::min<std::size_t>(cap, 1024));
    const auto root = tools::util::project_root();
    if (root.empty()) return out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator() && out.size() < cap;
         it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const auto& entry = *it;
        auto fn = entry.path().filename().string();
        const bool is_dir = entry.is_directory(ec);
        if (is_dir && tools::util::should_skip_dir(fn)) {
            it.disable_recursion_pending();
            continue;
        }
        if (is_dir && it.depth() > 0 && fn.starts_with(".")) {
            it.disable_recursion_pending();
            continue;
        }
        if (is_dir) continue;
        if (!entry.is_regular_file(ec)) continue;
        std::error_code rec;
        auto rel = fs::relative(entry.path(), root, rec);
        out.push_back(rec ? entry.path().string() : rel.string());
    }
    std::sort(out.begin(), out.end());
    return out;
}
} // namespace

namespace {
// The prewarm walk is OWNED, not detached. A detached FS walk still touching
// its captured statics (and the mimalloc heap) when a fast pipe-EOF exit runs
// the CRT atexit handlers is a use-after-free that faults on Windows
// (0xC0000005) — the same class of bug join_prewarm() closed for TLS dials.
// main() calls join_workspace_prewarm() before teardown; the thread is kept
// joinable here so it can.
std::thread& files_prewarm_thread() {
    static std::thread t;
    return t;
}
}  // namespace

void prewarm_workspace_files(std::size_t cap) {
    // Single-flight: only the first caller spawns the walk.
    bool expected = false;
    if (!files_building().compare_exchange_strong(expected, true)) return;
    {
        std::lock_guard<std::mutex> lk(files_mu());
        if (files_cache()) { files_building() = false; return; }   // already warm
    }
    // A prior prewarm thread may have finished but not yet been joined;
    // assigning over a joinable std::thread calls std::terminate, so reap it
    // first. (Single-flight above makes a second LIVE walk impossible, so
    // this join returns immediately.)
    if (files_prewarm_thread().joinable()) files_prewarm_thread().join();
    files_prewarm_thread() = std::thread([cap] {
        // Git signals first (fast, ~two git invocations) so the file list is
        // already git-aware the instant it publishes — a blank `@` leads
        // with your dirty files from the very first open.
        build_git_signals();
        auto built = std::make_shared<std::vector<std::string>>(build_file_list(cap));
        {
            std::lock_guard<std::mutex> lk(files_mu());
            files_cache() = std::move(built);
        }
        files_building() = false;
    });
}

void join_workspace_prewarm() {
    auto& t = files_prewarm_thread();
    if (t.joinable()) t.join();
}

bool files_ready() {
    std::lock_guard<std::mutex> lk(files_mu());
    return static_cast<bool>(files_cache());
}

void note_file_referenced(std::string_view path) {
    if (path.empty()) return;
    std::lock_guard<std::mutex> lk(frecency_mu());
    auto& v = frecency_list();
    std::erase(v, std::string{path});     // dedupe — move to front
    v.insert(v.begin(), std::string{path});
    if (v.size() > 64) v.resize(64);      // bounded recency window
}

GitTag file_git_tag(std::string_view path) {
    std::lock_guard<std::mutex> lk(git_mu());
    auto it = git_status_map().find(std::string{path});
    return it == git_status_map().end() ? GitTag::None : it->second;
}

std::string_view git_tag_label(GitTag tag) {
    switch (tag) {
        case GitTag::Modified:          return "modified";
        case GitTag::Staged:            return "staged";
        case GitTag::Untracked:         return "new";
        case GitTag::RecentlyCommitted: return "recent";
        default:                        return "";
    }
}

void refresh_git_signals() { build_git_signals(); }

std::vector<std::string> list_workspace_files(std::size_t cap) {
    // Fast path: warm cache → return it (a copy, cheap: it's paths).
    {
        std::lock_guard<std::mutex> lk(files_mu());
        if (auto c = files_cache()) return *c;
    }
    // Cold and someone needs it NOW (synchronous caller, no prewarm yet):
    // build inline and publish, so subsequent calls are instant.
    auto built = std::make_shared<std::vector<std::string>>(build_file_list(cap));
    {
        std::lock_guard<std::mutex> lk(files_mu());
        if (!files_cache()) files_cache() = built;
        return *files_cache();
    }
}

std::vector<std::size_t>
filter_files(const std::vector<std::string>& files, std::string_view query) {
    std::vector<std::size_t> matches;
    matches.reserve(files.size());

    // Frecency snapshot: path → recency rank (0 = most recent). Small map,
    // read once per filter pass.
    std::unordered_map<std::string, int> frec;
    {
        std::lock_guard<std::mutex> lk(frecency_mu());
        const auto& v = frecency_list();
        for (std::size_t i = 0; i < v.size(); ++i) frec.emplace(v[i], (int)i);
    }
    // Git-status snapshot: the STRONGEST signal. The file you're editing is
    // almost always dirty/staged — lead with it.
    std::unordered_map<std::string, GitTag> git;
    {
        std::lock_guard<std::mutex> lk(git_mu());
        git = git_status_map();
    }
    auto git_bonus = [&](std::size_t i) -> int {
        auto it = git.find(files[i]);
        if (it == git.end()) return 0;
        switch (it->second) {
            case GitTag::Modified:          return 100000;   // actively editing
            case GitTag::Staged:            return 90000;
            case GitTag::Untracked:         return 80000;
            case GitTag::RecentlyCommitted: return 30000;
            default:                        return 0;
        }
    };
    auto frec_bonus = [&](std::size_t i) -> int {
        auto it = frec.find(files[i]);
        if (it == frec.end()) return 0;
        // Recent files get a big, rank-decaying bonus so "the file I just
        // touched" floats to the top of both the no-query list and ties.
        return 10000 - it->second * 100;
    };
    // Combined context score — git dominates, frecency breaks its ties,
    // and both sit far above the fuzzy base so a working-set file never
    // sinks below an alphabetically-earlier stranger with a marginally
    // better fuzzy score.
    auto ctx_bonus = [&](std::size_t i) { return git_bonus(i) + frec_bonus(i); };

    if (query.empty()) {
        for (std::size_t i = 0; i < files.size(); ++i) matches.push_back(i);
        // Working-set first (git dirty → staged → new → recent → frecency),
        // then the pre-sorted alphabetic order for everything untouched.
        std::stable_sort(matches.begin(), matches.end(),
            [&](std::size_t a, std::size_t b) {
                return ctx_bonus(a) > ctx_bonus(b);
            });
        return matches;
    }

    // Strip whitespace from the query — the user typed a filter, not
    // a literal pattern. Internal spaces are kept as subsequence gaps
    // (so "src cpp" still matches "src/foo.cpp").
    std::string needle;
    needle.reserve(query.size());
    for (char c : query) {
        if (c == ' ' || c == '\t') continue;
        needle.push_back(ascii_lower(c));
    }
    if (needle.empty()) {
        for (std::size_t i = 0; i < files.size(); ++i) matches.push_back(i);
        std::stable_sort(matches.begin(), matches.end(),
            [&](std::size_t a, std::size_t b) {
                return ctx_bonus(a) > ctx_bonus(b);
            });
        return matches;
    }

    std::vector<Scored> scored;
    scored.reserve(files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        int s = fuzzy_score(files[i], needle);
        if (s != INT32_MIN) scored.push_back({s + ctx_bonus(i), i});
    }

    // Stable sort: equal scores fall back to the alphabetic order the
    // list_workspace_files() pass already established. Higher score first.
    std::stable_sort(scored.begin(), scored.end(),
        [](const Scored& a, const Scored& b) { return a.score > b.score; });

    matches.reserve(scored.size());
    for (const auto& s : scored) matches.push_back(s.idx);
    return matches;
}

} // namespace agentty
