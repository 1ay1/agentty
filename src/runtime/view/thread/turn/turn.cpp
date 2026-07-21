#include "agentty/runtime/view/thread/turn/turn.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <maya/widget/agent_timeline.hpp>
#include <maya/widget/markdown.hpp>
#include <maya/core/render_context.hpp> // available_height (resize-shrink detect)
#include <maya/render/cache_id.hpp>
#include <maya/render/renderer.hpp> // build_layout_tree / layout::compute
#include <maya/layout/yoga.hpp>     // maya::layout::compute / LayoutNode
#include <maya/platform/io.hpp>
#include <maya/style/theme.hpp>
#include <maya/app/app.hpp>

#include "agentty/domain/catalog.hpp"
#include "agentty/runtime/composer_attachment.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/agent_timeline.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_args.hpp"
#include "agentty/runtime/view/cache.hpp"
#include "agentty/runtime/view/helpers.hpp"
#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/thread/turn/permission.hpp"
#include "agentty/runtime/view/thread/seam.hpp"

namespace agentty::ui {

namespace {

// ── Cached markdown render. The ONE Element-returning helper kept in
//    agentty — strictly because cross-frame cache state lives in the
//    StreamingMarkdown widget instance, which we keep alive across
//    frames so its block cache survives.
//
//    Single rendering path: the StreamingMarkdown widget is used for
//    live AND settled messages. The widget's pre-finish output (prefix
//    ComponentElement + tail Element wrapped in vstack.gap(1)) has a
//    slightly different total height than the one-shot maya::markdown()
//    parser's output (flat blocks under the same vstack wrapper but no
//    ComponentElement seam). Swapping between them at StreamFinished
//    shifted the canvas by ~3 rows, which propagated through the per-row
//    diff and left the composer at a different terminal row — visible
//    as "composer pulled down + duplicate composer above it on the
//    first keypress."
//
//    Staying on the streaming widget keeps the height stable across the
//    streaming → idle transition. set_content with byte-identical bytes
//    is an internal no-op; build() returns cached_build_ when nothing
//    has dirtied, so the per-frame cost is the same as the finalized
//    path. The tail re-parses on each frame for finalized messages too,
//    but that's a single inline parse on the last few bytes — cheap.
maya::Element cached_markdown_for(const Message& msg, const Model& m) {
    // Lifecycle-aware cache access (see cache.hpp's partition rationale).
    // A message's md slot holds LOAD-BEARING animation state — the
    // StreamingMarkdown reveal widget + defer bookkeeping — exactly while
    // it is still moving: live wire bytes arriving, OR the widget itself
    // still animating (live / finalize ramp / cursor gliding / async
    // parse). Evicting the slot in that window destroys the reveal
    // mid-glide and stalls the typewriter, so it must be PINNED (kept out
    // of any evictable set entirely). Once fully drained the slot is a
    // pure render memo that stages settled until freeze drops it.
    //
    // We can't read the widget's post-update animation state before we
    // access the slot, so pin on either signal available up front: live
    // wire bytes on the message, or an existing widget that reports
    // itself still animating from last frame. A message that has neither
    // is settled and staged (dropped at freeze). The predicate is
    // deliberately the same shape as turn.cpp's `subturn_stably_keyable`
    // negation — liveness has ONE definition in this view.
    const bool has_live_bytes = !msg.streaming_text.empty()
                             || !msg.pending_stream.empty();
    const auto& probe = m.ui.view_cache; // const peek, no touch/reorder
    const bool widget_animating = [&] {
        // is_pinned is a cheap const lookup; if the slot is already
        // pinned from last frame we keep pinning until it drains (checked
        // post-update below via the same accessor). If it's settled or
        // absent we only pin when the message carries live bytes.
        return probe.is_pinned(m.d.current.id, msg.id);
    }();
    const bool want_pin = has_live_bytes || widget_animating;

    auto& cache = want_pin
        ? m.ui.view_cache.message_md_live(m.d.current.id, msg.id)
        : m.ui.view_cache.message_md    (m.d.current.id, msg.id);

    if (!cache.streaming) {
        cache.streaming = std::make_shared<maya::StreamingMarkdown>();
        // Animated live tail: gradient trail + scramble→resolve + pulsing
        // caret on the streaming edge (maya reveal_fx). Only animates while
        // the widget is live_; the settled build is untouched.
        cache.streaming->set_reveal_fx(true);
        // Reveal pacing for the rate-smoothed bounded-lag cursor (maya
        // RateCursor). The cursor reveals at backlog / drain_secs, so it
        // TRACKS the model's own speed with a fixed time lag, and low-passes
        // that rate so a chunky wire slides in instead of teleporting. Args:
        //   • floor_cps = the MINIMUM reveal speed. A trickle still types out
        //     at >= this so a slow/local model doesn't inch in one char at a
        //     time. It is NOT a ceiling — a fast model reveals FASTER, at its
        //     own delivery rate (the cursor tracks the wire), so the reveal
        //     never falls permanently behind and dumps at settle. 90 cp/s is
        //     a brisk readable minimum.
        //   • drain_secs = the target LAG: how far behind the live edge the
        //     cursor rides, in seconds. rate = backlog / drain_secs holds the
        //     reveal ~drain_secs behind the wire at the wire's own speed, so
        //     the STEADY-STATE backlog ≈ wire_cps × drain_secs. That backlog
        //     is exactly what gets dumped when the ToolUse guard hard-snaps
        //     the reveal to the edge (snap_reveal_to_edge is mandatory for
        //     scrollback safety — a growing tool card strands any lagged
        //     inline line; proven by scrollback_oracle_test, which fails on
        //     ANY glide at ANY ramp). So the ONLY safe lever on the
        //     "first char sticks then bursts with the next tool" symptom is
        //     to keep this lag small: at 0.15s a 1200 cps reply carries only
        //     ~180 chars of backlog at the boundary (half the old 0.3s
        //     burst), while 0.15s is still ~9 frames of jitter smoothing —
        //     a chunky SSE delta still slides in over several frames rather
        //     than teleporting. Tighter than this starts to feel per-token
        //     rather than smoothed.
        cache.streaming->set_reveal_pacing(/*floor_cps=*/90.0,
                                           /*lead_secs=*/0.15);
    }

    // Pick the source bytes for THIS frame. The reveal cursor must see
    // EVERY byte that has arrived from the wire so it can pace them
    // smoothly on its own wall clock; if it only saw `streaming_text`,
    // the visible text could advance only when meta.cpp's Tick handler
    // drips pending_stream → streaming_text (33 ms sync / 100 ms non-
    // sync / more over SSH). Between ticks each text delta still forces
    // a render (fps=0), but streaming_text wouldn't have grown, so the
    // reveal had nothing new to show — the text sat STUCK until the next
    // tick dripped a chunk, then JUMPED. That is the "md gets stuck then
    // bursts" stutter, and it is structural: deltas feed pending_stream,
    // the drip feeds streaming_text only on Tick, the view read only
    // streaming_text. Reading text + streaming_text + pending_stream
    // makes the wall-clock reveal cursor the SINGLE display pacer over
    // all arrived bytes, so visible output advances continuously at
    // kRevealCharsPerSec independent of the tick cadence. (meta.cpp's
    // drip still runs — it moves bytes into streaming_text where the
    // settle path commits them — but it no longer controls what the
    // user SEES.)
    //
    //   • settled: msg.text holds the final body, streaming_text +
    //     pending_stream empty.
    //   • mid-sub-turn-2: msg.text holds the PRIOR sub-turn's settled
    //     body; streaming_text + pending_stream hold the in-flight
    //     follow-up bytes. Feed all so the live tail keeps growing.
    //   • sub-turn-1 streaming: msg.text empty, streaming_text +
    //     pending_stream grow.
    // The joined buffer is cached on MessageMdCache so the string_view
    // we hand to set_content_async stays valid across the call.
    //
    // Size-based no-op fast-path. Reveal_fx animates at 60 fps; bytes
    // arrive at 10-30 / s, so >90% of frames produce no source-size
    // change. Without this guard we re-concat msg.text + streaming_text
    // + pending_stream (O(N) alloc+memcpy) AND set_content_async memcmps
    // the result (O(N) memcmp) every frame for the in-flight turn. For
    // a 50 KB sub-turn-2 body that's ~100 KB of memory bandwidth per
    // frame just to discover nothing changed. Compare the three sizes
    // instead: if none grew, the source bytes are byte-identical and we
    // can skip straight to build().
    const bool sizes_unchanged =
        cache.last_text_size      == msg.text.size()
     && cache.last_streaming_size == msg.streaming_text.size()
     && cache.last_pending_size   == msg.pending_stream.size();

    const std::string* source_ptr = &msg.text;
    const bool has_live = !msg.streaming_text.empty()
                       || !msg.pending_stream.empty();
    if (has_live) {
        if (msg.text.empty() && msg.pending_stream.empty()) {
            // Streaming-text only, nothing buffered — reference it
            // directly, no copy.
            source_ptr = &msg.streaming_text;
        } else if (sizes_unchanged && !cache.combined_source.empty()) {
            // Sizes match what we already concatenated last frame; the
            // backing buffer is still valid. Reuse it; no copy.
            source_ptr = &cache.combined_source;
        } else {
            cache.combined_source.clear();
            cache.combined_source.reserve(
                msg.text.size() + msg.streaming_text.size()
                + msg.pending_stream.size());
            cache.combined_source.append(msg.text);
            cache.combined_source.append(msg.streaming_text);
            cache.combined_source.append(msg.pending_stream);
            source_ptr = &cache.combined_source;
        }
    }
    const std::string& source = *source_ptr;

    // Remember the component sizes for next frame's no-op check.
    cache.last_text_size      = msg.text.size();
    cache.last_streaming_size = msg.streaming_text.size();
    cache.last_pending_size   = msg.pending_stream.size();

    const bool settled = !msg.text.empty()
                       && msg.streaming_text.empty()
                       && msg.pending_stream.empty();
    if (settled && !cache.combined_source.empty()) {
        // Reclaim the scratch buffer the moment streaming_text
        // drains — next freeze takes the snapshot off msg.text.
        std::string{}.swap(cache.combined_source);
    }

    // Fast-path: fully settled message whose reveal has completed.
    // Skip all per-frame work (typewriter advance, set_content,
    // finish, live-mode checks) and return the cached element tree
    // directly. This is the dominant case on long sessions — N
    // visible turns × every frame would otherwise pay constant
    // overhead for each one.
    if (settled
        && cache.last_settled_size == source.size()
        && cache.revealed_size    == source.size()) {
        auto built = cache.streaming->build();
        // Lifecycle down-migration: this message is fully drained (no
        // live bytes, reveal complete). If its slot is still PINNED from
        // its streaming days, hand it back to the LRU now — it is a pure
        // render memo from here on, and leaving it pinned would leak a
        // permanent (uncapped) entry per settled turn. `build` is a value
        // (not a reference into the slot), so the migrate that follows
        // can safely move the Entry. Idempotent: no-op once settled.
        if (want_pin)
            (void)m.ui.view_cache.message_md(m.d.current.id, msg.id);
        return built;
    }

    // ── Reveal: feed ALL arrived bytes, every frame ──
    //
    // There is NO host-side typewriter cursor. The display pacer is
    // maya's reveal_fx (scramble→resolve + gradient trail + caret on the
    // live edge), which animates the trailing run of the text we feed.
    // The host's only job is to keep handing the widget the full set of
    // bytes that have arrived from the wire and to keep the animation
    // frame armed while the stream is live.
    //
    // Why no second cursor: a host pacing clock that withholds bytes and
    // releases them at a fixed char/sec is a SECOND clock beating against
    // (a) the wire-arrival clock and (b) the render-wake cadence. Any
    // beat between them is visible as stutter — and if the host clock
    // ever stops advancing (a render skipped, the wake cadence dropped to
    // the 100 ms Tick) the text sits STUCK mid-reveal until an unrelated
    // event flips the frame. Feeding the raw arrived bytes removes that
    // clock entirely: visible text == arrived text, always. Chunky
    // deltas (a multi-KB code block in one delta) appear at once, but
    // reveal_fx animates their trailing edge so the seam still reads as a
    // live stream, the same approach Zed / Claude Code use.
    //
    // revealed_size is kept == source.size() so the settled fast-path and
    // the freeze snapshot (which read revealed_size) see a fully-revealed
    // message and never freeze a partial body.
    cache.revealed_size = source.size();
    const std::string_view feed_source = source;

    // Settled-message fast path. Once a message has settled
    // (msg.text is final, streaming_text empty) the source bytes are
    // immutable for the rest of the session. set_content's equal-
    // content check still costs O(source.size()) memcmp every frame;
    // on a long thread with many visible turns that adds up. Skip
    // the call entirely once we've fed the final bytes through once.
    //
    // Gate is (size + settled-once flag) only. No reducer rewrites
    // an Assistant's msg.text in place after StreamFinished moves
    // streaming_text → text; later edits replace the Message
    // wholesale (new MessageId, new cache slot). Hashing the bytes
    // every frame to guard against a same-length in-place rewrite
    // that doesn't exist cost O(text) per visible settled turn per
    // frame — the dominant per-frame cost on long sessions.
    const bool already_settled_into_cache =
        settled
        && cache.last_settled_size == source.size()
        && cache.revealed_size == source.size();

    // Optional per-frame timer for the streaming-markdown widget. Set
    // AGENTTY_STREAM_PROF=1 to log set_content+finish+build() cost for
    // each non-fast-path call to /tmp/agentty-stream-prof.log. Isolates
    // the in-flight widget cost from the (separately-profiled) timeline
    // render. One line per call; skips the settled fast-path entirely.
    static const bool stream_prof = []{
        const char* e = std::getenv("AGENTTY_STREAM_PROF");
        return e && *e && *e != '0';
    }();
    const auto prof_t0 = stream_prof
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};

