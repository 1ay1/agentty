#pragma once
// mcp_tools_backends — the agentty-side HostServices backends for the
// host-coupled tool SHELLS that mcp-cpp's toolset owns (remember/forget/
// wipe, todo, skill, search_docs, task).
//
//   mcp-cpp owns each tool's protocol surface; agentty supplies the work via
//   injected backends. install_host_backends() constructs the adapters
//   (MemoryStore over the JSONL store, SkillResolver over the Agent-Skills
//   engine, DocRetriever over the RAG pipeline, SubagentRunner over the
//   subagent loop) and installs them into the HostServices the bridge passes
//   to make_provider(). A tool whose backend is null isn't advertised.

#include <mcp/tools/host.hpp>

namespace agentty::tools {

// Populate svc.memory / svc.skills / svc.retriever / svc.subagent with the
// agentty backends. Leaves svc.todo null (the mcp todo shell needs no host
// state) and svc.http untouched (the bridge installs the HttpClient).
void install_host_backends(::mcp::tools::HostServices& svc);

} // namespace agentty::tools
