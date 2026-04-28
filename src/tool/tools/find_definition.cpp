#include "moha/tool/spec.hpp"
#include "moha/tool/tools.hpp"
#include "moha/tool/util/arg_reader.hpp"
#include "moha/tool/util/fs_helpers.hpp"
#include "moha/tool/util/tool_args.hpp"

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

namespace moha::tools {

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
    // Regex-escape the symbol. Operator names (`operator*`, `operator<<`)
    // and templated forms otherwise explode the regex parser.
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

    auto wp = util::make_workspace_path(a.root, "find_definition");
    if (!wp) return std::unexpected(std::move(wp.error()));

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
        body = a.display_description + "\n\n" + body;
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

} // namespace moha::tools