    if (!already_settled_into_cache) {
        // Skip set_content_async entirely on a sizes-unchanged frame.
        // The widget already saw these exact bytes last frame; its
        // internal source_ matches feed_source by length and prefix.
        // set_content_async's own no-op fast-path would memcmp
        // O(N) bytes to discover this; the size check here is O(1).
        // Still emit the call when sizes match but the widget is
        // currently parsing async (it needs the poll to adopt the
        // result) OR when this is the first feed (combined_source
        // empty above means we took the direct ref path).
        const bool skip_set_content =
            sizes_unchanged
            && cache.streaming->source().size() == feed_source.size()
            && !cache.streaming->is_parsing();
        if (!skip_set_content) {
            // Path split by liveness:
            //
            //   LIVE WIRE (!settled): always feed the sync
            //   set_content. The live combined feed is append-in-
            //   spirit — text + streaming_text + pending_stream grows
            //   monotonically within a sub-turn. But at each sub-turn
            //   boundary the prior sub-turn's streaming_text folds into
            //   msg.text and streaming_text resets, so the NEXT feed is
            //   a divergent-prefix change; once the accumulated body
            //   crosses set_content_async's 16 KB threshold that swap
            //   would spawn a detached parse worker. While the worker
            //   runs is_parsing() is true and the widget returns its
            //   PREVIOUS element tree — the live reveal FREEZES — and
            //   the result is only adopted (maybe_apply_async_) the next
            //   time build() is polled. If the wire goes quiet for a
            //   beat while the frozen-scrollback visual-hash gate
            //   suppresses redraws, build() stops being polled and the
            //   finished parse sits unapplied: the tail stalls until the
            //   next delta / 100 ms Tick. That's the "md streaming stops
            //   after a while in a long turn, fine again next turn"
            //   report (next turn = fresh <16 KB placeholder → sync
            //   path). The sync path has no is_parsing() window, so the
            //   live reveal can never freeze on a parse.
            //
            //   SETTLED / RELOAD (settled): keep the async variant. A
            //   thread reload or scrollback recovery swaps in a whole
            //   >=16 KB body at once as a divergent prefix; offloading
            //   that one-shot parse to a worker keeps the render thread
            //   responsive, and finish() below force-adopts it in the
            //   same pass, so there's no unbounded freeze window.
            if (!settled)
                cache.streaming->set_content(feed_source);
            else
                cache.streaming->set_content_async(feed_source);
        }

        // Settled message → commit any trailing tail to the prefix's
        // block list. finish() flushes whatever is still in the
        // streaming tail into the canonical committed block path
        // (md_block_to_element), so the settled render is byte-identical
        // to the live one.
        //
        // Historically this was load-bearing for a trailing closed code
        // fence: find_block_boundary only committed a fenced block once
        // its closing ``` was followed by a newline, so a message ending
        // at the closing backticks (the common case for a reply ending
        // in a code example) left the last block stuck in the tail,
        // rendered via render_tail's inline path. render_tail and
        // md_block_to_element feed the same border/padding builder
        // slightly different code strings, so their cells weren't
        // byte-identical — at settle the whole last block re-emitted to
        // the terminal (the "repaint", worst over SSH). That divergence
        // is now fixed upstream in maya (boundary.cpp eager-commits a
        // closing fence at end-of-buffer), so the live and settled cells
        // already match before this call. finish() is kept because it's
        // idempotent (no-op once committed_ == source_.size()) and still
        // the correct place to flush any OTHER trailing-block kind.
        //
        // At settle we kick off the finalize ramp FIRST — the widget
        // glides its reveal cursor to the live edge over ~200 ms and
        // flips live_ off itself when the cursor catches up. We defer
        // finish() (which forces live_=false) until that ramp completes,
        // so the reveal animation isn't cut short and dump its backlog
        // in one frame.
        //
        // Skip on a sizes-unchanged frame. set_live(true) goes through
        // the Tracked<> wrapper which auto-bumps build_dirty_ on EVERY
        // assignment (even same-value): a wasted rebuild per frame at 60
        // fps. request_finalize is idempotent (early-out on existing
        // deadline) but cheap to skip. With this gate, no-grow frames
        // leave build_dirty_ alone and build() returns cached_build_
        // directly.
        if (!sizes_unchanged) {
            if (settled) {
                // Finish IMMEDIATELY (no 200 ms request_finalize glide) so
                // the live height equals the settled/frozen height in the
                // same frame — agent_session's MessageStop discipline. The
                // glide kept the widget live_ and animating after the
                // stream ended; at fps=0 over SSH the sparse frames let the
                // live-tail height drift between paints, so the freeze
                // handoff diffed a moved tree and stranded a duplicate in
                // scrollback. The reducer already pre-settles via
                // settle_message_md; this keeps the view path in lockstep
                // for any settled message still in the live tail.
                cache.streaming->finish();
            } else {
                cache.streaming->set_live(true);
            }
        }

        // Auto-fold code blocks longer than ~40 lines so a wall of code
        // in a long conversation doesn't push every other turn
        // off-screen. CRITICAL for scrollback safety: fold EVERY frame
        // (live AND settled), not only inside the finish() gate below.
        //
        // auto_fold_long_blocks only touches COMMITTED blocks
        // (prefix_->metas) — the still-streaming tail block is never
        // folded, so the live edge keeps animating at full height. The
        // moment a long code block's closing fence commits, it folds to
        // ~1 row WHILE LIVE. If we only folded at finish() (the old
        // behaviour) the block rendered full-height for the whole stream
        // + the 200 ms finalize ramp, then collapsed ~N→1 rows in one
        // frame at settle — a large height shrink that maya diffs against
        // the prior full-height frame and re-emits the entire turn from
        // the top (the "redraws the turn after streaming finishes"
        // jump). Folding live makes the live height already equal the
        // settled/frozen height, so finish() is a no-op shape-wise and
        // the freeze handoff is seamless — exactly like agent_session,
        // where live height == settled height because it has no fold.
        // frozen.cpp's prose_rows mirrors this fold so the row estimate
        // agrees across the freeze. Respects an explicit user unfold
        // (entry stored as `false`) and won't re-fold.
        //
        // Skipped on a sizes-unchanged frame: no new bytes → no new
        // committed block can have appeared → nothing to fold. The
        // function would walk prefix_->metas (O(committed blocks),
        // dozens to hundreds on a long body) and hit the
        // `folds_.contains(source_offset)` early-out on every entry.
        // Cheap absolutely, but pure waste on the dominant no-grow frame.
        if (!sizes_unchanged) {
            constexpr std::uint16_t kFoldLineThreshold = 40;
            constexpr std::uint32_t kFoldKinds =
                (1u << static_cast<unsigned>(maya::StreamingMarkdown::BlockKind::CodeBlock));
            cache.streaming->auto_fold_long_blocks(kFoldLineThreshold, kFoldKinds);
        }

        if (settled
            && cache.revealed_size == source.size()
            && !cache.streaming->is_finalizing())
        {
            cache.streaming->finish();
            cache.last_settled_size = source.size();
        }
    }

