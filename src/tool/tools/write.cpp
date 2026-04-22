#include "moha/tool/tools.hpp"
#include "moha/tool/util/arg_reader.hpp"
#include "moha/tool/util/fs_helpers.hpp"
#include "moha/tool/util/tool_args.hpp"
#include "moha/io/diff.hpp"

#include <filesystem>
#include <format>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

namespace moha::tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct WriteArgs {
    util::NormalizedPath path;
    std::string content;
    std::string display_description;
    std::string coercion_note;  // non-empty when `content` was coerced from non-string
};

std::expected<WriteArgs, ToolError> parse_write_args(const json& j) {
    util::ArgReader ar(j);
    auto raw = ar.require_str("path");
    if (!raw)
        return std::unexpected(ToolError::invalid_args("path required"));
    // `content` is required. Silently writing an empty file when the field is
    // missing (previous behavior) was the root of "write succeeds but the
    // file is empty on disk" — models dropping the arg for any reason,
    // partial/cut SSE streams, or salvage_args producing path-only objects
    // all ended up with a zero-byte file and a cheerful green "Written" card.
    // Fail loudly instead; the model can retry with content.
    if (!ar.has("content"))
        return std::unexpected(ToolError::invalid_args(
            "content required (got path but no content — nothing written). "
            "Re-run with the full file body in the `content` field."));
    std::string note;
    std::string content = ar.str("content", "", &note);
    return WriteArgs{
        util::NormalizedPath{std::move(*raw)},
        std::move(content),
        ar.str("display_description", ""),
        std::move(note),
    };
}

ExecResult run_write(const WriteArgs& a) {
    const auto& p = a.path.path();
    std::string original;
    std::error_code ec;
    bool exists = fs::exists(p, ec);
    if (exists) {
        if (!fs::is_regular_file(p, ec))
            return std::unexpected(ToolError::not_a_file("not a regular file: " + a.path.string()));
        original = util::read_file(p);
    }
    // No-op short-circuit: an identical rewrite is often the model
    // "confirming" a file state it already reached. Skipping the fs
    // touch avoids spurious mtime bumps that break incremental builds.
    if (exists && original == a.content)
        return ToolOutput{"File already matches content — no changes written.",
                          std::nullopt};
    auto change = diff::compute(a.path.string(), original, a.content);
    if (auto err = util::write_file(p, a.content); !err.empty())
        return std::unexpected(ToolError::io(err));
    std::string prefix;
    if (!a.display_description.empty())
        prefix = a.display_description + "\n\n";
    auto msg = std::format("{}{} {} ({}+ {}-){}",
                           prefix,
                           exists ? "Overwrote" : "Created",
                           a.path.string(), change.added, change.removed,
                           a.coercion_note);
    return ToolOutput{std::move(msg), std::move(change)};
}

} // namespace

ToolDef tool_write() {
    ToolDef t;
    t.name = ToolName{std::string{"write"}};
    t.description = "Write (or overwrite) a file with the given contents. "
                    "Creates parent directories as needed. Include a brief "
                    "`display_description` so the user sees what's being "
                    "written — e.g. 'Generate nerd-themed landing page'.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"path","content"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI while the "
                               "file streams. Optional but strongly recommended."}}},
            {"path",    {{"type","string"}}},
            {"content", {{"type","string"}}},
        }},
    };
    t.needs_permission = [](Profile p){ return p != Profile::Write; };
    t.execute = util::adapt<WriteArgs>(parse_write_args, run_write);
    return t;
}

} // namespace moha::tools
