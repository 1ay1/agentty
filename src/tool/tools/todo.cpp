#include "moha/tool/tools.hpp"
#include "moha/tool/util/arg_reader.hpp"
#include "moha/tool/util/tool_args.hpp"

#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace moha::tools {

using json = nlohmann::json;

namespace {

enum class TodoStatus { Pending, InProgress, Completed };

struct TodoItem {
    std::string content;
    TodoStatus status;
};

struct TodoArgs {
    std::vector<TodoItem> todos;
};

TodoStatus parse_status(std::string_view s) {
    if (s == "completed")    return TodoStatus::Completed;
    if (s == "in_progress")  return TodoStatus::InProgress;
    return TodoStatus::Pending;
}

std::expected<TodoArgs, ToolError> parse_todo_args(const json& j) {
    util::ArgReader ar(j);
    TodoArgs out;
    const json* raw = ar.raw("todos");
    if (!raw || !raw->is_array()) return out;  // tolerate missing/invalid
    out.todos.reserve(raw->size());
    for (const auto& td : *raw) {
        if (!td.is_object()) continue;
        util::ArgReader inner(td);
        out.todos.push_back(TodoItem{
            inner.str("content", ""),
            parse_status(inner.str("status", "pending")),
        });
    }
    return out;
}

ExecResult run_todo(const TodoArgs& a) {
    std::ostringstream out;
    for (const auto& td : a.todos) {
        char mark = td.status == TodoStatus::Completed   ? 'x'
                  : td.status == TodoStatus::InProgress  ? '-'
                                                         : ' ';
        out << "[" << mark << "] " << td.content << "\n";
    }
    return ToolOutput{out.str(), std::nullopt};
}

} // namespace

ToolDef tool_todo() {
    ToolDef t;
    t.name = ToolName{std::string{"todo"}};
    t.description = "Maintain the session todo list. Overwrites with the provided list.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"todos"}},
        {"properties", {
            {"todos", {{"type","array"},
                {"items", {{"type","object"},
                    {"properties", {
                        {"content", {{"type","string"}}},
                        {"status",  {{"type","string"},
                            {"enum", {"pending","in_progress","completed"}}}},
                    }},
                    {"required", {"content","status"}},
                }},
            }},
        }},
    };
    t.needs_permission = [](Profile){ return false; };
    t.execute = util::adapt<TodoArgs>(parse_todo_args, run_todo);
    return t;
}

} // namespace moha::tools