    // (Live/finalize transitions are handled above, before the finish()
    // gate that depends on them.)

    // Track when `source` last grew so we can tell "actively streaming"
    // (bytes flowing) from "streaming but stalled" (e.g. a 60–120 s
    // extended-thinking pause with no deltas). Used by the RAF gate just
    // below.
    {
        const auto now2 = std::chrono::steady_clock::now();
        if (source.size() > cache.last_grow_size) {
            cache.last_grow_size = source.size();
            cache.last_grow_tick = now2;
        } else if (source.size() < cache.last_grow_size) {
            // Source rolled / shrank (set_content rollback, message reset).
            cache.last_grow_size = source.size();
            cache.last_grow_tick = now2;
        }
    }

    // Re-arm the 16 ms animation frame for the WHOLE active streaming
    // window. reveal_fx (the live-edge scramble/gradient/caret) is the
    // only animation now; it needs a wake every frame while bytes are
    // flowing so its trailing run resolves smoothly.
    //
    // PRIMARY signal: the model is authoritatively streaming. m.s
    // .is_streaming() is variant-backed (phase::Streaming) and exact —
    // it's the same signal the status bar / phase chip / sparkline gate
    // on. Combined with !settled (this message still has live bytes in
    // streaming_text/pending_stream), it means "the wire is open and
    // THIS turn is the one receiving it." While that holds, keep the
    // caret armed UNCONDITIONALLY — no matter how long the gap between
    // deltas. The pulsing caret means "waiting for the model," which is
    // exactly true during an inter-delta pause, even a 10 s one. This is
    // what makes the fix robust across models/networks: it keys off the
    // real in-flight state, not a guess about delta cadence.
    //
    // The earlier byte-recency window (since_grow_ms) raced the model's
    // gap: 250 ms lapsed inside every slow-model gap (median ~470 ms),
    // freezing the caret mid-sentence ("stream looks dead"); bumping it
    // to 3 s only moved the cliff. A fixed timeout can ALWAYS be out-run
    // by a slower model or a laggy link. The phase gate can't — it ends
    // the instant the wire closes (phase → Idle/ExecutingTool) and not a
    // frame before. Cost while streaming is bounded and already paid:
    // build() runs every frame the visual hash advances; an armed caret
    // adds a ~0.1-0.4 ms no-content repaint, only while genuinely live.
    //
    // The since_grow_ms window is kept as a SECONDARY fallback for the
    // edge where is_streaming() has already flipped (e.g. StreamFinished
    // landed, phase → Idle) but the reveal cursor still has a backlog to
    // glide out — reveal_in_progress() below already covers that, but the
    // window catches a stale-phase beat without re-introducing the race
    // (it only EXTENDS arming, never cuts it short while is_streaming()).
    const bool wire_streaming_here = !settled && m.s.is_streaming();
    constexpr std::int64_t kRevealActiveMs = 3000;
    const auto now3 = std::chrono::steady_clock::now();
    const std::int64_t since_grow_ms =
        cache.last_grow_tick.time_since_epoch().count() == 0
            ? kRevealActiveMs + 1
            : std::chrono::duration_cast<std::chrono::milliseconds>(
                  now3 - cache.last_grow_tick).count();
    const bool stream_in_motion =
        wire_streaming_here
        || (!settled && since_grow_ms <= kRevealActiveMs);

