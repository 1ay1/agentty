// Composer-submission, thread-virtualization, and settings-persistence
// helpers for the update reducer. submit_message is the entry point for
// ComposerEnter / ComposerSubmit and is also called from finalize_turn when
// flushing the composer's queued-message buffer, which is why it lives in a
// shared internal header rather than an anonymous namespace.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include "agentty/runtime/app/cmd_factory.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/composer_attachment.hpp"
#include "agentty/store/store.hpp"

namespace agentty::app::detail {

namespace {

// When advancing thread_view_start past kSliceChunk old messages, we tell
// maya's inline renderer how many rows of prev_cells to commit to the
// terminal's native scrollback (Cmd::commit_scrollback_overflow →
// Runtime::commit_inline_overflow).
//
// The discipline is asymmetric:
//
//   over-commit  — the renderer "forgets" rows still on screen as
//                  immutable scrollback; the next frame's diff is
//                  misaligned, the renderer extends the live region down
//                  by emitting \r\n at terminal bottom, which scrolls
//                  rows of NEW content into native scrollback at the top
//                  → text ghosting in scrollback.
//
//   under-commit — prev_cells keeps a few extra rows that no longer
//                  match the live frame; the next frame's shrink path
//                  emits cleanly bounded EL erases for the abandoned
//                  region. Bounded cost, no ghosting. (See agent_session.cpp
//                  in the maya repo for the same discipline applied to a
//                  pre-rendered `frozen` element list.)
//
// So any value passed to commit_scrollback MUST be a conservative LOWER
// BOUND on the actual rendered rows of messages
// [old_thread_view_start, new_thread_view_start). It must never exceed
// the actual height.
//
// History — estimates that failed:
//
//   1. `estimate_message_rows` (`kEstWidth = 100`, per-byte arithmetic
//      on `msg.text`). Over-committed at width > 100, on UTF-8
//      multi-byte content, and on elided tool-body previews.
//
//   2. `kRowsPerDroppedMessageLower = 2`. A captured profile of an
//      800-row session showed `maybe_virtualize` drop 8 messages while
//      content_rows shrank only 5 rows on the next render —
//      per-message live-frame residue averaged 0.6. Committing 8*2=16
//      rows over-committed by 11, ghosting border fragments and blank
//      rectangles into native scrollback 2–3 turns into long streaming
//      sessions.
//
//   3. `kRowsPerDroppedMessageLower = 0`. Provably safe (zero is always
//      a valid under-bound), but leaves prev_cells growing
//      monotonically with the cumulative content_rows of the whole
//      session — ~1–2 MB resident for a 200-turn conversation, plus a
//      memmove cost the moment a non-zero commit finally happens.
//
// Current discipline: ask maya to commit every row that's PROVABLY
// already overflowed the viewport. The renderer knows two numbers we
// don't (prev_rows from the last compose and term_h); their difference
// `max(0, prev_rows - term_h)` is an exact lower bound on rows that
// have already been scrolled into native scrollback as immutable
// cells (the bottom term_h rows of prev_cells are everything still on
// screen; everything above them overflowed). Maya computes the
// number itself; agentty just signals "please commit what's safe."
//
// Result: prev_cells stays bounded to roughly term_h rows in
// steady-state for the inline session, the natural shrink path in
// compose_inline_frame still handles the residue of dropped messages
// that hadn't yet scrolled out, and there's no estimator to drift
// from reality on a future content-mix change.

} // namespace

maya::Cmd<Msg> maybe_virtualize(Model& m) {
    using maya::Cmd;
    const int total = static_cast<int>(m.d.current.messages.size());
    const int visible = total - m.ui.thread_view_start;
    // Only slice in discrete chunks — a one-per-turn slice would refresh
    // the visible area every turn, whereas chunking it causes one refresh
    // every kSliceChunk turns.
    if (visible <= kViewWindow + kSliceChunk) return Cmd<Msg>::none();

    int committed_turns = 0;
    for (int i = m.ui.thread_view_start; i < m.ui.thread_view_start + kSliceChunk; ++i) {
        if (m.d.current.messages[i].role == Role::Assistant) ++committed_turns;
    }
    m.ui.thread_view_start      += kSliceChunk;
    m.ui.thread_view_start_turn += committed_turns;
    return Cmd<Msg>::commit_scrollback_overflow();
}

Step submit_message(Model m) {
    using maya::Cmd;
    // Composer is non-empty if it has typed text OR an attachment chip.
    // Even an "empty-looking" buffer with chips should submit — those
    // chips ARE the message (a single dropped @file or paste, with no
    // surrounding prose). The expand pass below pulls each chip's body
    // into the wire text.
    if (m.ui.composer.text.empty() && m.ui.composer.attachments.empty())
        return done(std::move(m));

    // Drain composer.text + composer.attachments into a single fully
    // expanded payload string, resetting composer fields. Used by the
    // queue-on-busy and queue-on-compact paths and by the actual
    // submit path below — all three need the same "linearise chips
    // now, attachments vector becomes empty" semantics so a Recall
    // (Up arrow) of a queued item never resurrects a placeholder
    // pointing at a dropped index.
    auto drain_composer = [](Model& mm) {
        std::string out = mm.ui.composer.attachments.empty()
            ? std::exchange(mm.ui.composer.text, {})
            : attachment::expand(mm.ui.composer.text, mm.ui.composer.attachments);
        mm.ui.composer.text.clear();
        mm.ui.composer.attachments.clear();
        mm.ui.composer.cursor = 0;
        // Submit boundary clears the per-draft transient state. Undo
        // / redo and the history-walk index belong to the draft the
        // user just sent; carrying them into the next draft would
        // produce surprising "Ctrl+Z restores half of last turn".
        mm.ui.composer.undo_stack.clear();
        mm.ui.composer.redo_stack.clear();
        mm.ui.composer.history_idx = -1;
        mm.ui.composer.draft_save.reset();
        return out;
    };

    // Belt-and-suspenders: queue if any non-Idle phase is in flight.
    // The bare check (Streaming || ExecutingTool) was correct in
    // practice — the keymap routes Esc/y/n/a to the permission modal
    // when `pending_permission.has_value()`, so an AwaitingPermission
    // phase can't reach a ComposerEnter dispatch — but `active()` /
    // `!is_idle()` makes the guarantee structural instead of relying
    // on two separate gating layers staying in sync. Future addition
    // of new phases (or a refactor that lets the composer stay live
    // during AwaitingPermission) won't silently regress to "submit
    // overwrites the active ctx".
    //
    // Also queue while a background OAuth refresh is in flight. Deps
    // still holds the pre-refresh (expired) auth header until the
    // TokenRefreshed handler swaps it; firing a stream now would 401.
    // The handler drains this queue once new creds are live.
    if (m.s.active() || m.s.oauth_refresh_in_flight) {
        m.ui.composer.queued.push_back(drain_composer(m));
        return done(std::move(m));
    }

    // Auto-compaction trigger. Threshold is **80% of the active
    // model's context window** — Sonnet's 200k → trips at 160k, the
    // 1M-token Sonnet variants → trip at 800k, future models scale
    // automatically. m.s.context_max is set on model selection from
    // ui::context_max_for_model(); a 20% safety margin gives the
    // compaction round + the user's queued message room to fit
    // before the actual ceiling.
    //
    // Inspired by Claude Code 2.1.119's BetaToolRunner.compactionControl
    // (binary near offset 134600), but its hardcoded 1e5 default was
    // wrong-direction-pessimistic for Anthropic's bigger models — for
    // a 200k context it would compact at 50%, throwing away half the
    // window's worth of accumulated context for no reason. Scaling
    // with the model fixes that.
    //
    // Falls back to no-trigger when context_max is unset / zero (a
    // freshly-constructed Model before model selection has run).
    // autocompact_disabled is set by the rapid-refill breaker in
    // stream.cpp when three compacts in a row land within three
    // assistant turns of each other. Skipping the auto-trigger here
    // avoids the compact→fill→compact thrash that surfaces on a
    // huge tool output the model keeps re-reading. The user can
    // still trigger compaction manually via /compact.
    //
    // Threshold = `context_max - kOutputReserve - kCompactSlack`,
    // mirroring Claude Code's `OP8=13000` autocompact-buffer (binary
    // near offset 112623088). The earlier 80 % constant left ~40 K
    // tokens (200 K window) on the table indefinitely — the user
    // would feel "context full" at 160 K when the model could safely
    // run to ~187 K. Reserving an output budget (`kOutputReserve`)
    // for the model's reply + a small slack (`kCompactSlack`) for
    // the round-trip is the correct framing: trigger only when the
    // NEXT request couldn't fit its own response, not at an
    // arbitrary fraction of the window.
    constexpr int kOutputReserve = 13000;
    constexpr int kCompactSlack  = 4000;
    int threshold = (m.s.context_max > 0 && !m.s.autocompact_disabled)
        ? std::max(0, m.s.context_max - kOutputReserve - kCompactSlack)
        : 0;
    // Use the larger of the lagging `tokens_in` signal (set from the
    // prior turn's StreamUsage event) and a local estimate computed from
    // the actual message content. `tokens_in` is unreliable as a
    // proactive trigger: it reflects the request that *just completed*,
    // so a turn whose tool calls grew the transcript by 80k chars
    // (~22k tokens) since the last usage event will see `tokens_in`
    // still report the pre-growth size and skip compaction here even
    // though the next request will exceed the window. The local
    // estimate (bytes / 3.5 + ~1500 per image) catches that case.
    int est = estimate_prefix_tokens(m.d.current);
    if (threshold > 0
        && std::max(m.s.tokens_in, est) > threshold
        && !m.d.current.messages.empty()) {
        m.ui.composer.queued.push_back(drain_composer(m));
        // Reuse the CompactContext reducer arm so the path is one
        // place: it appends the synthetic prompt + placeholder, sets
        // m.s.compacting, and launches the stream. finalize_turn's
        // compaction branch drains m.ui.composer.queued post-compact.
        return agentty::app::update(std::move(m), Msg{CompactContext{}});
    }

    Message user;
    user.role = Role::User;
    // Image attachments need to reach the wire as image content
    // blocks, NOT as the "[image: ...]" prose marker that
    // attachment::expand emits. Lift the bytes off the composer
    // BEFORE drain — drain still emits the marker into user.text so
    // surrounding prose stays anchored, but the actual bytes ride on
    // user.images and the transport flattens them into Anthropic's
    // image block format.
    for (auto& att : m.ui.composer.attachments) {
        if (att.kind == Attachment::Kind::Image) {
            ImageContent img;
            img.media_type = std::move(att.media_type);
            img.bytes      = std::move(att.body);
            user.images.push_back(std::move(img));
        }
    }
    user.text = drain_composer(m);
    if (m.d.current.title.empty())
        m.d.current.title = deps().title_from(user.text);
    m.d.current.messages.push_back(std::move(user));

    Message placeholder;
    placeholder.role = Role::Assistant;
    m.d.current.messages.push_back(std::move(placeholder));

    m.d.current.updated_at = std::chrono::system_clock::now();

    // Idle → Streaming. The fresh phase::Active replaces the prior
    // turn's context wholesale (Idle had none): zero retry counters,
    // fresh started/last_event_at stamps, default RetryState. Mirrors
    // the StreamStarted handler's reset so the post-submit render is
    // layout-identical to the post-StreamStarted render that lands
    // milliseconds later — without this, leftover status toast from
    // the prior turn (retry countdown / "Stream complete" / error
    // banner) would change status_bar height by one row when
    // StreamStarted fires, producing a visible "new turn appears at
    // viewport bottom and then realigns" two-frame flicker.
    auto now = std::chrono::steady_clock::now();
    phase::Active ctx;
    ctx.started       = now;
    ctx.last_event_at = now;
    m.s.phase         = phase::Streaming{std::move(ctx)};
    m.s.status.clear();
    m.s.status_until  = {};

    auto virt = maybe_virtualize(m);
    auto launch = cmd::launch_stream(m);
    auto cmd = virt.is_none()
        ? std::move(launch)
        : Cmd<Msg>::batch(std::vector<Cmd<Msg>>{std::move(virt), std::move(launch)});
    return {std::move(m), std::move(cmd)};
}

void persist_settings(const Model& m) {
    store::Settings s;
    s.model_id = m.d.model_id;
    s.profile  = m.d.profile;
    for (const auto& mi : m.d.available_models)
        if (mi.favorite) s.favorite_models.push_back(mi.id);
    deps().save_settings(s);
}

maya::Cmd<Msg> set_status_toast(Model& m, std::string text,
                                std::chrono::seconds ttl) {
    using maya::Cmd;
    m.s.status = std::move(text);
    auto now = std::chrono::steady_clock::now();
    m.s.status_until = now + ttl;
    auto stamp = m.s.status_until;
    return Cmd<Msg>::after(
        std::chrono::duration_cast<std::chrono::milliseconds>(ttl)
            + std::chrono::milliseconds{50},
        Msg{ClearStatus{stamp}});
}

} // namespace agentty::app::detail
