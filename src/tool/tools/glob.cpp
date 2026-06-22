#include "agentty/tool/spec.hpp"
#include "agentty/tool/tools.hpp"
#include "agentty/tool/util/arg_reader.hpp"
#include "agentty/tool/util/fs_helpers.hpp"
#include "agentty/tool/util/glob.hpp"
#include "agentty/tool/util/tool_args.hpp"
#include "agentty/domain/refined.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

namespace agentty::tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct GlobArgs {
    // pattern: non-blank by construction. A whitespace-only or empty
    // glob would either match nothing or match everything depending on
    // the substring fallback — either way useless.
    domain::NonBlank<std::string> pattern;
    std::string                   root;
    std::string                   display_description;
};

std::expected<GlobArgs, ToolError> parse_glob_args(const json& j) {
    util::ArgReader ar(j);
    auto pat_opt = ar.require_str("pattern");
    if (!pat_opt)
        return std::unexpected(ToolError::invalid_args("pattern required"));
    auto refined_pat = domain::NonBlank<std::string>::try_make(*std::move(pat_opt));
    if (!refined_pat)
        return std::unexpected(ToolError::invalid_args(std::format(
            "pattern {} (received only whitespace)",
            refined_pat.error().what)));
    return GlobArgs{
        *std::move(refined_pat),
        ar.str("path", "."),
        ar.str("display_description", ""),
    };
}

ExecResult run_glob(const GlobArgs& a) {
    // Workspace boundary check — even with default ".", canonicalising
    // it ensures glob can't be tricked into walking up via "../.."
    // tricks the model might try.
    auto wp = util::make_workspace_path_checked(a.root, "glob");
    if (!wp) return std::unexpected(std::move(wp.error()));

    // If the pattern has no glob metacharacters, fall back to substring
    // matching. The model often types `foo.cpp` intending "find anything
    // named that"; forcing it to write `*foo.cpp*` would be annoying.
    const auto& pat = a.pattern.value();
    bool has_glob = pat.find_first_of("*?[") != std::string::npos;

    struct Entry {
        std::string path;
        bool is_dir;
        bool is_link;
        uintmax_t size;
    };
    std::vector<Entry> entries;
    entries.reserve(512);

    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(wp->path(),
                fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        auto fn = it->path().filename().string();
        bool is_dir_entry = it->is_directory(ec);
        if (is_dir_entry) {
            if (util::should_skip_dir(fn)) { it.disable_recursion_pending(); continue; }
        }
        bool hit = has_glob ? util::glob_match(pat, fn)
                            : fn.find(pat) != std::string::npos;
        if (hit) {
            bool is_link = it->is_symlink(ec);
            uintmax_t sz = 0;
            if (!is_dir_entry && !is_link) {
                std::error_code sec;
                sz = it->file_size(sec);
            }
            entries.push_back({it->path().string(), is_dir_entry, is_link, sz});
            if (entries.size() > 500) break;
        }
    }

    if (entries.empty())
        return ToolOutput{"no matches. Try a different pattern, or `list_dir` "
                          "on parent directories to see what exists.",
                          std::nullopt};

    // Sort: directories first, then by path.
    std::sort(entries.begin(), entries.end(), [](const Entry& x, const Entry& y) {
        if (x.is_dir != y.is_dir) return x.is_dir > y.is_dir;
        return x.path < y.path;
    });

    auto format_size = [](uintmax_t bytes) -> std::string {
        char buf[16];
        if (bytes < 1024) { std::snprintf(buf, sizeof(buf), "%juB", bytes); return buf; }
        if (bytes < 1024*1024) { std::snprintf(buf, sizeof(buf), "%.1fK", bytes/1024.0); return buf; }
        std::snprintf(buf, sizeof(buf), "%.1fM", bytes/(1024.0*1024.0)); return buf;
    };

    std::ostringstream out;
    for (const auto& e : entries) {
        out << e.path;
        if (e.is_dir) out << "/";
        else if (e.is_link) out << "@";
        else if (e.size > 0) out << "  " << format_size(e.size);
        out << "\n";
    }

    std::string body = "Found " + std::to_string(entries.size()) + " file(s):\n" + out.str();
    if (entries.size() > 500) body += "[>500, truncated]\n";
    if (!a.display_description.empty())
        body = a.display_description + "\n" + body;
    return ToolOutput{std::move(body), std::nullopt};
}

} // namespace

ToolDef tool_glob() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"glob">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description = "Find files by glob pattern. Supports `*` (any run), `?` (one char), "
                    "`[abc]` classes, and bare substrings. Matches against filename "
                    "(not full path). Case-insensitive on Windows.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"pattern"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"pattern", {{"type","string"}, {"description","Glob pattern, e.g. *.cpp"}}},
            {"path",    {{"type","string"}, {"description","Root directory (default: cwd)"}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<GlobArgs>(parse_glob_args, run_glob);
    return t;
}

} // namespace agentty::tools