    // ── Scrollback safety: never let a mid-reveal row cross the viewport
    //    top. Rows above the top are committed to IMMUTABLE native
    //    scrollback; if one gets there while the typewriter is still
    //    behind the wire (ghost-blanked cells, scramble tip, hot
    //    gradient), later frames complete the reveal, the canvas row no
    //    longer matches the committed copy, maya's scrollback-invariant
    //    gate fires, and the grow-path recovery is a HardReset
    //    (\x1b[2J\x1b[3J wipe) — seen by the user as duplicated/garbled
    //    prose stranded above the tool cards.
    //
    //    (a) TOOL CARDS on this message: the Anthropic wire closes the
    //        text block BEFORE tool_use streams, so this message's prose
    //        is wire-complete the instant a ToolUse exists — there is
    //        nothing left to type out. Meanwhile the card panel renders
    //        BELOW this markdown and grows (Pending → Running progress
    //        → Done), pushing the prose toward/past the viewport top.
    //        Three-part hardening, all idempotent:
    //        • snap_reveal_to_edge(): un-ghosts the tail (bumps
    //          build_dirty_ so the rebuild is immediate);
    //        • set_reveal_fx(false): stops the per-frame tip mutation
    //          (scramble glyphs, gradient restyle, pulsing caret) so
    //          every row that scrolls off is in its FINAL glyphs+style;
    //        • finish(): force-commits the TRAILING PARAGRAPH into the
    //          block path NOW. A message's last paragraph never receives
    //          its terminating \n\n, so without this it rides in
    //          render_tail's inline path for the whole tool phase and
    //          only converts to a committed block at turn settle — and
    //          the two paths wrap at (off-by-one) different widths, so
    //          the conversion REWRITES the paragraph's rows. If those
    //          rows crossed the viewport top during the tool phase
    //          (they routinely do — the growing card pushes them up),
    //          the settle-time rewrite hits immutable scrollback and
    //          maya's gate can only HardReset (\x1b[2J\x1b[3J), wiping
    //          and re-stranding the transcript — the oracle's
    //          t1-settle corruption and the user's "prose duplicated
    //          above the cards" screenshots. Committing here makes the
    //          rewrap happen while the paragraph is still at the
    //          viewport bottom (diff-repaintable), so its bytes are
    //          FINAL before any later frame can scroll them off.
    //        Post-tool prose arrives on a NEW placeholder Message (own
    //        widget, fx on), so nothing visible is lost — the animation
    //        simply doesn't outlive the prose it animates.
    {
        const bool has_cards = !msg.tool_calls.empty();

        // ── Pre-emptive end-of-text drain (kills the burst at its ROOT) ──
        //
        // The reveal cursor rides ~drain_secs behind the wire to smooth
        // jitter, so when the model closes its text block there is a
        // backlog (~wire_cps × drain_secs) still to type out. The instant
        // a tool_use arrives the guard below MUST hard-snap that backlog
        // to the edge (a growing card would otherwise strand any lagged
        // inline line — scrollback corruption, oracle-proven on ANY
        // glide). That snap is the visible BURST ("first char sticks, then
        // it all appears with the next tool").
        //
        // But on the wire the text block goes QUIET a beat before the
        // tool_use streams. During that gap there is NO card yet, so the
        // reveal can safely GLIDE to the edge — nothing below it can
        // scroll a lagged row off. Detect the gap (live, no cards, bytes
        // have stopped growing for a short window) and request_finalize:
        // the cursor sprints to the edge over a bounded ramp WHILE fx
        // stays on, so it reads as the typewriter catching up, not a
        // paste. By the time the tool_use lands the cursor is already at
        // the edge and the mandatory snap below is a NO-OP → zero burst.
        //
        // Safe against a mid-text pause (model stalls mid-sentence, not
        // actually done): if more bytes arrive after a premature drain,
        // the widget simply resumes revealing from the new edge — the
        // early catch-up looks like "caught up, waiting," never a burst,
        // and never touches scrollback (still no card). Gated on the same
        // since_grow window the RAF logic uses so it never fights active
        // streaming.
        constexpr std::int64_t kTextQuietMs = 120;
        const bool text_gone_quiet =
            !settled && !has_cards && cache.streaming->is_live()
            && cache.streaming->reveal_in_progress()
            && cache.last_grow_tick.time_since_epoch().count() != 0
            && since_grow_ms >= kTextQuietMs;
        if (text_gone_quiet || msg.text_block_closed)
            cache.streaming->request_finalize(/*ramp_ms=*/160);

        // Gate on is_live(): finish() assigns live_=false through the
        // Tracked<> wrapper, which bumps build_dirty_ on EVERY assignment
        // (even same-value) — running this block per frame would force a
        // full widget rebuild every frame of the whole tool phase (the
        // long-turn "md streaming becomes slow" lag). After the first
        // pass live_ is off, is_live() is false, and the block is skipped;
        // snap/fx-off are no-ops on a finished widget anyway.
        //
        // Why HARD SNAP before the card can paint: the tool card renders
        // BELOW this prose and starts GROWING the same frame it appears
        // (Pending → Running → Done). A glide with the card visible
        // leaves the reveal cursor ~drain_secs behind the live edge, so
        // the lines between cursor and edge are still inline
        // (uncommitted) — and the growing card pushes exactly those lines
        // into native scrollback before the cursor reaches them
        // (oracle-proven: 124 gate recoveries when this glided). The snap
        // eliminates the lag instantly so there is no un-swept inline
        // window for the card to strand.
        //
        // ── Tool-panel DEFERRAL (kills the boundary burst) ──
        //
        // The pre-emptive drain above rarely gets to run: on the wire,
        // content_block_stop(text) and content_block_start(tool_use) are
        // CONSECUTIVE SSE events — usually the same TCP segment — so
        // text_block_closed and has_cards become true in the SAME reduce
        // batch and the drain gets ZERO frames before a card exists. The
        // mandatory snap then pastes the whole wire_cps×drain_secs
        // backlog in one frame: the user-reported "md sticks then bursts
        // when a tool use happens" (tool_boundary_burst_probe reproduces
        // it at 4-10× the steady reveal rate for close→tool gaps
        // ≤ 80 ms, and unconditionally on transports that never emit
        // StreamTextBlockClosed).
        //
        // Resolution: while the cursor is still mid-glide, HOLD THE TOOL
        // PANEL OFF-SCREEN (cache.defer_tool_panel — consumed by
        // append_assistant_body_slots / turn_config_for_assistant_run
        // this same frame) and arm the finalize ramp. With nothing
        // rendering below the prose there is no growing element to push
        // a mid-reveal row past the viewport top — the only rows that
        // can cross it are the OLDEST, fully-revealed committed ones,
        // exactly the normal mid-stream case the oracle already proves
        // safe. The typewriter finishes its glide (typically 300-700 ms
        // of visible catch-up, the RateCursor's natural backlog decay),
        // reveal_in_progress() flips false, and THEN the trio runs —
        // snap is a no-op, fx drops, finish() commits the trailing
        // paragraph — and the panel appears below FINAL prose. Zero
        // paste, and the scrollback invariant (card never paints under
        // un-swept inline rows) holds by construction.
        //
        // Bounds and bail-outs — the deferral must never hide a card
        // indefinitely or delay an interaction:
        //   • LAST-MESSAGE gate: the glide is only scrollback-safe while
        //     NOTHING renders below this prose. The instant a following
        //     message exists (the post-tool sub-turn placeholder — its
        //     markdown/panel renders underneath and grows), a lagged
        //     inline row above it can be pushed past the viewport top
        //     mid-reveal — the exact corruption the hard snap prevents
        //     (oracle-proven: deferring under a growing successor added
        //     9 gate recoveries). So the defer holds only while this
        //     message is the live tail's bottom-most element.
        //   • ALL-PENDING + height-budget gate: the unhide frame grows
        //     the tail by the hidden panel's full height in ONE frame.
        //     Rows that cross the viewport top on a grow are committed
        //     AS PAINTED — if the grow ≥ viewport height, the old
        //     bottom-rule row (mutated into panel content by the unhide)
        //     crosses un-repainted → shadow mismatch → HardReset
        //     (oracle-proven at 60x18: a Running card accumulating 2×H
        //     of hidden progress recovered on unhide). A Pending card's
        //     preview is tail-windowed (height-bounded ~a dozen rows);
        //     Running progress / Done output are unbounded. So the defer
        //     holds only while EVERY card is still Pending AND the
        //     estimated hidden height fits comfortably inside the
        //     viewport. Production timing makes this the common case:
        //     tools flip Running only after finalize_turn, so the whole
        //     args-streaming window (the burst window) stays deferrable.
        //   • kMaxCardDeferMs hard cap: a pathological backlog (the
        //     adaptive ramp in request_finalize stretches to 2.5 s on
        //     tens-of-KB dumps) falls back to the hard snap after 1.5 s
        //     — a small residual paste is the lesser evil vs. a card
        //     that looks stuck. Timed from cache.card_defer_since.
        //   • pending_permission: a tool awaiting approval floats its
        //     permission card as its own live-tail row the same frame —
        //     the user must see WHAT they're approving, so the panel
        //     cannot lag the prompt. Snap immediately.
        // Tool EXECUTION is untouched — kick_pending_tools runs in the
        // reducer regardless of panel visibility; a fast tool may
        // already be Running/Done when its card first paints, which
        // reads as "typewriter finished, then the result landed".
        if (has_cards && !settled && cache.streaming->is_live()) {
            constexpr std::int64_t kMaxCardDeferMs = 1500;
            const auto defer_now = std::chrono::steady_clock::now();
            const std::int64_t deferred_ms =
                cache.card_defer_since.time_since_epoch().count() == 0
                    ? 0
                    : std::chrono::duration_cast<std::chrono::milliseconds>(
                          defer_now - cache.card_defer_since).count();
            const bool is_tail_bottom =
                !m.d.current.messages.empty()
                && &msg == &m.d.current.messages.back();
            // Hidden-height budget: every card must be Pending (bounded
            // tail-window preview) and the worst-case hidden rows
            // (~12/card incl. chrome) must fit well inside the viewport
            // so the unhide grow can never push the mutated seam row
            // past the commit boundary in one frame.
            bool all_pending = true;
            for (const auto& tc : msg.tool_calls)
                if (!tc.is_pending()) { all_pending = false; break; }
            const int est_hidden_rows =
                static_cast<int>(msg.tool_calls.size()) * 12;
            const bool hidden_fits =
                est_hidden_rows < ::maya::available_height() - 4;
            const bool can_defer =
                cache.streaming->reveal_in_progress()
                && is_tail_bottom
                && all_pending
                && hidden_fits
                && !m.d.pending_permission
                && deferred_ms < kMaxCardDeferMs;
            if (can_defer) {
                if (cache.card_defer_since.time_since_epoch().count() == 0)
                    cache.card_defer_since = defer_now;
                cache.defer_tool_panel = true;
                // Glide to the edge now — covers transports that never
                // emit StreamTextBlockClosed (the drain above may not
                // have fired) and re-arms idempotently when it did.
                cache.streaming->request_finalize(/*ramp_ms=*/160);
            } else {
                // Exit (glide done / cap hit / bail-out): run the trio
                // NOW. If we had been deferring, this is phase 1 of the
                // two-phase exit — the finish() mutations paint on a
                // frame where the panel is STILL hidden (mutation-only,
                // diff-repaintable); the panel unhides next frame as a
                // pure grow. If we never deferred (e.g. permission card
                // pending on arrival), show immediately — the original
                // single-frame behavior.
                const bool was_deferring = cache.defer_tool_panel;
                cache.streaming->snap_reveal_to_edge();
                cache.streaming->set_reveal_fx(false);
                cache.streaming->finish();
                cache.card_defer_since = {};
                if (was_deferring) {
                    cache.defer_exit_finished = true;   // unhide next frame
                    ::maya::request_animation_frame();
                } else {
                    cache.defer_tool_panel = false;
                }
            }
        } else if (has_cards && !settled && cache.defer_tool_panel) {
            if (!cache.defer_exit_finished) {
                // The deferral's finalize ramp completed and the widget
                // flipped live_ off ON ITS OWN (advance_reveal_cursor_'s
                // scramble-settle gate) before the exit above got a frame.
                // finish() must STILL run exactly once: it force-commits
                // the trailing paragraph out of render_tail's inline path,
                // whose off-by-one wrap vs the committed-block path is the
                // settle-time row rewrite ("t1-settle corruption"). Panel
                // stays hidden this frame (phase 1); unhides next (phase 2).
                cache.streaming->set_reveal_fx(false);
                cache.streaming->finish();
                cache.defer_exit_finished = true;
                ::maya::request_animation_frame();
            } else {
                // Phase 2: the finish-mutation frame has painted; the
                // panel now appears as a pure bottom-append grow.
                cache.defer_tool_panel   = false;
                cache.defer_exit_finished = false;
                cache.card_defer_since   = {};
            }
        } else {
            // No cards / settled / widget already finished — make sure a
            // stale defer can never hide a panel on a later frame.
            cache.defer_tool_panel    = false;
            cache.defer_exit_finished = false;
            cache.card_defer_since    = {};
        }
    }

    //    (b) Viewport HEIGHT SHRINK: the terminal autonomously pushes the
    //        top viewport rows into native scrollback. If the live reveal
    //        edge is among them it freezes stale (the height-resize
    //        corruption). A resize is a discrete user event, so rendering
    //        the tail fully-revealed for that one frame is imperceptible
    //        and leaves no ghosted row to strand. snap_reveal_to_edge is
    //        a no-op when settled / not reveal_fx / already at the edge.
    {
        const int cur_h = ::maya::available_height();
        if (cache.last_render_height > 0 && cur_h < cache.last_render_height
            && !settled)
            cache.streaming->snap_reveal_to_edge();
        cache.last_render_height = cur_h;
    }

    auto built = cache.streaming->build();

