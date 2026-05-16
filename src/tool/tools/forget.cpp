// agentty::tools::tool_forget — counterpart to `remember`. Removes
// records from the JSONL memory stores so the model stops seeing them
// in subsequent <learned-memory> blocks. Two addressing modes:
//
//   { id: "a1b2c3d4" }          — exact record id (shown as `[id]`
//                                  prefix in <learned-memory>)
//   { substring: "fish shell" } — case-sensitive substring match
//                                  against the stored text
//
// Either mode scans both scopes (user + project). At most one of `id`
// / `substring` should be provided; if both are present, `id` wins.
// Refuses to run on an empty pattern so a stray `forget {}` can't nuke
// the whole store.

#include "agentty/tool/memory_store.hpp"
#include "agentty/tool/spec.hpp"
#include "agentty/tool/tools.hpp"
#include "agentty/tool/util/arg_reader.hpp"
#include "agentty/tool/util/tool_args.hpp"

#include <format>
#include <string>

#include <nlohmann/json.hpp>

namespace agentty::tools {

using json = nlohmann::json;

namespace {

struct ForgetArgs {
    std::string id;          // empty if not provided
    std::string substring;   // empty if not provided
};

std::expected<ForgetArgs, ToolError> parse_forget_args(const json& j) {
    util::ArgReader ar(j);
    ForgetArgs out;
    out.id        = ar.str("id", "");
    out.substring = ar.str("substring", "");
    if (out.id.empty() && out.substring.empty()) {
        return std::unexpected(ToolError::invalid_args(
            "forget: provide either `id` (8-char hex from a <learned-memory> "
            "line) or `substring` (text contained in the fact to remove)"));
    }
    return out;
}

ExecResult run_forget(const ForgetArgs& a) {
    std::size_t removed = 0;
    std::string by;
    if (!a.id.empty()) {
        removed = memory::forget_by_id(a.id);
        by = "id=" + a.id;
    } else {
        removed = memory::forget_by_substring(a.substring);
        by = "substring=" + a.substring;
    }
    if (removed == 0) {
        return ToolOutput{
            std::format("No memory matched ({}). Nothing to forget.", by),
            std::nullopt};
    }
    return ToolOutput{
        std::format("Forgot {} record(s) ({}).", removed, by),
        std::nullopt};
}

} // namespace

ToolDef tool_forget() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"forget">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description =
        "Remove a previously-remembered fact so it stops appearing in "
        "<learned-memory> on future turns. Provide either:\n"
        "  - `id`        — the 8-char hex shown as `[id]` prefix in "
        "<learned-memory>. Removes exactly that record.\n"
        "  - `substring` — text contained in the fact. Removes every "
        "record whose stored text contains this substring "
        "(case-sensitive). Use a long enough substring to be specific.\n"
        "Scans both user and project scopes; if the same fact exists in "
        "both, both are removed. Returns the count removed.";
    t.input_schema = json{
        {"type","object"},
        {"properties", {
            {"id",        {{"type","string"},
                {"description","Exact 8-char hex record id (from "
                               "<learned-memory> in your system prompt)."}}},
            {"substring", {{"type","string"},
                {"description","Substring of the stored text. "
                               "Case-sensitive. Refused if empty."}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<ForgetArgs>(parse_forget_args, run_forget);
    return t;
}

} // namespace agentty::tools
