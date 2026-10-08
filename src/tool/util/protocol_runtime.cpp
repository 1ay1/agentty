// protocol_runtime.cpp — rag-cpp's parallel regions on maya.
//
// mcp-cpp and acp-cpp have no runtime hook any more: agentty drives their
// connections itself (rpc::Peer). rag-cpp still asks an installed Executor
// for its parallel regions (index build, embedding batches, search); here
// each one is a maya::scope, so every share is joined before it returns and
// a region nested in another can't deadlock behind a busy pool.

#include "agentty/tool/util/protocol_runtime.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <thread>   // hardware_concurrency

#include <maya/runtime.hpp>
#if AGENTTY_HAS_RAGCPP
#include <rag/util/parallel.hpp>
#endif

namespace agentty::tools::util {

namespace {

#if AGENTTY_HAS_RAGCPP
class RagExecutor final : public ::rag::util::Executor {
  public:
    void parallel_for(std::size_t n,
                      const std::function<void(std::size_t)>& fn) override {
        if (n <= 1) { if (n) fn(0); return; }
        maya::scope([&](maya::nursery& nur) {
            for (std::size_t i = 1; i < n; ++i)
                nur.spawn([&fn, i] { fn(i); });
            fn(0);   // the caller runs a share too
        });
    }
    [[nodiscard]] std::size_t width() const noexcept override {
        const unsigned hc = std::thread::hardware_concurrency();
        return hc ? hc : 1;
    }
};
#endif

}  // namespace

void install_protocol_runtimes() {
#if AGENTTY_HAS_RAGCPP
    ::rag::util::set_executor(std::make_shared<RagExecutor>());
#endif
}

}  // namespace agentty::tools::util