    // Keep the 16 ms frame armed while EITHER new bytes are actively
    // flowing (stream_in_motion) OR the widget's reveal cursor is still
    // gliding toward the live edge after a burst. The second condition is
    // what makes the typewriter continuous across a wire pause: the model
    // ships a burst then goes quiet for 100-200 ms, but the cursor still
    // has a backlog to type out — without re-arming here the loop would
    // fall to the Tick cadence and the cursor would jump on the next wake
    // (the "bursts, not continuous" symptom). build() advanced the cursor
    // just above, so reveal_in_progress() reflects this frame's state.
    //
    // Third condition — a background parse in flight (is_parsing()):
    // set_content_async hands a large divergent prefix to a worker and
    // keeps returning the PREVIOUS element tree until the result lands.
    // maybe_apply_async_ (which adopts the landed result) runs ONLY from
    // build(), i.e. only when view() runs, i.e. only when the visual hash
    // advances. If bytes pause mid-parse (a wire burst-then-quiet), the
    // hash stops advancing, build() stops being called, and the finished
    // parse sits unapplied — the tail freezes until the next delta or the
    // 100 ms Tick happens to wake the loop (the "md gets stuck then
    // bursts" stutter on big pastes / large reflows). Keeping the frame
    // armed while parsing makes build() keep polling so the result is
    // adopted the instant the worker finishes.
    //
    // Fourth condition — the widget is LIVE with reveal_fx (is_live()).
    // render_live_overlay_ animates the trailing-edge scramble / gradient
    // / pulsing caret EVERY frame the widget is live_, even when the
    // reveal cursor has caught up to the edge (backlog 0) and is just
    // waiting for the next token. The first three terms can ALL be false
    // in that pinned-but-live state during a slow mid-stream gap: bytes
    // aren't flowing (stream_in_motion can lapse if phase briefly leaves
    // Streaming during a tool round-trip AND the 3 s window expires),
    // the cursor is at the edge (reveal_in_progress false), no ramp, no
    // parse. Without this term the caret would stop pulsing and the turn
    // would look frozen even though the model is still working. Gating on
    // is_live() (the exact condition render_live_overlay_ animates under)
    // guarantees the caret keeps breathing for the whole live window,
    // independent of phase or any timeout — the same robustness principle
    // as the is_streaming() caret gate, applied to the widget's own live
    // state. Cost is the bounded ~0.1-0.4 ms no-content repaint, paid
    // only while a turn is genuinely live.
    const bool live_caret =
        !settled && cache.streaming->is_live();
    if (stream_in_motion
        || live_caret
        || (!settled && cache.streaming->reveal_in_progress())
        || cache.streaming->is_finalizing()
        || cache.streaming->is_parsing()) {
        ::maya::request_animation_frame();
    }

    if (stream_prof) {
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - prof_t0).count();
        static std::FILE* out =
            std::fopen("/tmp/agentty-stream-prof.log", "a");
        if (out) {
            std::fprintf(out,
                "[stream] src=%zu revealed=%zu settled=%d fastpath=%d "
                "build_us=%lld\n",
                source.size(), cache.revealed_size, settled ? 1 : 0,
                already_settled_into_cache ? 1 : 0,
                static_cast<long long>(us));
            std::fflush(out);
        }
    }

    // Live markdown body returned UNPADDED. Composer anti-bounce — the
    // stream-start indicator→first-content seam, the typewriter crossing
    // a block boundary, a tool card collapsing as it settles — is handled
    // autonomously in maya (Runtime::render): it tracks the live
    // transcript's running-max height and pads up to it across a
    // transient dip, decaying once the shrink proves real so idle carries
    // no dead space. maya owns that mechanism end-to-end; this body
    // element just renders the revealed extent honestly.
    return built;
}

// ── Per-speaker visual identity: rail color + glyph + display name.
//    Centralized so the rail color, the header glyph, and the bottom
//    streaming indicator stay in lockstep.
struct SpeakerStyle {
    maya::Color color;
    std::string glyph;
    std::string label;
};

SpeakerStyle speaker_style_for(Role role, const Model& m) {
    if (role == Role::User) {
        // User rail is `role_brand` (magenta) — distinct from code-reference
        // cyan and matching the composer's accent color when has-text, so
        // the user's typed message visually flows into their turn header.
        return {role_brand, "\xe2\x9d\xaf", "You"};                  // ❯
    }
    const auto& id = m.d.model_id.value;
    const auto caps = ModelCapabilities::from_id(id);
    maya::Color c;
    std::string label;
    // Model rails use ROLE colors (persistent identity), never status
    // colors. Opus is the bright-magenta variant so it's visually
    // distinguishable from the user-turn magenta (same hue family,
    // different intensity — flagship gets the brighter shade). Haiku
    // used to render in `success` (green) which collided with the ✓
    // done icon; bright_cyan keeps the "fast/agile" feel without the
    // status collision.
    bool anthropic_known = true;
    if      (caps.is_opus())   { c = role_brand_alt; label = "Opus";   } // bright_magenta
    else if (caps.is_sonnet()) { c = role_info;      label = "Sonnet"; } // blue
    else if (caps.is_haiku())  { c = code_path;      label = "Haiku";  } // bright_cyan
    else {
        // Non-Anthropic / local model: derive a short, title-cased name
        // from the raw id (`codellama:latest` → "Codellama",
        // `qwen2.5-coder:7b` → "Qwen2.5 Coder 7b"). No date-suffix
        // version extraction — that's Anthropic-id specific.
        c = highlight;                            // cyan (fallback)
        label = pretty_model_label(id);
        anthropic_known = false;
    }
    // Extract a short version run like "4-5" → "4.5" from Anthropic model
    // ids only. Reject segments longer than 2 digits so date suffixes
    // (8-digit YYYYMMDD on ids like "claude-sonnet-4-20250514") don't get
    // displayed as `Sonnet 4.20250514`. Segments are 1–2 digits
    // joined by `-`/`.`; once a 3-digit run appears we stop the
    // version at the boundary before it (so `4-5-20250514` → `4.5`,
    // `4-20250514` → `4` only).
    for (std::size_t i = 0; anthropic_known && i + 2 < id.size(); ++i) {
        char ch = id[i];
        if (ch >= '0' && ch <= '9') {
            char delim = id[i + 1];
            if ((delim == '-' || delim == '.') && id[i + 2] >= '0' && id[i + 2] <= '9') {
                std::size_t cursor = i;
                std::string ver;
                // Read 1–2 digits for the first segment.
                {
                    std::size_t start = cursor;
                    while (cursor < id.size() && id[cursor] >= '0' && id[cursor] <= '9'
                           && (cursor - start) < 2)
                        ++cursor;
                    ver.append(id, start, cursor - start);
                    // If more digits follow than 2, this is a date — bail.
                    if (cursor < id.size() && id[cursor] >= '0' && id[cursor] <= '9')
                        goto have_ver;
                }
                // Optional further segments, each 1–2 digits.
                while (cursor + 1 < id.size()
                       && (id[cursor] == '-' || id[cursor] == '.')
                       && id[cursor + 1] >= '0' && id[cursor + 1] <= '9')
                {
                    std::size_t sep_pos = cursor;
                    std::size_t start   = cursor + 1;
                    std::size_t end     = start;
                    while (end < id.size() && id[end] >= '0' && id[end] <= '9'
                           && (end - start) < 2)
                        ++end;
                    // If more than 2 digits in this segment, it's a date —
                    // stop BEFORE the separator so the prior version stands.
                    if (end < id.size() && id[end] >= '0' && id[end] <= '9') break;
                    ver += '.';
                    ver.append(id, start, end - start);
                    cursor = end;
                    (void)sep_pos;
                }
              have_ver:
                if (!ver.empty())
                    label += " " + ver;
                break;
            }
        }
    }
    return {c, "\xe2\x9c\xa6", std::move(label)};                    // ✦
}

// ── Trailing meta strip for the turn header — `12:34 · 4.2s · turn N`.
// `checkpoint` appends a subtle `· ↺ checkpoint` tag so a restore-point
// user turn reads as an ordinary turn carrying a marker, NOT a separate
// full-width divider widget hanging above the rail (which looked like
// chrome / a broken empty message).
std::string format_turn_meta(const Message& msg, int turn_num,
                             std::optional<float> elapsed_secs,
                             bool checkpoint = false) {
    std::string meta = timestamp_hh_mm(msg.timestamp);
    // Only surface elapsed when it's meaningful. A near-instant local-model
    // reply yields ~0s wall-clock, which `format_duration_compact` renders
    // as a useless "0ms"/"12ms" — drop anything under 100ms.
    if (elapsed_secs && *elapsed_secs >= 0.1f)
        meta += "  \xc2\xb7  " + format_duration_compact(*elapsed_secs);
    if (turn_num > 0)
        meta += "  \xc2\xb7  turn " + std::to_string(turn_num);
    if (checkpoint)
        meta += "  \xc2\xb7  \xe2\x86\xba checkpoint";   // ↺ checkpoint
    return meta;
}

