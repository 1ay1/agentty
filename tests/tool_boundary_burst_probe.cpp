// tool_boundary_burst_probe — RECREATES the user-reported bug:
//
//   "in a long turn the md rendering gets stuck and then BURSTS when a
//    tool use happens"
//
// end-to-end in a headless harness, with the EXACT production sequence
// from src/runtime/view/thread/turn/turn.cpp::cached_markdown_for:
//
//   1. live wire: set_content(growing prefix) every 16 ms frame,
//      set_reveal_pacing(90, 0.15), reveal_fx on, live on;
//   2. the wire closes the TEXT block (Anthropic content_block_stop →
//      StreamTextBlockClosed → msg.text_block_closed) — from that frame
//      on the view calls request_finalize(160) every frame (the
//      "pre-emptive end-of-text drain");
//   3. `gap` ms later the tool_use lands (tool_calls grows) — the view
//      executes the MANDATORY scrollback-safety trio:
//          snap_reveal_to_edge(); set_reveal_fx(false); finish();
//
// Every frame is rendered to a Canvas and the newly-visible content
// cells are counted (same method as reveal_smoothness_probe). The BURST
// is the snap frame's cell delta: whatever backlog the reveal cursor
// still carried when the card arrived gets pasted in ONE frame.
//
// WHY the pre-emptive drain does not save it (the thing this probe
// makes measurable):
//
//   • While streaming, RateCursor rides ~drain_secs (0.15 s) behind the
//     wire, so at text-block-close the backlog ≈ wire_cps × 0.15
//     (e.g. 1500 cps → ~225 cp).
//   • request_finalize(160) does NOT honour 160 ms: reveal_fx.cpp
//     stretches the ramp to backlog / (floor_cps × 2) — with the
//     production floor of 90 cps that is backlog / 180 cps, i.e.
//     1.25 SECONDS for a 225 cp backlog (capped at 2.5 s).
//   • The real block-close → tool_use gap on the wire is ~100-400 ms.
//     The cursor drains exponentially (τ ≈ 0.15-0.3 s), so after a
//     200 ms gap ~40-60% of the backlog is STILL unrevealed — and the
//     mandatory snap pastes it in one frame. The faster the model, the
//     bigger the paste.
//   • Non-Anthropic transports never emit StreamTextBlockClosed, and
//     the fallback quiet heuristic needs 120 ms of silence — a tool_use
//     that starts streaming sooner skips the drain entirely: the FULL
//     backlog snaps.
//
// Sweeps wire speed × close→tool gap, prints per-scenario: steady
// per-frame reveal, the cell delta of the snap frame, and the burst
// ratio. Exit 0 = burst reproduced (probe did its job), exit 1 = could
// not reproduce.

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <maya/core/anim_clock.hpp>
#include <maya/core/render_context.hpp>
#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>
#include <maya/widget/markdown.hpp>

using namespace maya;

static constexpr int kWidth   = 100;
static constexpr int kTermH   = 40;
static constexpr int kFrameMs = 16;

// Long-turn prose body (~6 KB): plain paragraphs so cells ≈ chars and
// the per-frame cell delta is a faithful proxy for revealed codepoints.
static std::string make_body() {
    std::string b;
    for (int p = 0; p < 18; ++p) {
        b += "Paragraph " + std::to_string(p) +
             " walks through the plan in enough words that the reveal "
             "cursor has real distance to cover while the model keeps "
             "streaming ahead of it, exactly like a long assistant turn "
             "that ends by calling a tool.\n\n";
    }
    b += "Now let me read that file to confirm before editing.";
    return b;   // no trailing \n\n — trailing paragraph rides the tail
}

static int content_cells(const Canvas& c) {
    int n = 0;
    const int mr = c.max_content_row();
    for (int y = 0; y <= mr; ++y)
        for (int x = 0; x < kWidth; ++x) {
            char32_t ch = c.get(x, y).character;
            if (ch == 0 || ch == U' ') continue;
            if (ch >= 0x2500 && ch <= 0x259F) continue;   // border chrome
            ++n;
        }
    return n;
}

struct Result {
    double steady_mean = 0;   // mean cells/frame while wire streams
    int    pre_snap_max = 0;  // max cells/frame between close and snap
    int    snap_delta  = 0;   // cells revealed on the SNAP frame
    int    backlog_at_close = 0;   // unrevealed cells when text closed
    int    backlog_at_snap  = 0;   // unrevealed cells the frame before snap
    int    defer_frames = 0;  // fix path: frames the panel was held back
    int    defer_max    = 0;  // fix path: max cells/frame during the hold
};

