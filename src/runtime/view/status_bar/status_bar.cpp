#include "agentty/runtime/view/status_bar/status_bar.hpp"

#include "agentty/runtime/view/status_bar/context_gauge.hpp"
#include "agentty/runtime/view/helpers.hpp"
#include "agentty/runtime/view/status_bar/phase_chip.hpp"
#include "agentty/runtime/view/status_bar/status_banner.hpp"
#include "agentty/runtime/view/status_bar/title_chip.hpp"
#include "agentty/runtime/view/status_bar/token_stream_sparkline.hpp"

namespace agentty::ui {

maya::StatusBar::Config status_bar_config(const Model& m) {
    maya::StatusBar::Config cfg;
    cfg.phase_color   = phase_color(m.s.phase);
    cfg.breadcrumb    = title_chip_config(m);
    cfg.phase         = phase_chip_config(m);
    cfg.token_stream  = token_stream_sparkline_config(m);
    // Model · provider moved to the composer's footer row (see
    // composer.cpp / model_badge.hpp). The status bar describes the
    // TURN — phase, throughput, context budget; the model describes the
    // MESSAGE you are about to send, so it belongs by the input. Leaving
    // the badge slot default-empty makes StatusBar skip it entirely,
    // which also hands the shed ladder back ~14 columns for the phase
    // verb and context gauge on narrow terminals.
    cfg.context       = context_gauge_config(m);
    cfg.status_banner = status_banner_config(m);
    // Shortcuts row retired — the welcome screen carries the full
    // keybinding map. While a thread is active the user has already
    // internalised the bindings, and the status bar's middle row
    // doubles as the toast slot for transient notifications (retry,
    // cancel, compact, error) which is more useful real estate.

    // Width thresholds retired (maya 6263c4f): StatusBar's activity row
    // is now a measured degradation ladder — every fragment is built at
    // its real styled width and the row sheds detail until it fits. No
    // per-host knobs to tune.

    // Identity of the row, so maya can skip the ladder when nothing moved.
    //
    // That ladder builds up to eight candidate shapes and MEASURES each to
    // find the richest that fits. Correct, and expensive to redo 30 times a
    // second while streaming — profiling put activity_row at 53% of all
    // render time. maya caches the built row under this key, so the work
    // happens once per distinct row instead of once per frame.
    //
    // EVERY input the ladder reads has to be in here. A key that misses one
    // paints a stale status bar, which is a worse failure than the cost it
    // saves: the row would silently stop reporting the thing it exists for.
    // So this mirrors the five configs assigned above, field for field, and
    // `model_badge` is absent for the one reason that makes it safe — this
    // host never sets it (see the note above), leaving it default-empty.
    {
        maya::CacheIdBuilder k;
        k.add(std::string_view{"agentty.status_bar"});
        // phase_color + phase chip
        k.add(static_cast<std::uint64_t>(cfg.phase_color.kind()))
         .add(static_cast<std::uint64_t>(cfg.phase_color.raw_r()))
         .add(cfg.phase.glyph)
         .add(cfg.phase.verb)
         .add(static_cast<std::uint64_t>(cfg.phase.frame))
         .add(static_cast<std::uint64_t>(cfg.phase.elapsed_secs * 10.0f))
         .add(static_cast<std::uint64_t>(cfg.phase.breathing));
        // token stream sparkline — history is the shape that is drawn
        k.add(static_cast<std::uint64_t>(cfg.token_stream.total))
         .add(static_cast<std::uint64_t>(cfg.token_stream.rate * 10.0))
         .add(static_cast<std::uint64_t>(cfg.token_stream.live))
         .add(static_cast<std::uint64_t>(cfg.token_stream.history.size()));
        for (const auto& h : cfg.token_stream.history)
            k.add(static_cast<std::uint64_t>(h));
        // context gauge
        k.add(static_cast<std::uint64_t>(cfg.context.used))
         .add(static_cast<std::uint64_t>(cfg.context.max))
         .add(static_cast<std::uint64_t>(cfg.context.cells))
         .add(static_cast<std::uint64_t>(cfg.context.show_bar))
         .add(static_cast<std::uint64_t>(cfg.context.show_tokens));
        // breadcrumb
        k.add(cfg.breadcrumb.title)
         .add(static_cast<std::uint64_t>(cfg.breadcrumb.max_chars));
        // status banner (takes over the slot entirely when non-empty)
        k.add(cfg.status_banner.text)
         .add(static_cast<std::uint64_t>(cfg.status_banner.is_error))
         .add(static_cast<std::uint64_t>(cfg.status_banner.kind));
        cfg.content_key = k.build().hash();
    }
    return cfg;
}

} // namespace agentty::ui