// ── Compute the assistant turn's wall-clock elapsed: from previous
//    user message timestamp to this one.
std::optional<float> assistant_elapsed(const Message& msg, const Model& m) {
    if (msg.role != Role::Assistant) return std::nullopt;
    // Memoized: the result depends only on msg.timestamp and the prior
    // User message's timestamp — both immutable once this message
    // exists — so a settled value never changes. Without the memo the
    // reverse scan below is O(sub-turns since the last user turn) run
    // every frame for the run head, which grows with turn depth (the
    // whole in-flight turn sits in the live tail until settle). Cache
    // on the head message's per-message slot; return the cached value
    // on every subsequent frame.
    //
    // Access is PARTITION-SAFE. The head of a live run is frequently the
    // PINNED streaming edge; routing this memo through the settled
    // message_md() would migrate it out of the pinned set every frame,
    // defeating the eviction-immunity. Read via a non-migrating peek()
    // first; only when the slot exists and already holds the memo do we
    // return it. On the (rare) miss we must write, so we touch the slot
    // in whichever home it already lives — message_md_live() if pinned
    // (preserves the pin), message_md() otherwise — so the write never
    // moves the entry across the partition.
    if (const auto* mc = m.ui.view_cache.peek(m.d.current.id, msg.id);
        mc && mc->elapsed_valid)
        return mc->elapsed_cached;
    std::optional<float> result;
    for (std::size_t i = m.d.current.messages.size(); i-- > 0;) {
        if (&m.d.current.messages[i] == &msg) continue;
        if (m.d.current.messages[i].role == Role::User) {
            auto dt = std::chrono::duration<float>(
                msg.timestamp - m.d.current.messages[i].timestamp).count();
            if (dt > 0.0f && dt < 3600.0f) result = dt;
            break;
        }
    }
    auto& cache = m.ui.view_cache.is_pinned(m.d.current.id, msg.id)
        ? m.ui.view_cache.message_md_live(m.d.current.id, msg.id)
        : m.ui.view_cache.message_md     (m.d.current.id, msg.id);
    cache.elapsed_cached = result;
    cache.elapsed_valid  = true;
    return result;
}

// Append one Assistant Message's body slots (markdown + tools panel +
// inline permission) to `cfg.body`. Pulled out of turn_config so a run
// of consecutive Assistant Messages can be rendered as ONE Turn (see
// turn_config_for_assistant_run), matching agent_session's discipline
// where every internal seam contributes a body slot to one Turn.
//
// Tool panel emission is split from text emission so a run of
// consecutive sub-turn Messages (each with one tool, no text) renders
// as ONE merged panel rather than N stacked single-tool panels. The
// panel cache key is the `anchor_msg_id` — stable across rebuilds
// because messages are append-only, and unique per merged group
// because each group is anchored at the FIRST contributing Message.
// Build the actions panel fresh every frame (agent_session pattern).
// Settled assistant runs get snapshotted into m.ui.frozen by
// freeze_range — they never re-enter this function. Live panels are
// bounded by the in-flight turn's tool count, so per-frame cost is
// O(active_tools). One AgentTimeline carrier per panel, born here,
// dropped when cfg.body goes out of scope: no shared_ptr identity
// flip races, no spinner-bucket cache, no freeze fast path. The old
// freeze cache was dead weight — the Element it built is snapshotted
// straight into m.ui.frozen the same frame the slot is populated.
//
// Permission card is NO LONGER appended here. The host floats it as
// its own live_tail entry below the active assistant Turn (mirrors
// agent_session's `Permission` sibling under the root vstack).
void append_assistant_tool_panel(maya::Turn::Config& cfg,
                                 const Message& msg,
                                 std::span<const ToolUse> tool_calls,
                                 const Model& m,
                                 const SpeakerStyle& style)
{
    if (tool_calls.empty()) return;

    // Route settled sub-turn panels through the dedicated per-message
    // panel memo (g_panel_render_memo, agent_timeline.cpp). It is keyed
    // on this message's stable id + compute_render_key() so a settled
    // sub-turn is a single-uint64-compare hit — skipping even the
    // O(tools) content-key string build a bare g_panel_cache hit would
    // pay. That is what keeps per-frame view cost flat as an in-flight
    // run accumulates hundreds of settled sub-turns (they stay in the
    // live tail until settle and are re-emitted every frame). The memo
    // lives next to g_panel_cache, NOT in the RAM-bounded ViewCache, so
    // its depth is decoupled from markdown-tree retention. A running
    // tool advances the render_key each frame → natural miss → rebuild
    // (spinner animates), handled inside the memoized helper.
    const int frame = m.s.spinner.frame_index();
    cfg.body.emplace_back(agent_timeline_element_memoized(
        msg.id.value, msg.compute_render_key(),
        tool_calls, frame, style.color));
}

// Single-message body slot append: text (if any) then this message's
// tool panel (if any). Used by `turn_config` for non-run-merged
// renders (User turns delegate to a different branch; this is hit by
// the single-Message Assistant path that pre-dates the run merge).
void append_assistant_body_slots(maya::Turn::Config& cfg,
                                 const Message& msg,
                                 std::span<const ToolUse> tool_calls,
                                 const Model& m,
                                 const SpeakerStyle& style)
{
    const bool has_body = !msg.text.empty() || !msg.streaming_text.empty();
    if (has_body) {
        cfg.body.emplace_back(cached_markdown_for(msg, m));
        // Tool-panel deferral: cached_markdown_for just decided (this
        // same frame) whether the reveal cursor is still mid-glide with
        // a fresh tool card. Read via non-migrating peek() — the slot may
        // be PINNED (live), and message_md() here would migrate it out
        // of the pinned set. See the decision site for the mechanism.
        if (const auto* mc = m.ui.view_cache.peek(m.d.current.id, msg.id);
            mc && mc->defer_tool_panel)
            return;
    }
    append_assistant_tool_panel(cfg, msg, tool_calls, m, style);
}

} // namespace

