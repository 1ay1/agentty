#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "moha/model.hpp"

namespace moha::tools {

struct ExecResult {
    std::string output;
    bool error = false;
    // For edit/write tools, a FileChange summarizing the proposed diff.
    std::optional<FileChange> change;
};

struct ToolDef {
    std::string name;
    std::string description;
    nlohmann::json input_schema;
    // Returns true if this tool requires permission under the given profile.
    std::function<bool(Profile)> needs_permission;
    // Executes the tool synchronously. Called from a worker thread.
    std::function<ExecResult(const nlohmann::json& args)> execute;
};

const std::vector<ToolDef>& registry();
const ToolDef* find(std::string_view name);

// Individual tool factories (defined in src/tools/*.cpp)
ToolDef make_read_tool();
ToolDef make_write_tool();
ToolDef make_edit_tool();
ToolDef make_bash_tool();
ToolDef make_grep_tool();
ToolDef make_glob_tool();
ToolDef make_todo_tool();

} // namespace moha::tools