// One scenario: stream `body` at wire_cps; text closes at end-of-body;
// `block_closed_signal` = the transport emits StreamTextBlockClosed
// (Anthropic) vs not (OpenAI-compat/Ollama, where only the 120 ms quiet
// heuristic can arm the drain); tool card lands gap_ms after close.
// `deferral_fix` = the turn.cpp tool-panel deferral: when the card lands
// while the reveal is mid-glide, the view HOLDS the panel off-screen and
// arms the finalize ramp; the snap trio runs only once the glide is done
// (or the 1.5 s cap trips).
static Result run(const std::string& body, double wire_cps, int gap_ms,
                  bool block_closed_signal, bool deferral_fix = false,
                  bool trace = false) {
    StreamingMarkdown md;
    md.set_reveal_fx(true);
    md.set_reveal_pacing(/*floor_cps=*/90.0, /*drain_secs=*/0.15);  // prod
    md.set_live(true);

    StylePool pool;
    std::vector<layout::LayoutNode> nodes;
    auto paint = [&]() -> Canvas {
        RenderContext ctx{kWidth, kTermH, render_generation(), true};
        RenderContextGuard guard(ctx);
        Canvas c(kWidth, 4000, &pool);
        c.clear();
        render_tree(md.build(), c, pool, theme::dark, nodes, true);
        return c;
    };

    Result r;
    const double per_frame = wire_cps * (kFrameMs / 1000.0);
    double fed_f = 0.0;
    std::size_t fed = 0;
    int prev = 0;
    long steady_sum = 0; int steady_n = 0;

    // ── Phase 1: wire streams the text ──
    while (fed < body.size()) {
        fed_f += per_frame;
        std::size_t n = std::min<std::size_t>((std::size_t)fed_f - fed,
                                              body.size() - fed);
        fed += n;
        md.set_content(std::string_view{body.data(), fed});
        maya::testing::advance_anim_clock_ms(kFrameMs);
        int cells = content_cells(paint());
        steady_sum += cells - prev; ++steady_n;
        prev = cells;
    }
    r.steady_mean = steady_n ? double(steady_sum) / steady_n : 0.0;
    r.backlog_at_close = int(body.size()) - prev;   // ≈ unrevealed chars

    // ── Phase 2: text block closed; gap before the tool_use lands ──
    // Mirrors turn.cpp: request_finalize(160) fires each frame IF the
    // transport signalled block-close, OR once 120 ms of byte silence
    // passes (text_gone_quiet).
    constexpr int kTextQuietMs = 120;
    int elapsed = 0;
    while (elapsed < gap_ms) {
        const bool quiet = elapsed >= kTextQuietMs;
        if ((block_closed_signal || quiet) && md.reveal_in_progress())
            md.request_finalize(160);
        maya::testing::advance_anim_clock_ms(kFrameMs);
        elapsed += kFrameMs;
        int cells = content_cells(paint());
        r.pre_snap_max = std::max(r.pre_snap_max, cells - prev);
        if (trace)
            std::printf("    gap t=%4dms  +%d cells\n", elapsed, cells - prev);
        prev = cells;
    }
    r.backlog_at_snap = int(body.size()) - prev;

    // ── Phase 3: tool_use lands ──
    if (deferral_fix) {
        // The FIX (turn.cpp tool-panel deferral): the card exists in the
        // model but the view holds its panel off-screen while the reveal
        // is mid-glide; each frame re-arms the finalize ramp. Nothing
        // renders below the prose, so the glide is scrollback-safe. The
        // snap trio runs when the glide completes or the 1.5 s cap trips.
        constexpr int kMaxCardDeferMs = 1500;
        int held = 0;
        while (md.is_live() && md.reveal_in_progress()
               && held < kMaxCardDeferMs) {
            md.request_finalize(160);
            maya::testing::advance_anim_clock_ms(kFrameMs);
            held += kFrameMs;
            int cells = content_cells(paint());
            r.defer_max = std::max(r.defer_max, cells - prev);
            ++r.defer_frames;
            prev = cells;
        }
    }
    // The mandatory snap trio (a no-op when the deferral drained first).
    md.snap_reveal_to_edge();
    md.set_reveal_fx(false);
    md.finish();
    maya::testing::advance_anim_clock_ms(kFrameMs);
    int cells = content_cells(paint());
    r.snap_delta = cells - prev;
    return r;
}