maya::Turn::Config turn_config(const Message& msg, std::size_t msg_idx,
                               int turn_num, const Model& m,
                               bool continuation,
                               std::string_view meta_override,
                               std::span<const ToolUse> tool_calls_override) {
    // agent_session pattern: build a fresh Config every call. Settled
    // turns get their Element snapshotted into m.ui.frozen at freeze
    // time and rendered from there; the live tail rebuilds each frame
    // but is bounded to the in-flight turn. No Config / Element
    // memoization here.
    (void)msg_idx;

    // Tool-batch merge plumbing: when the caller passes an override
    // span, treat it as the effective tool_calls for this turn. Saves
    // an O(N) deep-copy of `msg` every frame on the live tail's merged
    // path — the originals stay in `m.d.current.messages[*]` and we
    // borrow them through the span.
    std::span<const ToolUse> tool_calls = tool_calls_override.empty()
        ? std::span<const ToolUse>{msg.tool_calls}
        : tool_calls_override;

    auto style = speaker_style_for(msg.role, m);

    maya::Turn::Config cfg;
    cfg.glyph        = style.glyph;
    cfg.label        = style.label;
    cfg.rail_color   = style.color;
    cfg.continuation = continuation;
    cfg.meta         = format_turn_meta(msg, turn_num,
                          msg.role == Role::Assistant
                              ? assistant_elapsed(msg, m)
                              : std::nullopt,
                          /*checkpoint=*/msg.role == Role::User
                              && msg.checkpoint_id.has_value());
    if (!meta_override.empty()) cfg.meta = std::string{meta_override};

    // Compact-boundary turn: rendered as a real, minimal SYSTEM turn
    // rather than a bare full-width divider (which read as chrome / a
    // stray rule floating in the transcript). It gets its own quiet
    // speaker identity — a `≡` glyph, a muted rail, a "Compacted"
    // label + timestamp meta — and a one-line body that says what
    // happened. This slots into the conversation as a legible event,
    // the same shape as a User/Assistant turn, so it no longer looks
    // broken.
    //
    // The model still receives the full summary text on the wire
    // (msg.text is untouched); the view deliberately elides the raw
    // summary prose (it's written for the model, can be many KB, and
    // would push the preserved tail off-screen the instant compaction
    // lands). CC does the same — its `compact_boundary` transcript line
    // renders as a marker, not the summary body.
    if (msg.is_compact_summary) {
        cfg.glyph      = "\xe2\x89\xa1";              // ≡
        cfg.label      = "Compacted";
        cfg.rail_color = muted;
        cfg.meta       = timestamp_hh_mm(msg.timestamp);
        cfg.body.emplace_back(maya::Turn::PlainText{
            .content = "Earlier conversation summarized to reclaim context.",
            .color   = muted});
        return cfg;
    }

    // Proactive-retrieval context turn: like the compact boundary, this is
    // a synthetic User message the MODEL sees in full (msg.text carries the
    // <retrieved-context> block) but the transcript renders as a quiet
    // one-liner so the raw passages don't dominate the user's view. Its
    // own muted identity (a book glyph, "Retrieved context" label) marks
    // it as auto-injected reference, not the user's words.
    if (msg.proactive_context) {
        // Parse the [source:path:line] headers out of the block so the
        // card can show the user WHAT grounded the answer — not just how
        // many passages. Each header is a line of the form
        //   [docs:path/to/file.md:12]  /  [skill:git:0]  /  [memory:...]
        // We collect the DISTINCT "source path" pairs (dropping the line
        // number and any duplicate chunks of the same file) so a file
        // that contributed three passages shows once. Cheap: one linear
        // scan of a bounded block.
        int n = 0;
        std::vector<std::string> sources;   // distinct "source path", in order
        for (std::size_t p = msg.text.find("\n["); p != std::string::npos;
             p = msg.text.find("\n[", p + 1)) {
            ++n;
            std::size_t open = p + 2;                       // past "\n["
            std::size_t close = msg.text.find(']', open);
            if (close == std::string::npos) continue;
            std::string tag = msg.text.substr(open, close - open);
            // Split "source:path:line" → "source" + "path" (drop trailing
            // ":line" if the last colon-field is all digits).
            std::size_t colon = tag.find(':');
            std::string src  = colon == std::string::npos
                                 ? std::string{"docs"} : tag.substr(0, colon);
            std::string path = colon == std::string::npos
                                 ? tag : tag.substr(colon + 1);
            if (std::size_t lc = path.rfind(':'); lc != std::string::npos) {
                std::string_view tail{path.data() + lc + 1,
                                      path.size() - lc - 1};
                if (!tail.empty()
                    && std::all_of(tail.begin(), tail.end(),
                                   [](unsigned char c){ return std::isdigit(c); }))
                    path.resize(lc);
            }
            std::string label = path.empty() ? src : (src + " · " + path);
            if (std::find(sources.begin(), sources.end(), label)
                    == sources.end())
                sources.push_back(std::move(label));
        }

        cfg.glyph      = "\xf0\x9f\x93\x9a";          // 📚
        cfg.label      = "Retrieved context";
        // Blue rail: this is a CONTEXT / reference turn (status_info axis),
        // which also lifts the whole card out of the flat muted gray.
        cfg.rail_color = status_info;
        // Passage count rides the meta line (right-aligned, like elapsed
        // time on assistant turns) so the body is free for the sources.
        const int shown_n = n > 0 ? n : 1;
        cfg.meta = timestamp_hh_mm(msg.timestamp) + "  \xc2\xb7  "
            + std::to_string(shown_n)
            + (shown_n == 1 ? " passage" : " passages");

        cfg.body.emplace_back(maya::Turn::PlainText{
            .content = "Grounded the answer in your knowledge base:",
            .color   = muted});

        // Confidence bar — a compact 10-cell gauge of the retrieval
        // confidence that cleared the injection floor, colored by level so
        // the user reads trust at a glance: green (strong), yellow
        // (moderate), muted (weak). Filled cells carry the level color;
        // empty cells stay muted. Only drawn when a real value was threaded
        // through (>= 0); older/cached proactive messages skip the bar.
        if (msg.proactive_confidence >= 0.0) {
            using namespace maya::dsl;
            const double c = msg.proactive_confidence > 1.0
                                 ? 1.0 : msg.proactive_confidence;
            constexpr int kCells = 10;
            const int filled = static_cast<int>(c * kCells + 0.5);
            const maya::Color lvl = c >= 0.60 ? status_ok
                                  : c >= 0.35 ? status_warn
                                              : muted;
            std::string on, off;
            for (int i = 0; i < filled; ++i)          on  += "\xe2\x96\xb0";
            for (int i = filled; i < kCells; ++i)     off += "\xe2\x96\xb1";
            const int pct = static_cast<int>(c * 100.0 + 0.5);
            cfg.body.emplace_back(maya::Turn::BodySlot{
                h(text("  confidence ", fg_of(muted)),
                  text(on,  fg_of(lvl)),
                  text(off, fg_of(muted)),
                  text(" " + std::to_string(pct) + "%", fg_bold(lvl)))
                    .build()});
        }

        // One row per distinct source — a tree-ish "└ " bullet + a muted
        // "source ·" prefix, with the PATH in code-reference cyan so it
        // pops as the actionable provenance. Capped so a wide multi-file
        // hit can't dominate the transcript; overflow collapses to a
        // "…and N more" tail.
        {
            using namespace maya::dsl;
            constexpr std::size_t kMaxSources = 6;
            const std::size_t total = sources.size();
            for (std::size_t i = 0; i < sources.size() && i < kMaxSources; ++i) {
                // sources[i] is "src · path" (or just "src"); split on the
                // first " · " so the path can take the cyan.
                const std::string& s = sources[i];
                std::string pre = s, path;
                if (auto d = s.find(" \xc2\xb7 "); d != std::string::npos) {
                    pre  = s.substr(0, d + 4);   // include " · " separator
                    path = s.substr(d + 4);
                }
                cfg.body.emplace_back(maya::Turn::BodySlot{
                    h(text("  \xe2\x94\x94 ", fg_of(muted)),   // └
                      text(pre,  fg_of(muted)),
                      text(path, fg_of(code_path)))
                        .build()});
            }
            if (total > kMaxSources) {
                cfg.body.emplace_back(maya::Turn::PlainText{
                    .content = "  \xe2\x80\xa6 and "                // …
                        + std::to_string(total - kMaxSources) + " more",
                    .color   = muted});
            }
        }
        return cfg;
    }

    if (msg.role == Role::User) {
        // Substitute chip placeholders (\x01ATT:N\x01) with their
        // human-readable captions so a 400-line paste renders as
        // "[Pasted text · 412 lines · 14 KB]" in the transcript
        // instead of inlining the whole body. The wire still sees
        // the full bytes — the transport calls attachment::expand()
        // at request-build time. Image placeholders consult
        // msg.attachments (which still holds an entry per image with
        // path/media_type/byte_count populated even after the bytes
        // were lifted onto msg.images), so the same chip label
        // formula used in the composer applies here verbatim.
        std::string display;
        if (msg.attachments.empty()) {
            display = msg.text;
        } else {
            display.reserve(msg.text.size());
            std::size_t i = 0;
            while (i < msg.text.size()) {
                if (static_cast<unsigned char>(msg.text[i]) == attachment::kSentinel) {
                    auto len = attachment::placeholder_len_at(msg.text, i);
                    if (len > 0) {
                        auto idx = attachment::placeholder_index(msg.text, i);
                        if (idx < msg.attachments.size()) {
                            display.push_back('[');
                            display.append(attachment::chip_label(msg.attachments[idx]));
                            display.push_back(']');
                        }
                        i += len;
                        continue;
                    }
                }
                display.push_back(msg.text[i++]);
            }
        }
        cfg.body.emplace_back(maya::Turn::PlainText{.content = std::move(display), .color = fg});
    } else if (msg.role == Role::Assistant) {
        append_assistant_body_slots(cfg, msg, tool_calls, m, style);
        if (msg.error) cfg.error = *msg.error;
    }

    return cfg;
}

