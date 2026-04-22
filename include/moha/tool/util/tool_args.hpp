#pragma once
// Uniform plumbing for the "parse json -> typed Args -> run(Args)" pattern
// every tool follows. Each tool file defines:
//
//   struct FooArgs { ... fields ... };
//   [[nodiscard]] std::expected<FooArgs, ToolError> parse_foo_args(const json&);
//   [[nodiscard]] ExecResult run_foo(const FooArgs&);
//
//   ToolDef tool_foo() {
//       ToolDef t; /* name, description, schema, needs_permission */
//       t.execute = adapt<FooArgs>(parse_foo_args, run_foo);
//       return t;
//   }
//
// `adapt` is the only place the raw `const json&` surface leaks into tool
// dispatch — every tool body below it sees a typed struct.

#include "moha/tool/registry.hpp"

#include <expected>
#include <functional>
#include <utility>

#include <nlohmann/json.hpp>

namespace moha::tools::util {

template <typename Args>
[[nodiscard]] auto adapt(
        std::expected<Args, ToolError> (*parse)(const nlohmann::json&),
        ExecResult (*run)(const Args&))
    -> std::function<ExecResult(const nlohmann::json&)>
{
    return [parse, run](const nlohmann::json& j) -> ExecResult {
        auto parsed = parse(j);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        return run(*parsed);
    };
}

} // namespace moha::tools::util
