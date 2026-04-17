#pragma once

#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "moha/model.hpp"

namespace moha::tools {

// ── Tool result types (std::expected-based) ──────────────────────────────

struct ToolOutput {
    std::string text;
    std::optional<FileChange> change;
};

struct ToolError {
    std::string message;
};

using ExecResult = std::expected<ToolOutput, ToolError>;

// ── Tool definition ──────────────────────────────────────────────────────

struct ToolDef {
    ToolName    name;
    std::string description;
    nlohmann::json input_schema;

    std::function<bool(Profile)> needs_permission;
    std::function<ExecResult(const nlohmann::json& args)> execute;
};

[[nodiscard]] const std::vector<ToolDef>& registry();
[[nodiscard]] const ToolDef* find(std::string_view name);

} // namespace moha::tools
