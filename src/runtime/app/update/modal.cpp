// Composer-submission, thread-virtualization, and settings-persistence
// helpers for the update reducer. submit_message is the entry point for
// ComposerEnter / ComposerSubmit and is also called from finalize_turn when
// flushing the composer's queued-message buffer, which is why it lives in a
// shared internal header rather than an anonymous namespace.

#include "moha/runtime/app/update/internal.hpp"
#include "moha/runtime/app/update.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include "moha/runtime/app/cmd_factory.hpp"
#include "moha/runtime/app/deps.hpp"
#include "moha/store/store.hpp"

namespace moha::app::detail {

namespace {

// When advancing thread_view_start past kSliceChunk old messages, we tell
// maya's inline renderer how many rows of prev_cells to commit to the
// terminal's native scrollback (Cmd::commit_scrollback →
// Runtime::commit_inline_prefix → InlineFrameState::commit_prefix).
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
// So the value passed to commit_scrollback MUST be a conservative LOWER
// BOUND on the actual rendered rows of messages
// [old_thread_view_start, new_thread_view_start). It must never exceed
// the actual height.
//
// The previous implementation tried a content-aware estimate
// (`estimate_message_rows`) using a hardcoded `kEstWidth = 100` and
// per-byte arithmetic on `msg.text`. Two paths over-committed:
//
//   1. Terminal wider than 100 columns: text wraps to fewer rows than
//      `bytes / 96 + 1` predicts.
//   2. UTF-8 multi-byte content: byte-count overstates display width.
//   3. Elided tool body previews: the maya::ToolBodyPreview kinds
//      (BashOutput tail-only, FileRead head-only, GrepMatches grouped)
//      render in 4-8 rows where the estimate's `min(nl, 10)` could
//      easily reach 14.
//
// Replacing the estimate with a small constant per dropped message
// guarantees safety regardless of terminal width, content composition,
// or tool-body rendering choices. The exact rows-per-message-rendered
// vary widely (1 row for a continuation with empty body up to dozens
// for a tool-heavy turn), but the LOWER bound — header row plus one row
// of body or inter-turn separator — is reliably ≥ 1.  A value of 2 here
// matches the conservative `ROWS_PER_DROP_LOWER` used in
// agent_session.cpp's frozen-elements trim, and stays at-or-below
// almost any plausible real rendered height.
constexpr int kRowsPerDroppedMessageLower = 2;

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
    const int committed_rows = kSliceChunk * kRowsPerDroppedMessageLower;
    return Cmd<Msg>::commit_scrollback(committed_rows);
}

Step submit_message(Model m) {
    using maya::Cmd;
    if (m.ui.composer.text.empty()) return done(std::move(m));

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
    if (m.s.active()) {
        m.ui.composer.queued.push_back(std::exchange(m.ui.composer.text, {}));
        m.ui.composer.cursor = 0;
        return done(std::move(m));
    }

    // Auto-compaction trigger. Mirrors Claude Code 2.1.119's
    // `BetaToolRunner.compactionControl` (binary near offset 134600,
    // default `um8 = 1e5` = 100k tokens). Threshold is 100k or 80% of
    // the model's context_max, whichever is lower — gives a safety
    // margin before we'd otherwise blow the window. When tripped,
    // queue the user's just-typed message and dispatch CompactContext;
    // the post-compaction drain in finalize_turn submits the queued
    // message against the now-shrunk history. tokens_in is the
    // last-turn input token count; on a fresh thread it's 0 so this
    // gate sleeps until the conversation has actually grown.
    constexpr int kCompactThresholdAbs = 100'000;
    int rel_threshold = (m.s.context_max > 0)
        ? static_cast<int>(m.s.context_max * 0.80)
        : kCompactThresholdAbs;
    int threshold = (rel_threshold < kCompactThresholdAbs && rel_threshold > 0)
                  ? rel_threshold : kCompactThresholdAbs;
    if (m.s.tokens_in > threshold && !m.d.current.messages.empty()) {
        m.ui.composer.queued.push_back(std::exchange(m.ui.composer.text, {}));
        m.ui.composer.cursor = 0;
        // Reuse the CompactContext reducer arm so the path is one
        // place: it appends the synthetic prompt + placeholder, sets
        // m.s.compacting, and launches the stream. finalize_turn's
        // compaction branch drains m.ui.composer.queued post-compact.
        return moha::app::update(std::move(m), Msg{CompactContext{}});
    }

    Message user;
    user.role = Role::User;
    user.text = std::exchange(m.ui.composer.text, {});
    m.ui.composer.cursor = 0;
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

} // namespace moha::app::detail