maya::Turn::Config turn_config_for_assistant_run(
    std::size_t run_first, std::size_t run_end,
    int turn_num, const Model& m)
{
    const auto& msgs = m.d.current.messages;
    // Pre-conditions defended at the only two call sites (build_live_tail
    // / freeze_range), but guard anyway so this function can be reused
    // without subtle row-shape corruption if the range ever turns out empty.
    if (run_first >= run_end || run_first >= msgs.size())
        return {};
    const std::size_t end = std::min(run_end, msgs.size());

    const Message& head = msgs[run_first];
    auto style = speaker_style_for(head.role, m);

    maya::Turn::Config cfg;
    cfg.glyph        = style.glyph;
    cfg.label        = style.label;
    cfg.rail_color   = style.color;
    cfg.meta         = format_turn_meta(head, turn_num,
                          head.role == Role::Assistant
                              ? assistant_elapsed(head, m)
                              : std::nullopt);

    if (head.role != Role::Assistant) {
        // Defensive: only Assistant runs use the multi-message path.
        // For a User head this collapses to the single-message build.
        return turn_config(head, run_first, turn_num, m,
                           /*continuation=*/false,
                           /*meta_override=*/{},
                           /*tool_calls_override=*/{});
    }

    // Walk the run, emitting one tool panel per Message that carries
    // tools. agent_session's discipline: one tool batch = one panel,
    // flushed when the next text block starts OR the run ends. In
    // agentty terms a "batch" is the tools attached to a single
    // Anthropic Message; the post-tool placeholder model gives us
    // one Message per sub-turn already, so each Message's tool_calls
    // are conceptually one batch (the model's reply text for that
    // sub-turn either precedes them in the same Message or arrives
    // on the following Message).
    //
    // Order in the rendered body mirrors wire order: text(i)
    // → panel(i) → text(i+1) → panel(i+1) …  Sub-turns whose only
    // contribution is a tool batch (no text) emit just a panel.
    // No cross-Message merging: every sub-turn gets its own panel,
    // matching agent_session where each ev::ToolEnd batch becomes
    // its own actions_panel(...).
    std::string error_accum;

    // ── Emit one sub-turn's body slots into `into`. Factored out of
    //    the run loop so the settled-prefix hoist below can reuse the
    //    EXACT same slot construction (byte-identical rows) inside a
    //    nested Turn.
    auto emit_subturn = [&](std::size_t idx, maya::Turn::Config& into) {
        const Message& m_i = msgs[idx];
        const bool has_text = !m_i.text.empty() || !m_i.streaming_text.empty();
        bool defer_panel = false;
        if (has_text) {
            into.body.emplace_back(cached_markdown_for(m_i, m));
            // Same-frame deferral handshake as append_assistant_body_slots:
            // cached_markdown_for just decided (this frame) whether the
            // reveal cursor is still mid-glide with a fresh tool card.
            // Read the flag via a non-migrating peek() — cached_markdown_for
            // may have PINNED this slot (it's live), and re-accessing it
            // through the settled message_md() here would migrate it back
            // out of the pinned set, un-pinning the very edge we just
            // protected. peek() reads from whichever home it lives in
            // without perturbing the partition.
            if (const auto* mc = m.ui.view_cache.peek(m.d.current.id, m_i.id))
                defer_panel = mc->defer_tool_panel;
        }
        if (!m_i.tool_calls.empty() && !defer_panel) {
            append_assistant_tool_panel(
                into,
                m_i,
                std::span<const ToolUse>{m_i.tool_calls},
                m, style);
        }
        if (m_i.error && error_accum.empty()) error_accum = *m_i.error;
    };

    // ── Per-sub-turn stable-identity slots (flat per-frame cost + a
    //    corruption-proof live tail vs. turn depth). ──
    //
    //    An in-flight assistant run can accumulate hundreds of settled
    //    sub-turns that all sit in the live tail until the whole run
    //    settles and freezes (freeze happens only at turn-settle; there
    //    is no sound mid-run freeze — see frozen.cpp). So the live tail
    //    grows unbounded during a deep autopilot run, and its leading
    //    sub-turns overflow into the terminal's IMMUTABLE native
    //    scrollback while the run is still live. Two requirements fall
    //    out of that, and a single mechanism satisfies both:
    //
    //      (1) FLAT per-frame cost: a settled sub-turn must not be
    //          rebuilt/re-walked from scratch every frame, or per-frame
    //          CPU grows O(turn depth).
    //      (2) APPEND-ONLY scrollback: once a sub-turn's rows have
    //          committed to native scrollback they can NEVER be re-
    //          emitted at a shifted position or under a changed element
    //          identity — maya's inline diff treats any committed-row
    //          mutation as uncorrectable and HardResets (\x1b[2J\x1b[3J),
    //          wiping+restranding the transcript. THIS is the user's
    //          report: "a running tool card doesn't show, then the reply
    //          and the prior cards render broken."
    //
    //    The mechanism: wrap EACH settled sub-turn in its OWN bare Turn
    //    keyed on a STABLE per-sub-turn hash (its message id + its own
    //    compute_render_key). Consequences:
    //
    //      • Stable identity for life. A sub-turn's slot occupies a
    //        fixed body index (its ordinal in the run) and, once the
    //        sub-turn settles, a fixed hash_id — for every frame until
    //        it freezes. maya blits it from its component cache (flat
    //        cost) and never re-emits it (append-only safe). A settling
    //        sub-turn just goes from "hash changes each frame" (live) to
    //        "hash frozen" — the exact lifecycle maya already handles
    //        for the whole live tail.
    //      • NO regrouping, ever. The earlier growing-prefix and
    //        fixed/row chunk designs both MOVED a sub-turn from an eager
    //        top-level slot INTO a differently-keyed group Turn as the
    //        boundary advanced. On a viewport short enough that the sub-
    //        turn had already overflowed, that move rewrote a committed
    //        row → HardReset. Per-sub-turn slots never move between
    //        containers, so that transition cannot occur at ANY depth or
    //        terminal size (scrollback_oracle deep_run_turn: green on
    //        every shape).
    //
    //    Robustness carve-outs (a slot must stay per-frame REBUILT, not
    //    stably keyed, while any of these hold — else a card vanishes or
    //    an animation stalls):
    //      • A non-terminal tool (still running): its render_key already
    //        advances every frame (progress/elapsed), so keying it is a
    //        natural miss+rebuild anyway; we still stamp the key so the
    //        blit engages the instant it settles.
    //      • A live/finalizing/revealing/parsing reveal widget: the hash
    //        is invariant across the scramble→clean transition, so a
    //        stable key mid-reveal would freeze scramble glyphs. Keep
    //        such a sub-turn UNKEYED (rebuild every frame) until drained.
    //      • An active tool-panel DEFER machine (defer_tool_panel /
    //        defer_exit_finished / card_defer_since): the defer flag is
    //        per-frame mutable state NOT folded into the render key, and
    //        its two-phase exit only advances while cached_markdown_for
    //        runs every frame. A stable key would (a) bake a panel-hidden
    //        body under a key the defer-clear never bumps → card
    //        invisible forever, and (b) stall the exit machine. Keep it
    //        UNKEYED until the defer machine is idle.
    //
    //    Byte-identity with the flat build (freeze_range) and the whole-
    //    run-hash live-tail path holds by construction: each bare Turn
    //    reuses maya::Turn's own is_blank-gated body assembly and sits as
    //    a top-level body slot, so build_inner inserts exactly one gap
    //    between adjacent non-blank slots — the same rows as emitting the
    //    sub-turn's markdown/panel inline.
    auto subturn_stably_keyable = [&](std::size_t j) -> bool {
        const auto& mj = msgs[j];
        if (mj.role != Role::Assistant) return false;
        // Live wire bytes still arriving → this is the streaming edge (or
        // a sub-turn mid-stream). Its reveal_fx animates the scramble /
        // gradient / caret BETWEEN byte arrivals with NO size change, so
        // a content-keyed hash would freeze between deltas: maya blits the
        // cached bare Turn, cached_markdown_for never re-runs, its
        // request_animation_frame() is never re-armed, and the typewriter
        // FREEZES until an unrelated hash axis flips (the low-CPU "md gets
        // stuck mid-turn" report). Must be built inline so its per-frame
        // builder keeps running. Checked FIRST — independent of whether
        // msg.text has any settled prefix yet.
        if (!mj.streaming_text.empty() || !mj.pending_stream.empty())
            return false;
        for (const auto& tc : mj.tool_calls)
            if (!tc.is_terminal()) return false;
        // Non-migrating peek(): this is a READ-ONLY state probe. Routing
        // it through message_md() would migrate a pinned live entry down
        // into the settled map — exactly the un-pin we must avoid. A slot
        // that doesn't exist yet is trivially not animating (no widget),
        // so nullptr → keyable-so-far.
        const auto* mc = m.ui.view_cache.peek(m.d.current.id, mj.id);
        if (mc && (mc->defer_tool_panel || mc->defer_exit_finished
                || mc->card_defer_since.time_since_epoch().count() != 0))
            return false;
        // Reveal widget still animating (live / finalize ramp / cursor
        // gliding backlog / async parse) → same freeze hazard as live
        // bytes: the hash is invariant across the scramble→clean
        // transition, so keying it would strand the animation. Applies
        // whether or not text has committed — a message can have a
        // settled text prefix with the widget still live_ during the
        // finalize ramp, so DON'T gate this on text being non-empty.
        if (mc && mc->streaming
            && (mc->streaming->is_live()
             || mc->streaming->is_finalizing()
             || mc->streaming->reveal_in_progress()
             || mc->streaming->is_parsing()))
            return false;
        return true;
    };

    // ── Live-edge protection is STRUCTURAL, not a manual touch. ──
    //
    //    The loop below calls cached_markdown_for() once per sub-turn
    //    that has text — O(depth) distinct (thread,msg) accesses per
    //    frame. Previously that walk shared ONE LRU with the live edge,
    //    so once depth exceeded the cap the walk evicted the oldest
    //    entries as it inserted new ones — and the streaming edge (last
    //    message, touched last) sat at the LRU back when the walk began.
    //    Evicting it destroyed its StreamingMarkdown widget + reveal
    //    bookkeeping, restarting the reveal from scratch and stalling the
    //    typewriter (the "md animation not smooth in a long turn"
    //    report). The old fix was a belt-and-braces manual touch of the
    //    edge BEFORE the walk plus a bumped cap so realistic depth
    //    wouldn't thrash — both mitigations of a shared-LRU hazard.
    //
    //    That hazard no longer exists, and there is no LRU at all now.
    //    cached_markdown_for routes a live message (live wire bytes OR an
    //    animating widget) through the cache's PINNED map, which is never
    //    evicted; settled sub-turns go in a separate staging map that is
    //    never evicted either — it is DROPPED per-message at freeze (see
    //    cache.hpp / freeze_range). No walk depth can touch a live edge,
    //    and no walk depth grows memory: both maps are bounded by the
    //    active turn. The stall class is unrepresentable, not merely
    //    unlikely. Nothing to pre-touch here.

    for (std::size_t i = run_first; i < end; ++i) {
        if (msgs[i].role != Role::Assistant) break;   // run boundary
        if (subturn_stably_keyable(i)) {
            // Settled sub-turn: emit as its own bare Turn with a stable
            // per-sub-turn hash_id. maya caches + blits it every frame
            // and never re-emits it once its rows commit to scrollback.
            maya::Turn::Config sub;
            sub.bare       = true;
            sub.rail_color = style.color;
            emit_subturn(i, sub);
            if (sub.body.empty()) continue;   // nothing to show
            sub.hash_id = maya::CacheIdBuilder{}
                .add(std::string_view{"agentty.turn.subturn"})
                .add(std::string_view{msgs[i].id.value})
                .add(msgs[i].compute_render_key())
                .build();
            cfg.body.emplace_back(maya::Turn{std::move(sub)}.build());
        } else {
            // Live / animating / deferring sub-turn: build inline into
            // the outer Turn so its side-effecting per-frame builders
            // (reveal cursor, defer exit machine, spinner) keep running.
            emit_subturn(i, cfg);
        }
    }

    if (!error_accum.empty()) cfg.error = std::move(error_accum);

    return cfg;
}

std::size_t turn_run_end(const std::vector<Message>& messages,
                         std::size_t from)
{
    if (from >= messages.size()) return from;
    if (messages[from].role != Role::Assistant) return from + 1;
    std::size_t end = from + 1;
    while (end < messages.size()
           && messages[end].role == Role::Assistant) {
        ++end;
    }
    return end;
}

maya::CacheId assistant_run_hash_id(
    const Model& m, std::size_t run_start, std::size_t run_end)
{
    const auto& msgs = m.d.current.messages;
    maya::CacheIdBuilder kb;
    kb.add(std::string_view{"agentty.turn.assistant_run"})
      .add(static_cast<std::uint64_t>(run_end - run_start));
    for (std::size_t j = run_start; j < run_end && j < msgs.size(); ++j) {
        kb.add(std::string_view{msgs[j].id.value});
        kb.add(msgs[j].compute_render_key());
    }
    return kb.build();
}


} // namespace agentty::ui
