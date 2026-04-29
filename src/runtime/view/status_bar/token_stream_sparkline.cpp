#include "moha/runtime/view/status_bar/token_stream_sparkline.hpp"

#include <algorithm>
#include <chrono>
#include <vector>

#include "moha/runtime/view/palette.hpp"

namespace moha::ui {

namespace {

// Pull the rate ring buffer out in chronological (oldest → newest) order.
std::vector<float> ordered_rate_history(const StreamState& s) {
    std::vector<float> out;
    out.reserve(StreamState::kRateSamples);
    if (s.rate_history_full) {
        for (std::size_t i = 0; i < StreamState::kRateSamples; ++i) {
            const auto idx = (s.rate_history_pos + i) % StreamState::kRateSamples;
            out.push_back(s.rate_history[idx]);
        }
    } else {
        for (std::size_t i = 0; i < s.rate_history_pos; ++i)
            out.push_back(s.rate_history[i]);
    }
    return out;
}

} // namespace

maya::TokenStreamSparkline::Config token_stream_sparkline_config(const Model& m) {
    const bool is_streaming = m.s.is_streaming() && m.s.active();

    auto hist = ordered_rate_history(m.s);
    auto now  = std::chrono::steady_clock::now();
    // Reach into the active ctx for the current sub-turn's live
    // counters; nullptr means "no active stream" → we'll fall back
    // to the historical sparkline.
    const phase::Active* a = active_ctx(m.s.phase);
    bool ever_streamed =
        a && a->first_delta_at.time_since_epoch().count() != 0;
    long long ts_ms = ever_streamed
        ? std::chrono::duration_cast<std::chrono::milliseconds>(
              now - a->first_delta_at).count()
        : 0;
    double sec        = std::max(0.001, static_cast<double>(ts_ms) / 1000.0);
    double approx_tok = a ? static_cast<double>(a->live_delta_bytes) / 4.0 : 0.0;

    // Pick the displayed rate: live during streaming (after a 250 ms
    // warm-up so the first frame isn't a divide-by-tiny-time spike),
    // freeze on most recent sample otherwise (so the number doesn't
    // decay during tool execution), 0 before any data.
    float disp_rate;
    if (is_streaming && ts_ms >= 250) {
        disp_rate = static_cast<float>(approx_tok / sec);
    } else if (!hist.empty()) {
        disp_rate = hist.back();
    } else {
        disp_rate = 0.0f;
    }

    maya::TokenStreamSparkline::Config cfg;
    cfg.rate    = disp_rate;
    cfg.total   = static_cast<int>(approx_tok);
    cfg.history = std::move(hist);
    cfg.color   = highlight;
    cfg.live    = is_streaming;
    return cfg;
}

} // namespace moha::ui
