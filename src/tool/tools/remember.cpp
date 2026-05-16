// agentty::tools::tool_remember — persist a fact the user wants the
// agent to recall on future turns. Writes one JSONL record to either
// ~/.agentty/memory.jsonl (scope=user, cross-project) or
// <workspace>/.agentty/memory.jsonl (scope=project). The system-prompt
// builder reloads the tail of these files into <learned-memory> on
// every turn, so the model gets to see what it stored.
//
// Why this exists as a tool, not a CLAUDE.md edit: CLAUDE.md is
// human-curated and the model should respect it as authoritative. The
// JSONL memory store is the model's own scratchpad — additive,
// removable, bounded in size — for facts the *user* asked it to
// remember mid-conversation. Keeping the two stores separate avoids
// the model accidentally mutating files the human owns.

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

struct RememberArgs {
    std::string text;
    memory::Scope scope;
};

std::expected<RememberArgs, ToolError> parse_remember_args(const json& j) {
    util::ArgReader ar(j);
    auto text = ar.require_str("text");
    if (!text) {
        return std::unexpected(ToolError::invalid_args(
            "remember: `text` is required (the fact to remember, one short sentence)"));
    }
    RememberArgs out;
    out.text = std::move(*text);
    // `scope` is optional and defaults to project — the safer default;
    // global cross-project facts should be opt-in. Tolerate the common
    // synonym "global" for user-scope since some models prefer it.
    auto raw = ar.str("scope", "project");
    if (raw == "global" || raw == "all") raw = "user";
    auto parsed = memory::parse_scope(raw);
    if (!parsed) {
        return std::unexpected(ToolError::invalid_args(
            "remember: `scope` must be \"user\" or \"project\" (got: " + raw + ")"));
    }
    out.scope = *parsed;
    return out;
}

ExecResult run_remember(const RememberArgs& a) {
    auto res = memory::append(a.scope, a.text);
    if (!res.error.empty()) return std::unexpected(ToolError::io(res.error));
    std::string msg = std::format(
        "Remembered ({} scope, id={}): {}",
        memory::to_string(a.scope), res.id, a.text);
    if (!res.note.empty()) msg += "  [" + res.note + "]";
    if (res.rolled > 0) {
        msg += std::format("  [rolled {} oldest record(s) to fit cap]", res.rolled);
    }
    return ToolOutput{std::move(msg), std::nullopt};
}

} // namespace

ToolDef tool_remember() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"remember">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description =
        "Persist a fact for future turns. Call this whenever the user asks "
        "you to remember something (\"remember X\", \"don't forget Y\", "
        "\"from now on Z\", \"always do W\"). The fact is written to a "
        "JSONL store and reloaded into your system prompt on every "
        "subsequent turn under <learned-memory>.\n\n"
        "scope=\"project\" (default) — stored at <workspace>/.agentty/"
        "memory.jsonl, only visible when running in this codebase.\n"
        "scope=\"user\"               — stored at ~/.agentty/memory.jsonl, "
        "visible across every project (use for facts about the user "
        "themselves, not the project).\n\n"
        "Keep each fact short and self-contained. If a previously-stored "
        "fact becomes wrong, use `forget` to remove it.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"text"}},
        {"properties", {
            {"text",  {{"type","string"},
                {"description","The fact to remember. One short sentence; "
                               "future-you reads this back without any of the "
                               "surrounding conversation."}}},
            {"scope", {{"type","string"},
                {"enum", {"user","project"}},
                {"description","\"project\" (default) is workspace-scoped; "
                               "\"user\" is cross-project (for user "
                               "preferences, identity, etc)."}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<RememberArgs>(parse_remember_args, run_remember);
    return t;
}

} // namespace agentty::tools
