// jet_global.cpp — activates jetalloc as agentty's process-wide allocator.
//
// jetalloc is header-only, so (unlike the old static-archive build) nothing
// overrides the global operator new/delete unless exactly ONE translation unit
// opts in with JET_GLOBAL_NEW. This is that TU. Building it into the agentty
// executable routes every new/delete — throughout agentty AND every vendored
// submodule linked into the same image (maya, acp-cpp, mcp-cpp, rag-cpp) —
// through jetalloc's per-thread slabs. No header include is needed anywhere
// else; these operator definitions are strong globals the linker prefers over
// libstdc++'s weak ones.
//
// Compiled only when AGENTTY_USE_JETALLOC is set (see the top-level
// CMakeLists.txt). SPDX-License-Identifier: MIT
#define JET_GLOBAL_NEW
#include <jetalloc.hpp>