int main() {
    const std::string body = make_body();
    std::printf("tool_boundary_burst_probe — body %zu bytes, width %d, "
                "pacing floor=90cps drain=0.15s (production)\n\n",
                body.size(), kWidth);

    struct Case { double cps; int gap; bool signal; };
    const Case cases[] = {
        // Anthropic path: StreamTextBlockClosed arms the drain instantly.
        // gap=0: the tool_use content_block_start arrives in the SAME SSE
        // packet as the text content_block_stop — both reduce before the
        // next render, so the drain never gets a single frame to run.
        {1500,   0, true}, {1500,  32, true}, {1500,  80, true},
        {1500, 160, true}, {1500, 300, true},
        { 600,   0, true}, { 600, 160, true},
        {3000,   0, true}, {3000,  32, true}, {3000,  80, true},
        {3000, 160, true}, {3000, 300, true},
        // Non-Anthropic path: no signal; quiet heuristic needs 120 ms —
        // a tool_use that streams sooner gets NO drain at all.
        {1500,  80, false}, {3000, 100, false}, {3000, 300, false},
    };

    std::printf("%7s %7s %7s | %8s %9s %9s %9s %7s\n",
                "wire", "gap", "signal", "steady/f", "bkl@close",
                "bkl@snap", "SNAP+", "burst");
    std::printf("---------------------------------------------------------"
                "---------------\n");

    int reproduced = 0;
    for (const auto& c : cases) {
        Result r = run(body, c.cps, c.gap, c.signal);
        const double burst = r.steady_mean > 0.1
            ? r.snap_delta / r.steady_mean : 0.0;
        // A burst = the snap frame pastes >3× the steady per-frame reveal
        // and at least ~2 rows worth of text (>60 cells, the smoothness
        // probe's cap for "a block popped whole").
        const bool is_burst = r.snap_delta > 60 && burst > 3.0;
        reproduced += is_burst;
        std::printf("%5.0f/s %5dms %7s | %8.1f %9d %9d %9d %6.1fx%s\n",
                    c.cps, c.gap, c.signal ? "yes" : "NO",
                    r.steady_mean, r.backlog_at_close, r.backlog_at_snap,
                    r.snap_delta, burst, is_burst ? "  << BURST" : "");
    }

    std::printf("\n");
    if (!reproduced) {
        std::printf("NOT reproduced — no scenario burst at the tool "
                    "boundary.\n");
        return 1;
    }
    std::printf("REPRODUCED: %d scenario(s) paste a multi-line backlog in "
                "the single frame the tool card lands.\n"
                "Root cause chain: RateCursor rides wire_cps*0.15s behind "
                "the edge; the block-close->tool_use\ngap on the wire is "
                "~0ms (consecutive SSE events), so the pre-emptive drain "
                "gets no frames\nand the mandatory snap_reveal_to_edge() "
                "at the card pastes the backlog.\n\n",
                reproduced);

    // ── The FIX: tool-panel deferral (turn.cpp) ──
    // Re-run every burst-prone scenario with the deferral enabled and
    // assert (a) the snap frame pastes nothing notable, and (b) the
    // glide frames during the hold stay burst-free too.
    std::printf("with tool-panel deferral (the fix):\n\n");
    std::printf("%7s %7s %7s | %8s %9s %7s %7s %9s\n",
                "wire", "gap", "signal", "steady/f", "defer_fr",
                "deferX", "SNAP+", "verdict");
    std::printf("---------------------------------------------------------"
                "---------------\n");
    int still_bursting = 0;
    for (const auto& c : cases) {
        Result r = run(body, c.cps, c.gap, c.signal, /*deferral_fix=*/true);
        const double burst = r.steady_mean > 0.1
            ? r.snap_delta / r.steady_mean : 0.0;
        const int worst = std::max(r.snap_delta, r.defer_max);
        const bool is_burst =
            (r.snap_delta > 60 && burst > 3.0) || r.defer_max > 120;
        still_bursting += is_burst;
        std::printf("%5.0f/s %5dms %7s | %8.1f %9d %7d %7d %9s\n",
                    c.cps, c.gap, c.signal ? "yes" : "NO",
                    r.steady_mean, r.defer_frames, r.defer_max,
                    r.snap_delta, is_burst ? "BURST" : "ok");
        (void)worst;
    }
    std::printf("\n%s\n", still_bursting
        ? "FIX INCOMPLETE — deferral still bursts"
        : "FIX VERIFIED — no boundary burst with the panel deferral");
    return still_bursting ? 1 : 0;
}
