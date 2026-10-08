#pragma once
// agentty's runtime for the protocol libraries (mcp-cpp, rag-cpp).
//
// Those libraries own no threads: their engines and transports ask an
// installed Runtime for every background job, sleep and parallel-for. This
// is that Runtime, built on maya's runtime (and so on jaal), installed once
// at startup by install_protocol_runtimes(). docs/LAYERING.md.

namespace agentty::tools::util {

// Install agentty's runtime into mcp-cpp and rag-cpp. Call once, early in
// main(), before any engine or transport starts.
void install_protocol_runtimes();

}  // namespace agentty::tools::util
