// protocol_runtime.cpp — mcp::Runtime / acp::Runtime on maya → jaal.
//
// Each background job is a one-worker maya::pool running the body as an
// isolated task. That gives every job what the libraries need, from jaal:
//   * a std::stop_token, requested on stop
//   * stop() that waits for the body to return
//   * stop(grace) that waits only that long and then abandons the task. The
//     pool's state is co-owned by the task, so an abandoned body that wakes
//     later touches only memory it still owns, and the process can exit.
// parallel_for is a maya::scope (every share joined before it returns), and
// sleep_for is maya::delay_for (woken by stop). rag-cpp's parallel regions
// get the same maya::scope through its Executor.

#include "agentty/tool/util/protocol_runtime.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <stop_token>
#include <thread>   // hardware_concurrency
#include <utility>

#include <maya/runtime.hpp>
#include <mcp/runtime.hpp>
#include <acp/runtime.hpp>
#if AGENTTY_HAS_RAGCPP
#include <rag/util/parallel.hpp>
#endif

namespace agentty::tools::util {

namespace {

// One job: a single isolated task on a pool of its own, entered in a
// stop_group so its natural end can be waited for without stopping it.
class PoolJob {
  public:
    explicit PoolJob(std::function<void(std::stop_token)> body) {
        auto m = done_.join();   // always admitted: the group is new
        pool_.post_isolated(
            [b = std::move(body), m = std::make_shared<decltype(m)>(std::move(m))](
                std::stop_token st) mutable {
                b(st);
                m->reset();      // leave the group
            });
    }
    ~PoolJob() { stop(); }
    // Wait for the body to return on its own, then reap the pool.
    void wait() noexcept {
        done_.wait();
        stop();
    }
    void stop() noexcept { (void)pool_.shutdown(maya::pool::no_deadline); }
    bool stop(std::chrono::milliseconds grace) noexcept {
        return pool_.shutdown(grace) == 0;
    }

  private:
    maya::stop_group done_;
    maya::pool       pool_{/*max_workers=*/1};
};

void scope_for(std::size_t n, const std::function<void(std::size_t)>& fn) {
    if (n <= 1) { if (n) fn(0); return; }
    maya::scope([&](maya::nursery& nur) {
        for (std::size_t i = 1; i < n; ++i)
            nur.spawn([&fn, i] { fn(i); });
        fn(0);   // the caller runs a share too
    });
}

// The two libraries declare the same interface in their own namespaces.
template <class Runtime>
class MayaRuntime final : public Runtime {
    class Job final : public Runtime::Job {
      public:
        explicit Job(std::function<void(std::stop_token)> body)
            : job_(std::move(body)) {}
        void wait() noexcept override { job_.wait(); }
        void stop() noexcept override { job_.stop(); }
        bool stop(std::chrono::milliseconds grace) noexcept override {
            return job_.stop(grace);
        }
      private:
        PoolJob job_;
    };

  public:
    std::unique_ptr<typename Runtime::Job>
    spawn(const char*, std::function<void(std::stop_token)> body) override {
        return std::make_unique<Job>(std::move(body));
    }
    bool sleep_for(std::stop_token st, std::chrono::milliseconds d) override {
        return maya::delay_for(st, d);
    }
    void parallel_for(std::size_t n,
                      const std::function<void(std::size_t)>& fn) override {
        scope_for(n, fn);
    }
};

#if AGENTTY_HAS_RAGCPP
// rag-cpp's parallel regions (index build, embedding batches, search) on
// maya::scope. A scope never queues behind a busy pool, so a region nested
// inside another (a query inside a parallel batch) can't deadlock.
class RagExecutor final : public ::rag::util::Executor {
  public:
    void parallel_for(std::size_t n,
                      const std::function<void(std::size_t)>& fn) override {
        scope_for(n, fn);
    }
    [[nodiscard]] std::size_t width() const noexcept override {
        const unsigned hc = std::thread::hardware_concurrency();
        return hc ? hc : 1;
    }
};
#endif

}  // namespace

void install_protocol_runtimes() {
    ::mcp::set_runtime(std::make_shared<MayaRuntime<::mcp::Runtime>>());
    ::acp::set_runtime(std::make_shared<MayaRuntime<::acp::Runtime>>());
#if AGENTTY_HAS_RAGCPP
    ::rag::util::set_executor(std::make_shared<RagExecutor>());
#endif
}

}  // namespace agentty::tools::util
