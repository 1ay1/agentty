#include "moha/tool/spec.hpp"
#include "moha/tool/tools.hpp"
#include "moha/tool/util/arg_reader.hpp"
#include "moha/tool/util/fs_helpers.hpp"
#include "moha/tool/util/glob.hpp"
#include "moha/tool/util/tool_args.hpp"

#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

namespace moha::tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct GlobArgs {
    std::string pattern;
    std::string root;
    std::string display_description;
};

std::expected<GlobArgs, ToolError> parse_glob_args(const json& j) {
    util::ArgReader ar(j);
    auto pat_opt = ar.require_str("pattern");
    if (!pat_opt)
        return std::unexpected(ToolError::invalid_args("pattern required"));
    return GlobArgs{
        *std::move(pat_opt),
        ar.str("path", "."),
        ar.str("display_description", ""),
    };
}

ExecResult run_glob(const GlobArgs& a) {
    // If the pattern has no glob metacharacters, fall back to substring
    // matching. The model often types `foo.cpp` intending "find anything
    // named that"; forcing it to write `*foo.cpp*` would be annoying.
    bool has_glob = a.pattern.find_first_of("*?[") != std::string::npos;

    std::ostringstream out;
    int n = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(a.root,
                fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        auto fn = it->path().filename().string();
        if (it->is_directory(ec)) {
            if (util::should_skip_dir(fn)) { it.disable_recursion_pending(); continue; }
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        bool hit = has_glob ? util::glob_match(a.pattern, fn)
                            : fn.find(a.pattern) != std::string::npos;
        if (hit) {
            out << it->path().string() << "\n";
            if (++n > 500) { out << "[>500, truncated]\n"; break; }
        }
    }
    if (n == 0)
        return ToolOutput{"no matches. Try a different pattern, or `list_dir` "
                          "on parent directories to see what exists.",
                          std::nullopt};
    std::string body = "Found " + std::to_string(n) + " file(s):\n" + out.str();
    if (!a.display_description.empty())
        body = a.display_description + "\n\n" + body;
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

} // namespace moha::tools
