#pragma once
// agentty/rag/rag_host.hpp — what rag-cpp needs from agentty, in one place.
//
// rag-cpp owns no threads, sockets, processes or clocks. Its parallel
// regions split through a Splitter, its network and bridge backends talk
// through HostIo, and its document loader runs converters through a RunFn.
// These build each of those on agentty's runtime.

#include <rag/loaders/extract.hpp>
#include <rag/plugin/builder.hpp>
#include <rag/util/parallel.hpp>

namespace agentty::rag {

// Parallel regions on maya::scope (safe to nest: a nested region runs its
// shares on fresh helpers rather than waiting on a busy pool).
[[nodiscard]] ::rag::util::Splitter splitter();

// HTTP through agentty's client (TLS, proxy, SSRF guard off: these are the
// user's configured endpoints) and processes through util::ChildProcess.
[[nodiscard]] ::rag::plugin::HostIo host_io();

// The built-in backends with host_io().
[[nodiscard]] const ::rag::plugin::Registries& registries();

// Converters (pdftotext, antiword, …) run as child processes, stdout captured.
[[nodiscard]] ::rag::loaders::RunFn converter_runner();

}  // namespace agentty::rag
