#include "agentty/workspace/symbols.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <regex>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "agentty/tool/util/fs_helpers.hpp"

namespace agentty {

namespace {
namespace fs = std::filesystem;

std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s)
        out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

// File extensions we scan. Mirrors find_definition.cpp's set; if a
// language extension is added there, mirror it here.
bool is_source_ext(std::string_view ext) noexcept {
    static const std::string_view kExts[] = {
        ".cpp", ".hpp", ".c", ".h", ".cc", ".hh", ".cxx", ".hxx",
        ".py", ".js", ".ts", ".jsx", ".tsx",
        ".go", ".rs", ".java", ".kt", ".rb", ".swift", ".zig", ".lua",
    };
    for (auto e : kExts) if (ext == e) return true;
    return false;
}

// One regex per language family. Captures the symbol name in group 1.
// Pre-built once at first use so we don't recompile per file.
const std::vector<std::regex>& symbol_patterns() {
    // std::regex compilation is non-trivial; do it once.
    static const std::vector<std::regex> kPatterns = []{
        std::vector<std::regex> v;
        // C / C++: class/struct/enum/union/namespace/typedef declarations.
        v.emplace_back(R"(^\s*(?:class|struct|enum(?:\s+class)?|union|namespace)\s+(\w+))",
                       std::regex::optimize);
        // C / C++: typedef name (last identifier before ;)
        v.emplace_back(R"(^\s*typedef\s+.+?\s+(\w+)\s*;)", std::regex::optimize);
        // C / C++ function: very loose — return type then identifier(... at start of line.
        v.emplace_back(R"(^[\w:][\w\s\*\&<>:]*?\s(\w+)\s*\([^;]*?(?:\)\s*\{|\)\s*$))",
                       std::regex::optimize);
        // Python: def / class.
        v.emplace_back(R"(^\s*(?:def|class)\s+(\w+))", std::regex::optimize);
        // JS / TS: function / class / const / let / var / type / interface, optionally export.
        v.emplace_back(R"(^\s*(?:export\s+)?(?:async\s+)?(?:function|class|const|let|var|type|interface)\s+(\w+))",
                       std::regex::optimize);
        // Go: func (with or without receiver), type.
        v.emplace_back(R"(^func(?:\s+\([^)]*\))?\s+(\w+))", std::regex::optimize);
        v.emplace_back(R"(^type\s+(\w+))", std::regex::optimize);
        // Rust: fn / struct / enum / trait / type / mod / const / static.
        v.emplace_back(R"(^\s*(?:pub(?:\([^)]*\))?\s+)?(?:fn|struct|enum|trait|type|mod|const|static)\s+(\w+))",
                       std::regex::optimize);
        return v;
    }();
    return kPatterns;
}

bool scan_file(const fs::path& root, const fs::path& file,
               std::vector<SymbolEntry>& out, std::size_t cap) {
    std::error_code ec;
    auto sz = fs::file_size(file, ec);
    if (ec) return true;
    // Skip pathologically large files (minified bundles, generated
    // headers). 512 KiB matches find_definition's cap.
    if (sz > 512 * 1024) return true;
    std::ifstream in(file);
    if (!in) return true;

    std::error_code rec;
    auto rel = fs::relative(file, root, rec);
    std::string path_str = rec ? file.string() : rel.string();

    std::string line;
    int lineno = 0;
    const auto& pats = symbol_patterns();
    while (std::getline(in, line)) {
        ++lineno;
        // Skip comment-only lines for the regex pass — saves work and
        // avoids matching `// fn foo()` examples.
        std::string_view sv{line};
        // Find first non-whitespace.
        std::size_t i = 0;
        while (i < sv.size() && (sv[i] == ' ' || sv[i] == '\t')) ++i;
        if (i + 1 < sv.size() && sv[i] == '/' && sv[i+1] == '/') continue;
        if (i < sv.size() && sv[i] == '#' && !line.starts_with("#define")
            && !line.starts_with("#include")) {
            // Python uses '#'; keep scanning for python's def/class
            // (matched above by pat[3], which uses ^\s*).
        }
        std::smatch m;
        for (const auto& re : pats) {
            if (std::regex_search(line, m, re) && m.size() >= 2) {
                SymbolEntry e;
                e.name = m[1].str();
                e.path = path_str;
                e.line_number = lineno;
                out.push_back(std::move(e));
                if (out.size() >= cap) return false;
                break;  // One match per line is enough.
            }
        }
    }
    return true;
}

} // namespace

