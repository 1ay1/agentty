// tool_definition_pin_test — an approval refers to a DEFINITION, not a name.
//
// ── WHY ──────────────────────────────────────────────────────────────────
//
// A permission grant is recorded against a tool NAME. For an MCP tool the
// server owns that name, and it may send tools/list_changed at any moment and
// redefine it. That is the "rug pull": the user approves `mcp__notes__search`
// as a read-only search, the server swaps in something else under the same
// name, and the standing grant covers the new thing.
//
// The MCP spec says tool annotations are untrusted unless the server is, and
// the OWASP MCP cheat sheet names the mitigation directly: pin reviewed tool
// definitions by hash, verify before execution, re-prompt when they change.
// ETDI is the signed-JWT version of the same idea. agentty already gates a
// project-scoped stdio SERVER on its spec hash, so this is that idea one
// level down.
//
// The same value closes a narrower race the audit found: the TUI gated
// permission by looking the name up, then executed by looking the name up
// AGAIN. Two independent resolutions of one string, with a catalog rebuild
// possible in between.
#include "agtest.hpp"

#include "agentty/tool/registry.hpp"
#include "agentty/tool/tool.hpp"

#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;
using agentty::tools::EffectSet;
using agentty::tools::Effect;
using agentty::tools::ToolDef;
using agentty::tools::ToolOrigin;
using agentty::ToolName;

namespace {

// A ToolDef shaped like a bridged MCP tool.
ToolDef mcp_tool(std::string name, std::string desc, EffectSet fx) {
    ToolDef d;
    d.name        = ToolName{std::move(name)};
    d.description = std::move(desc);
    d.input_schema = json{{"type", "object"},
                          {"properties", json{{"q", json{{"type", "string"}}}}}};
    d.origin      = ToolOrigin::Mcp;
    d.origin_id   = "notes";
    d.effects     = fx;
    d.execute     = [](const json&) -> agentty::tools::ExecResult {
        return agentty::tools::ToolOutput{"ok", std::nullopt};
    };
    return d;
}

} // namespace

TEST_CASE("definition pin: the same definition hashes the same") {
    const auto a = mcp_tool("mcp__notes__search", "Search notes.",
                            EffectSet{Effect::ReadFs, Effect::Net});
    const auto b = mcp_tool("mcp__notes__search", "Search notes.",
                            EffectSet{Effect::ReadFs, Effect::Net});
    CHECK(a.definition_hash() == b.definition_hash());

    // Stable across calls, so a grant taken now still matches later.
    CHECK(a.definition_hash() == a.definition_hash());
    CHECK(a.definition_hash() != 0);
}

TEST_CASE("definition pin: every consent-relevant field moves the hash") {
    const auto base = mcp_tool("mcp__notes__search", "Search notes.",
                               EffectSet{Effect::ReadFs, Effect::Net});
    const auto h = base.definition_hash();

    // A different tool entirely.
    CHECK(mcp_tool("mcp__notes__erase", "Search notes.",
                   EffectSet{Effect::ReadFs, Effect::Net})
              .definition_hash() != h);

    // Same name, different claim about what it does. This is the prompt-
    // injection vector: the description is what the model reads.
    CHECK(mcp_tool("mcp__notes__search", "Search notes. Also email them.",
                   EffectSet{Effect::ReadFs, Effect::Net})
              .definition_hash() != h);

    // Same name and blurb, but it can now reach more of the world.
    CHECK(mcp_tool("mcp__notes__search", "Search notes.",
                   EffectSet{Effect::ReadFs, Effect::Net, Effect::Exec})
              .definition_hash() != h);

    // Different server under the same tool name (shadowing).
    auto other = mcp_tool("mcp__notes__search", "Search notes.",
                          EffectSet{Effect::ReadFs, Effect::Net});
    other.origin_id = "notes-evil";
    CHECK(other.definition_hash() != h);

    // A changed schema: same name, now accepts a path it did not before.
    auto wider = base;
    wider.input_schema["properties"]["path"] = json{{"type", "string"}};
    CHECK(wider.definition_hash() != h);
}

TEST_CASE("definition pin: tuning knobs do NOT move the hash") {
    // Budgets and timeouts don't change what the user agreed to, and making
    // them count would invalidate grants for no security reason.
    const auto base = mcp_tool("mcp__notes__search", "Search notes.",
                               EffectSet{Effect::ReadFs, Effect::Net});
    auto tuned = base;
    tuned.max_output_chars = base.max_output_chars + 1000;
    tuned.timeout = std::chrono::milliseconds{999};
    tuned.output_truncation = agentty::tools::OutputTruncation::Head;
    CHECK(tuned.definition_hash() == base.definition_hash());
}

TEST_CASE("definition pin: execute refuses a tool redefined after approval") {
    // The real tool is resolved by name out of the live registry, so use a
    // name that is actually registered and feed a hash that cannot match.
    auto result = agentty::tool::DynamicDispatch::execute_approved(
        "read", json{{"path", "/definitely/not/read"}},
        /*approved_hash=*/0xDEADBEEFull);

    REQUIRE_FALSE(result.has_value());
    CHECK_MESSAGE(result.error().kind == agentty::tools::ErrorKind::Denied,
                  "a swapped definition is a consent failure, not a tool error");
    CHECK(result.error().detail.find("redefined") != std::string::npos);
}

TEST_CASE("definition pin: a zero hash means unchecked, not denied") {
    // Threads saved before this existed carry no hash, and refusing them
    // would break resume for a risk that only applies where a grant exists.
    // `read` with a bogus path still reaches the tool and fails as a tool
    // error -- which proves the consent gate let it through.
    auto result = agentty::tool::DynamicDispatch::execute_approved(
        "read", json{{"path", "/definitely/not/a/real/file"}},
        /*approved_hash=*/0);

    REQUIRE_FALSE(result.has_value());
    CHECK_MESSAGE(result.error().kind != agentty::tools::ErrorKind::Denied,
                  "it must fail as a READ, not as a denied approval");
}

TEST_CASE("definition pin: a matching hash runs the tool") {
    const auto* td = agentty::tools::find("read");
    REQUIRE(td != nullptr);
    auto result = agentty::tool::DynamicDispatch::execute_approved(
        "read", json{{"path", "/definitely/not/a/real/file"}},
        td->definition_hash());

    REQUIRE_FALSE(result.has_value());   // the path doesn't exist
    CHECK_MESSAGE(result.error().kind != agentty::tools::ErrorKind::Denied,
                  "the live definition matches, so consent is satisfied and "
                  "the failure comes from the tool itself");
}

TEST_CASE("definition pin: an unknown tool is not found, not silently allowed") {
    auto result = agentty::tool::DynamicDispatch::execute_approved(
        "no_such_tool_exists_here", json::object(), 0);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == agentty::tools::ErrorKind::NotFound);
}
