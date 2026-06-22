#include "agentty/tool/spec.hpp"
#include "agentty/tool/tools.hpp"
#include "agentty/tool/util/arg_reader.hpp"
#include "agentty/tool/util/fs_helpers.hpp"
#include "agentty/tool/util/subprocess.hpp"
#include "agentty/tool/util/tool_args.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

namespace agentty::tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct FindDefinitionArgs {
    std::string symbol;
    std::string root;
    std::string display_description;
};

std::expected<FindDefinitionArgs, ToolError> parse_find_definition_args(const json& j) {
    util::ArgReader ar(j);
    auto sym_opt = ar.require_str("symbol");
    if (!sym_opt)
        return std::unexpected(ToolError::invalid_args("symbol required"));
    return FindDefinitionArgs{
        *std::move(sym_opt),
        ar.str("path", "."),
        ar.str("display_description", ""),
    };
}

ExecResult run_find_definition(const FindDefinitionArgs& a) {
    auto wp = util::make_workspace_path_checked(a.root, "find_definition");
    if (!wp) return std::unexpected(std::move(wp.error()));

    // Regex-escape the symbol for both ripgrep and std::regex.
    std::string esc;
    esc.reserve(a.symbol.size() * 2);
    for (char c : a.symbol) {
        switch (c) {
            case '.': case '*': case '+': case '?': case '(': case ')':
            case '[': case ']': case '{': case '}': case '|': case '^':
            case '$': case '\\':
                esc.push_back('\\'); [[fallthrough]];
            default:
                esc.push_back(c);
        }
    }

    // ── Ripgrep fast path (10-100x faster than std::regex walk) ─────────
    // Combined pattern covers C/C++/Python/JS/TS/Go/Rust definitions.
    // ripgrep's PCRE2 handles the alternation efficiently.
    std::string rg_pattern =
        "\\b(class|struct|enum|union|namespace|typedef|using|def|function|"
        "const|let|var|type|interface|export|func|fn|trait|mod|static)\\s+"
        + esc + "\\b|#define\\s+" + esc + "\\b|\\b\\w[\\w:*&<> ]*\\s+" + esc + "\\s*\\(";

    // Check if ripgrep is available (cached after first check).
    static int rg_available = -1;  // -1 = unknown, 0 = no, 1 = yes
    if (rg_available < 0) {
        auto r = util::run_command_s("rg --version", 5000, std::chrono::seconds{2});
        rg_available = (r.started && r.exit_code == 0) ? 1 : 0;
    }

    if (rg_available == 1) {
        // Build ripgrep command. -n = line numbers, -H = filenames,
        // --no-heading = grep-style output, -M 500 = max line length,
        // -m 50 = max matches, --type-add for code extensions.
        std::string cmd = "rg -n -H --no-heading -M 500 -m 50 "
            "--type-add 'code:*.{cpp,hpp,c,h,cc,hh,cxx,hxx,py,js,ts,jsx,tsx,go,rs,java,kt,rb,swift,zig,lua}' "
            "-t code -e '" + rg_pattern + "' '" + wp->path().string() + "'";
        auto r = util::run_command_s(cmd, 100000, std::chrono::seconds{30});
        if (r.started && (r.exit_code == 0 || r.exit_code == 1)) {
            // exit_code 1 = no matches (normal for rg)
            if (r.output.empty() || r.exit_code == 1) {
                return ToolOutput{"no definitions found for '" + a.symbol + "'", std::nullopt};
            }
            std::string body = r.output;
            // Trim trailing newline for cleaner output.
            while (!body.empty() && body.back() == '\n') body.pop_back();
            if (!a.display_description.empty())
                body = a.display_description + "\n" + body;
            return ToolOutput{std::move(body), std::nullopt};
        }
        // ripgrep failed (maybe bad regex) — fall through to std::regex.
    }

    // ── Fallback: std::regex walk ────────────────────────────────────────
    std::vector<std::regex> patterns;
    try {
        // C/C++
        patterns.emplace_back("\\b(class|struct|enum|union|namespace|typedef|using)\\s+" + esc + "\\b");
        patterns.emplace_back("\\b\\w[\\w:*&<> ]*\\s+" + esc + "\\s*\\(");
        patterns.emplace_back("#define\\s+" + esc + "\\b");
        // Python
        patterns.emplace_back("\\b(def|class)\\s+" + esc + "\\s*[\\(:]");
        // JS/TS
        patterns.emplace_back("\\b(function|const|let|var|type|interface|export)\\s+" + esc + "\\b");
        // Go
        patterns.emplace_back("\\b(func|type)\\s+" + esc + "\\b");
        // Rust
        patterns.emplace_back("\\b(fn|struct|enum|trait|type|mod|const|static)\\s+" + esc + "\\b");
    } catch (...) {
        return std::unexpected(ToolError::invalid_regex("invalid symbol name for regex"));
    }

    std::ostringstream out;
    int matches = 0;
    std::error_code ec;
    // File size cap. Catches generated bundles (minified JS, single-header
    // libraries dumped as one giant .h) that would otherwise pin a worker
    // running 7 regex passes over a multi-MB body.
    constexpr uintmax_t kMaxFileBytes = 512u * 1024u;   // 512 KiB
    for (auto it = fs::recursive_directory_iterator(wp->path(),
                fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const auto& entry = *it;
        auto fn = entry.path().filename().string();
        const bool is_dir = entry.is_directory(ec);

        // Stop descending into known-noisy directories at every depth.
        // The previous local skip_dirs list missed `_deps`, `.venv`,
        // `cmake-build-debug/release`, `third_party`, etc. — running
        // find_definition from a cmake build/ directory walked into
        // _deps/{nlohmann,simdjson,gtest,...} and chewed through tens
        // of thousands of vendored source files before the watchdog
        // fired. Use the canonical fs_helpers list so this stays in
        // sync with grep / list_dir.
        if (is_dir && util::should_skip_dir(fn)) {
            it.disable_recursion_pending();
            continue;
        }
        // Hidden entries: hidden DIRs are skipped (no descent, no scan);
        // hidden FILEs (e.g. .clang-format) we don't index either.
        if (fn.starts_with(".")) {
            if (is_dir) it.disable_recursion_pending();
            continue;
        }
        if (is_dir) continue;
        if (!entry.is_regular_file(ec)) continue;
        auto ext = entry.path().extension().string();
        static const std::vector<std::string> code_exts = {
            ".cpp", ".hpp", ".c", ".h", ".cc", ".hh", ".cxx", ".hxx",
            ".py", ".js", ".ts", ".jsx", ".tsx", ".go", ".rs",
            ".java", ".kt", ".rb", ".swift", ".zig", ".lua",
        };
        bool is_code = false;
        for (const auto& e : code_exts) { if (ext == e) { is_code = true; break; } }
        if (!is_code) continue;
        // Size cap. One stat per candidate; cheap vs the per-line regex
        // scan we'd otherwise do.
        std::error_code sec;
        auto sz = entry.file_size(sec);
        if (!sec && (sz == 0 || sz > kMaxFileBytes)) continue;

        std::ifstream ifs(entry.path());
        if (!ifs) continue;
        std::string line;
        int n = 1;
        while (std::getline(ifs, line)) {
            for (const auto& re : patterns) {
                if (std::regex_search(line, re)) {
                    out << entry.path().string() << ":" << n << ": " << line << "\n";
                    if (++matches > 50) goto done;
                    break;
                }
            }
            n++;
        }
    }
    done:
    if (matches == 0) return ToolOutput{"no definitions found for '" + a.symbol + "'", std::nullopt};
    if (matches > 50) out << "[>50 definitions, truncated]\n";
    std::string body = out.str();
    if (!a.display_description.empty())
        body = a.display_description + "\n" + body;
    return ToolOutput{std::move(body), std::nullopt};
}

} // namespace

ToolDef tool_find_definition() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"find_definition">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description = "Find the definition of a symbol (function, class, struct, enum, type) "
                    "across the codebase. Searches for common definition patterns in C/C++, "
                    "Python, JavaScript/TypeScript, Go, and Rust.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"symbol"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"symbol", {{"type","string"}, {"description","The symbol name to find"}}},
            {"path",   {{"type","string"}, {"description","Directory to search (default: cwd)"}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<FindDefinitionArgs>(parse_find_definition_args, run_find_definition);
    return t;
}

} // namespace agentty::tools