// ── Cache + async prewarm (mirrors files.cpp) ─────────────────────
// The symbol scan is the expensive one: std::regex across every line of up
// to 5000 source files. Running that on the `#` keystroke froze the UI for
// seconds on a large repo. Now the scan runs on a background thread pool
// at startup, files fanned out across cores, published behind a swap.
namespace {
std::mutex& sym_mu() { static std::mutex m; return m; }
std::shared_ptr<const std::vector<SymbolEntry>>& sym_cache() {
    static std::shared_ptr<const std::vector<SymbolEntry>> c; return c;
}
std::atomic<bool>& sym_building() { static std::atomic<bool> b{false}; return b; }

std::vector<SymbolEntry> build_symbol_list(std::size_t cap) {
    std::vector<SymbolEntry> out;
    const auto root = tools::util::project_root();
    if (root.empty()) return out;

    // 1. Collect candidate source files (cheap directory walk).
    std::vector<fs::path> src;
    {
        std::error_code ec;
        constexpr std::size_t kFileCap = 5000;
        for (auto it = fs::recursive_directory_iterator(
                 root, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator() && src.size() < kFileCap;
             it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            const auto& entry = *it;
            auto fn = entry.path().filename().string();
            const bool is_dir = entry.is_directory(ec);
            if (is_dir && tools::util::should_skip_dir(fn)) {
                it.disable_recursion_pending(); continue;
            }
            if (is_dir && it.depth() > 0 && fn.starts_with(".")) {
                it.disable_recursion_pending(); continue;
            }
            if (is_dir) continue;
            if (!entry.is_regular_file(ec)) continue;
            if (!is_source_ext(entry.path().extension().string())) continue;
            src.push_back(entry.path());
        }
    }
    if (src.empty()) return out;

    // 2. Scan files in parallel — each worker builds a local vector, then
    //    we merge. The regex pass is CPU-bound; fanning it out across cores
    //    turns a multi-second cold scan into a fraction of it.
    unsigned nthreads = std::thread::hardware_concurrency();
    if (nthreads == 0) nthreads = 4;
    nthreads = std::min<unsigned>(nthreads, 12);
    nthreads = std::min<unsigned>(nthreads, (unsigned)src.size());

    std::atomic<std::size_t> next{0};
    std::vector<std::vector<SymbolEntry>> partials(nthreads);
    {
        std::vector<std::jthread> pool;
        pool.reserve(nthreads);
        for (unsigned t = 0; t < nthreads; ++t) {
            pool.emplace_back([&, t] {
                auto& local = partials[t];
                for (;;) {
                    std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
                    if (i >= src.size()) return;
                    if (local.size() >= cap) return;
                    try { (void)scan_file(root, src[i], local, cap); }
                    catch (...) { /* skip a file that trips the matcher */ }
                }
            });
        }
    }
    for (auto& p : partials) {
        for (auto& e : p) {
            out.push_back(std::move(e));
            if (out.size() >= cap) break;
        }
        if (out.size() >= cap) break;
    }
    std::sort(out.begin(), out.end(),
              [](const SymbolEntry& a, const SymbolEntry& b){ return a.name < b.name; });
    return out;
}
} // namespace

void prewarm_workspace_symbols(std::size_t cap) {
    bool expected = false;
    if (!sym_building().compare_exchange_strong(expected, true)) return;
    {
        std::lock_guard<std::mutex> lk(sym_mu());
        if (sym_cache()) { sym_building() = false; return; }
    }
    std::thread([cap] {
        auto built = std::make_shared<std::vector<SymbolEntry>>(build_symbol_list(cap));
        {
            std::lock_guard<std::mutex> lk(sym_mu());
            sym_cache() = std::move(built);
        }
        sym_building() = false;
    }).detach();
}

bool symbols_ready() {
    std::lock_guard<std::mutex> lk(sym_mu());
    return static_cast<bool>(sym_cache());
}

const std::vector<SymbolEntry>& list_workspace_symbols(std::size_t cap) {
    {
        std::lock_guard<std::mutex> lk(sym_mu());
        if (auto c = sym_cache()) return *c;
    }
    // Cold synchronous caller: build inline + publish.
    auto built = std::make_shared<std::vector<SymbolEntry>>(build_symbol_list(cap));
    std::lock_guard<std::mutex> lk(sym_mu());
    if (!sym_cache()) sym_cache() = built;
    return *sym_cache();
}

std::vector<std::size_t>
filter_symbols(const std::vector<SymbolEntry>& entries, std::string_view query) {
    std::vector<std::size_t> matches;
    matches.reserve(entries.size());
    if (query.empty()) {
        for (std::size_t i = 0; i < entries.size(); ++i) matches.push_back(i);
        return matches;
    }
    auto needle = to_lower(query);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        auto hay = to_lower(entries[i].name);
        if (hay.find(needle) != std::string::npos) matches.push_back(i);
    }
    return matches;
}

} // namespace agentty
