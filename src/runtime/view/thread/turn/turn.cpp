#include "agentty/runtime/view/thread/turn/turn.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <maya/widget/agent_timeline.hpp>
#include <maya/widget/markdown.hpp>
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
    auto& cache = m.ui.view_cache.message_md(m.d.current.id, msg.id);

    if (!cache.streaming) {
        cache.streaming = std::make_shared<maya::StreamingMarkdown>();
        // Animated live tail: gradient trail + scramble→resolve + pulsing
        // caret on the streaming edge (maya reveal_fx). Only animates while
        // the widget is live_; the settled build is untouched.
        cache.streaming->set_reveal_fx(true);
        // Reveal pacing for the constant-glide cursor (the ChatGPT / Vercel
        // smoothStream model): the cursor drains the arrived-bytes BUFFER at
        // a near-constant cruising speed and lets the buffer absorb the wire's
        // burstiness — instead of tracking the (spiky) backlog. So the args
        // mean:
        //   • floor_cps = the CRUISING speed. This is the speed the text
        //     actually glides at almost all the time. It must be at LEAST
        //     the rate a fast cloud model sustains (~80-200 cp/s) or the
        //     cursor falls permanently behind the wire and the finalize ramp
        //     has to sprint a huge backlog at settle — which reads as "it
        //     hung, then dumped the whole reply at once". 90 cp/s cruises
        //     fast enough to track most models in real time while still
        //     looking like a deliberate typewriter glide, not an instant
        //     paste. The cursor auto-tunes UP from here for a model that
        //     sustains an even bigger buffer, but drifts slowly (base_tau)
        //     so the speed never steps at a chunk boundary.
        //   • drain_secs = the target LEAD window (how far behind the live
        //     edge the cursor comfortably rides). 0.3s of buffer is enough to
        //     smooth out SSE chunk jitter while keeping the reveal tight to
        //     the wire so it never visibly lags.
        // The instantaneous rate is hard-capped at 1.8× cruising (set in
        // RateCursor) so even a huge burst can't make the cursor sprint — it
        // just leans forward briefly and keeps gliding. The settle deadline
        // ramp still bypasses all caps so the tail always lands on time.
        cache.streaming->set_reveal_pacing(/*cruise_cps=*/90.0,
                                           /*lead_secs=*/0.3);
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
        return cache.streaming->build();
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
            // Use the async variant: tiny appends stay on the sync
            // incremental path inside StreamingMarkdown (cheap), but a
            // diverging-prefix swap of >=16 KB (loading an old thread,
            // recovering scrollback, pasting a long markdown body)
            // gets offloaded to a worker so the render thread doesn't
            // stall on the parse. While the worker is in flight the
            // widget keeps returning its previous element tree, so
            // there's no visible blank during the handoff.
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
                cache.streaming->request_finalize(200);
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
std::string format_turn_meta(const Message& msg, int turn_num,
                             std::optional<float> elapsed_secs) {
    std::string meta = timestamp_hh_mm(msg.timestamp);
    // Only surface elapsed when it's meaningful. A near-instant local-model
    // reply yields ~0s wall-clock, which `format_duration_compact` renders
    // as a useless "0ms"/"12ms" — drop anything under 100ms.
    if (elapsed_secs && *elapsed_secs >= 0.1f)
        meta += "  \xc2\xb7  " + format_duration_compact(*elapsed_secs);
    if (turn_num > 0)
        meta += "  \xc2\xb7  turn " + std::to_string(turn_num);
    return meta;
}

// ── Compute the assistant turn's wall-clock elapsed: from previous
//    user message timestamp to this one.
std::optional<float> assistant_elapsed(const Message& msg, const Model& m) {
    if (msg.role != Role::Assistant) return std::nullopt;
    for (std::size_t i = m.d.current.messages.size(); i-- > 0;) {
        if (&m.d.current.messages[i] == &msg) continue;
        if (m.d.current.messages[i].role == Role::User) {
            auto dt = std::chrono::duration<float>(
                msg.timestamp - m.d.current.messages[i].timestamp).count();
            if (dt > 0.0f && dt < 3600.0f) return dt;
            return std::nullopt;
        }
    }
    return std::nullopt;
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
                                 std::span<const ToolUse> tool_calls,
                                 const Model& m,
                                 const SpeakerStyle& style)
{
    if (tool_calls.empty()) return;
    const int frame = m.s.spinner.frame_index();
    cfg.body.emplace_back(agent_timeline_element(tool_calls, frame, style.color));
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
    }
    append_assistant_tool_panel(cfg, tool_calls, m, style);
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
                              : std::nullopt);
    if (!meta_override.empty()) cfg.meta = std::string{meta_override};
    cfg.checkpoint_above = (msg.role == Role::User && msg.checkpoint_id.has_value());
    cfg.checkpoint_color = warn;

    // Compact-boundary turn: a thin one-line divider only. The
    // summary body itself can be many KB / dozens of rows when the
    // model is verbose, and rendering it inline would (a) push the
    // preserved-tail and any subsequent assistant turn off-screen
    // immediately after compaction lands and (b) tempt the user to
    // read it (it's prose written for the model, not for them). The
    // model still receives the full summary text on the wire because
    // msg.text isn't mutated; the view just elides it. CC does the
    // same — its `compact_boundary` transcript line type renders as
    // chrome, not content (binary near offset 114920224).
    if (msg.is_compact_summary) {
        cfg.glyph      = "\xe2\x89\xa1";              // ≡
        cfg.label      = "Conversation compacted";
        cfg.rail_color = muted;
        // Empty body → the Turn frame collapses to just the header
        // row + bottom rule, ~2 rows total. The user sees a clear
        // divider where the boundary is and nothing more.
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

    for (std::size_t i = run_first; i < end; ++i) {
        const Message& m_i = msgs[i];
        if (m_i.role != Role::Assistant) break;   // run boundary

        const bool has_text = !m_i.text.empty() || !m_i.streaming_text.empty();
        if (has_text) {
            cfg.body.emplace_back(cached_markdown_for(m_i, m));
        }
        if (!m_i.tool_calls.empty()) {
            append_assistant_tool_panel(
                cfg,
                std::span<const ToolUse>{m_i.tool_calls},
                m, style);
        }
        if (m_i.error && error_accum.empty()) error_accum = *m_i.error;
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
