// agentty::mcp::serve_stdio — expose agentty's native tools OVER MCP.
//
// The inverse of bridge.cpp. We build a single mcp::Server over stdio and
// register every tool in tools::registry() as an MCP tool. A registered tool's
// handler round-trips through tool::DynamicDispatch::execute(name, args) — the
// SAME dispatch the agent loop uses — so output budgets, the empty-args guard,
// and crash isolation all apply identically. The result (an ExecResult =
// expected<ToolOutput, ToolError>) is projected onto an mcp::CallToolResult.
//
// Effects → annotations is the reverse of bridge.cpp's effects_for(): a tool
// that neither writes the filesystem nor execs is advertised readOnlyHint:true
// (and not destructive); anything that writes/execs is destructiveHint:true.
// These are HINTS for the client's own UX — agentty's permission policy still
// lives wholly on the consuming side. The schema is the tool's own JSON schema
// decoded straight into mcp::JsonSchema.
//
// The heavy mcp-cpp templates stay confined to this TU (it's part of the
// agentty_mcp object library); the agentty-facing header is mcp-cpp-free.

#include "agentty/mcp/serve.hpp"

#include "agentty/tool/tool.hpp"        // DynamicDispatch
#include "agentty/tool/registry.hpp"    // registry(), ToolDef
#include "agentty/tool/effects.hpp"
#include "agentty/domain/profile.hpp"

#include <mcp/mcp.hpp>

#include <cstdio>
#include <iostream>
#include <string>

#ifndef AGENTTY_VERSION
#define AGENTTY_VERSION "0.0.0-dev"
#endif

namespace agentty::mcp {

namespace {

using ::mcp::Json;

// Reverse of bridge.cpp's effects_for(): describe a native tool to a remote
// client via MCP annotations. A tool that does not write the filesystem and
// does not exec is read-only (it may still ReadFs / Net — those observe, not
// mutate). Anything with WriteFs or Exec is flagged destructive so a cautious
// client can gate it. idempotentHint/openWorldHint are left unset (unknown).
::mcp::ToolAnnotations annotations_for(tools::EffectSet fx) {
    using tools::Effect;
    ::mcp::ToolAnnotations a;
    const bool mutates = fx.has(Effect::WriteFs) || fx.has(Effect::Exec);
    a.readOnlyHint    = !mutates;
    a.destructiveHint = mutates;
    // A tool that touches the network reaches beyond the local closed world.
    a.openWorldHint   = fx.has(Effect::Net);
    return a;
}

// Project agentty's ExecResult onto an MCP CallToolResult. Success → the
// tool's text as a single content block; if the tool produced a FileChange
// (write/edit), surface its summary as structuredContent so a remote caller
// that wants the diff metadata can read it without it polluting the text the
// model sees. Failure → isError:true with the rendered ToolError as text (the
// spec's convention for tool-level, non-protocol errors).
::mcp::CallToolResult to_call_result(tools::ExecResult&& r) {
    ::mcp::CallToolResult out;
    if (!r) {
        out.isError = true;
        out.content = {::mcp::text(r.error().render())};
        return out;
    }
    out.content = {::mcp::text(r->text.empty() ? std::string{"(no output)"}
                                               : std::move(r->text))};
    if (r->change.has_value()) {
        const auto& c = *r->change;
        out.structuredContent = Json{
            {"path",    c.path},
            {"added",   c.added},
            {"removed", c.removed},
        };
    }
    return out;
}

// Build the mcp::Tool spec (schema + annotations + description) for one ToolDef.
::mcp::Tool spec_for(const tools::ToolDef& def) {
    ::mcp::Tool t;
    t.name        = def.name.value;
    t.description = def.description;
    // The tool's JSON schema decodes straight into mcp::JsonSchema (type is
    // always "object"; properties + required pass through verbatim).
    if (def.input_schema.is_object())
        t.inputSchema = ::mcp::from_json<::mcp::JsonSchema>(def.input_schema);
    t.annotations = annotations_for(def.effects);
    return t;
}

} // namespace

int serve_stdio() {
    ::mcp::StdioTransport transport(std::cin, std::cout);
    ::mcp::Server server(
        transport.sink(),
        ::mcp::Implementation{"agentty", AGENTTY_VERSION,
                              std::string("agentty native tools"),
                              ::mcp::Nothing, ::mcp::Nothing, ::mcp::Nothing});
    server.set_capabilities(::mcp::ServerCapabilities{
        .tools = ::mcp::ToolsCapability{},
    });
    server.set_instructions(
        "agentty's native coding tools served over MCP: file read/edit/write, "
        "shell (bash), code search (grep/glob/find_definition), web fetch/search, "
        "diagnostics, and git. Filesystem tools are sandboxed to the workspace "
        "the agentty process was launched in.");

    // Register every native tool. The handler routes through the very same
    // DynamicDispatch the agent loop uses, so behaviour is identical.
    std::size_t n = 0;
    for (const auto& def : tools::registry()) {
        const std::string name = def.name.value;
        server.register_tool(spec_for(def),
            [name](const Json& args) -> ::mcp::CallToolResult {
                return to_call_result(
                    tool::DynamicDispatch::execute(name, args));
            });
        ++n;
    }

    std::fprintf(stderr, "agentty: MCP server ready on stdio (%zu tools)\n", n);

    transport.start(server.engine());
    transport.join();   // run until stdin closes
    return 0;
}

} // namespace agentty::mcp
