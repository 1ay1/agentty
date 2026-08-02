// tool_body_preview — `todo` tool body.
//
// Structured checkbox list (TodoList owns the icons). Returns false when
// the args don't carry a non-empty `todos` array, letting the dispatcher
// leave Kind::None.
//
// NOTE on ordering: in the dispatcher this is checked AFTER the shared
// Failure fallback, so a FAILED todo renders as a plain error body, not a
// checkbox list — matching the original monolithic control flow.

#include "tool_body_common.hpp"

#include <string>
#include <utility>

namespace agentty::ui::detail {

bool todo_body(const ToolUse& tc, maya::ToolBodyPreview::Config& out) {
    using Kind = maya::ToolBodyPreview::Kind;
    if (!tc.args.is_object()) return false;
    auto it = tc.args.find("todos");
    if (it == tc.args.end() || !it->is_array() || it->empty()) return false;

    using Status = maya::ToolBodyPreview::TodoItem::Status;
    out.kind = Kind::TodoList;
    out.todos.reserve(it->size());
    for (const auto& td : *it) {
        if (!td.is_object()) continue;
        auto string_field = [&](const char* key) -> std::string {
            auto field = td.find(key);
            return field != td.end() && field->is_string()
                ? field->get<std::string>() : std::string{};
        };
        auto content = string_field("content");
        if (content.empty()) continue;
        Status s = Status::Pending;
        auto st = string_field("status");
        if      (st == "completed")   s = Status::Completed;
        else if (st == "in_progress") s = Status::InProgress;
        out.todos.push_back({std::move(content), s});
    }
    return true;
}

} // namespace agentty::ui::detail
