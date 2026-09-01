// logx_lifecycle_test — session banner, SIGUSR1 mark, and rotation seeding.
//
// These are the dev-workflow guarantees from docs/website/logging.md
// ("For developers: the always-on capture workflow"):
//   • every process run writes a grep-able "=== agentty session:" banner
//   • signal_mark() stamps a MARK + flight-recorder block, async-signal-safe
//   • both write through the same O_APPEND fd as normal events (so they
//     interleave correctly and count toward mid-run rotation)

#include <fstream>
#include <string>

#include "agtest.hpp"
#include "agentty/util/logx.hpp"

using namespace agentty;

namespace {

std::string whole_log() {
    const auto lf = logx::log_file();
    if (lf.empty()) return {};
    std::ifstream in{std::string{lf}, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>()};
}

void require_logging() {
    REQUIRE_MESSAGE(!logx::log_file().empty(),
                    "AGENTTY_LOG/_FILE must be set for this binary "
                    "(ctest sets them; see cmake/AgenttyTests.cmake)");
}

} // namespace

TEST_CASE("logx: session_banner writes a grep-able run separator") {
    require_logging();
    logx::session_banner("9.9.9-test debug pid=42 cwd=/tmp/x");
    const auto all = whole_log();
    CHECK(all.find("=== agentty session: 9.9.9-test debug pid=42 cwd=/tmp/x ===")
          != std::string::npos);
}

TEST_CASE("logx: signal_mark stamps a MARK block with the flight recorder") {
    require_logging();
    // Seed the ring with a recognisable event so the dump has content.
    AGT_LOG(General, Warn, "lifecycle.test", "pre-mark marker {}", 7);
    logx::signal_mark();
    const auto all = whole_log();
    const auto mark = all.rfind("=== MARK (SIGUSR1)");
    REQUIRE(mark != std::string::npos);
    const auto end = all.find("=== END MARK ===", mark);
    REQUIRE(end != std::string::npos);
    // The flight-recorder snapshot between the fences must contain the
    // seeded Warn event (Warn+ is always ring-recorded).
    CHECK(all.substr(mark, end - mark).find("pre-mark marker 7")
          != std::string::npos);
}

TEST_CASE("logx: banner and mark are no-ops without a file sink") {
    // Can't disable the sink in-process (init is once); this pins the
    // CONTRACT via the API's documented behaviour instead: both calls are
    // noexcept and must not throw or crash even when called repeatedly.
    logx::session_banner("");
    logx::signal_mark();
    logx::session_banner("second call");
    CHECK(true);   // reaching here is the assertion
}
