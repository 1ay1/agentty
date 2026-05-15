#include "agentty/workspace/files.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <utility>

#include "agentty/tool/util/fs_helpers.hpp"

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

std::vector<std::string> list_workspace_files(std::size_t cap) {
    std::vector<std::string> out;
    out.reserve(std::min<std::size_t>(cap, 1024));
    const auto& root = tools::util::workspace_root();
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

        // Same skip rules as list_dir.cpp's recursive walker — keep
        // node_modules / build / .git / __pycache__ / etc. out of the
        // candidate set. Otherwise the picker would be drowned in
        // generated artefacts.
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

        // Workspace-relative paths read better in the picker UI and
        // round-trip cleanly through the FileRef chip caption.
        std::error_code rec;
        auto rel = fs::relative(entry.path(), root, rec);
        out.push_back(rec ? entry.path().string() : rel.string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::size_t>
filter_files(const std::vector<std::string>& files, std::string_view query) {
    std::vector<std::size_t> matches;
    matches.reserve(files.size());
    if (query.empty()) {
        for (std::size_t i = 0; i < files.size(); ++i) matches.push_back(i);
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
        return matches;
    }

    std::vector<Scored> scored;
    scored.reserve(files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        int s = fuzzy_score(files[i], needle);
        if (s != INT32_MIN) scored.push_back({s, i});
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
