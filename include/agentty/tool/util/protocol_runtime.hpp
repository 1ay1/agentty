#pragma once
// agentty's runtime for rag-cpp's parallel regions.
//
// rag-cpp owns no threads: it asks an installed Executor to run its
// parallel regions. This installs one built on maya, once, at startup.

namespace agentty::tools::util {

// Install agentty's executor into rag-cpp. Call once, early in main().
void install_protocol_runtimes();

}  // namespace agentty::tools::util
