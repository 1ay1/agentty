// loop_timing.cpp — note_slow(): name the loop step that ate a frame.
//
// Lives outside the reducers on purpose: it reads the real clock, which the
// fold must not (elm_purity). program.hpp brackets each update arm, view()
// and subscribe() with it; a fast step costs one clock read and a compare.

#include "agentty/runtime/app/program.hpp"
#include "agentty/util/logx.hpp"

namespace agentty::app {

void note_slow(const char* what, std::chrono::steady_clock::time_point t0) noexcept {
    // One frame at 60 Hz. Anything slower is visible as input lag.
    constexpr auto kFrame = std::chrono::milliseconds(16);
    const auto dt = std::chrono::steady_clock::now() - t0;
    if (dt < kFrame) return;
    const double ms = std::chrono::duration<double, std::milli>(dt).count();
    AGT_LOG(Ui, Warn, "loop.slow", "{} {:.1f} ms", what, ms);
}

}  // namespace agentty::app
