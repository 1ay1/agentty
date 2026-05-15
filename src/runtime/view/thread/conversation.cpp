#include "agentty/runtime/view/thread/conversation.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <unordered_set>

#include <maya/widget/turn.hpp>

#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/thread/turn/turn.hpp"

namespace agentty::ui {

namespace {

// Synthetic divider rendered in place of an absent "compact summary
// message". Identical chrome to the legacy `is_compact_summary`-on-
// Message branch in turn.cpp (≡ + "Conversation compacted" on a
// muted rail), surfaced as its own PreBuilt entry between the
// pre- and post-compaction turns. Marks the boundary the model no
// longer sees in raw form, without taking a Message slot of its own.
maya::Conversation::PreBuilt compaction_divider_prebuilt() {
    maya::Turn::Config cfg;
    cfg.glyph      = "\xe2\x89\xa1";              // ≡
    cfg.label      = "Conversation compacted";
    cfg.rail_color = muted;
    return maya::Conversation::PreBuilt{
        .element      = maya::Turn{std::move(cfg)}.build(),
        .continuation = false,
    };
}

} // namespace

maya::Conversation::Config conversation_config(const Model& m) {
    maya::Conversation::Config cfg;

    // Virtualize: older messages live in the terminal's native scrollback
    // (committed via maya::Cmd::commit_scrollback). Preserve absolute
    // turn numbering by reading the running turn count that maybe_virtualize
    // maintains alongside thread_view_start — O(1), regardless of how
    // many turns the session has accumulated.
    //
    // Compaction does NOT alter what's drawn here. It's a wire-only
    // event: the model's view of history collapses to a summary +
    // recent-tail when the next request goes out (handled in
    // `cmd_factory::wire_messages_for`), but the user keeps seeing
    // every turn they ever had. The only visible signal of compaction
    // is the ≡ divider injected below at each CompactionRecord's
    // boundary index — chrome, not content.
    const std::size_t total = m.d.current.messages.size();
    const std::size_t start = static_cast<std::size_t>(
        std::clamp(m.ui.thread_view_start, 0, static_cast<int>(total)));
    int turn = 1 + m.ui.thread_view_start_turn;

    // Set of message indices that should be PRECEDED by a compaction
    // divider in the rendered conversation. Built from the thread's
    // CompactionRecord list. `up_to_index` is "covers messages[0..N)"
    // — the divider therefore sits immediately before messages[N], i.e.
    // exactly at index N. Skip records whose boundary is 0 (no turns to
    // mark off) or beyond `total` (defensive against malformed loads).
    std::unordered_set<std::size_t> divider_at;
    divider_at.reserve(m.d.current.compactions.size());
    for (const auto& rec : m.d.current.compactions) {
        if (rec.up_to_index > 0 && rec.up_to_index <= total) {
            divider_at.insert(rec.up_to_index);
        }
    }

    // Use the agent_session-style fast path: emit pre-built turn
    // Elements via maya::Conversation's `built_turns` field. Settled
    // turns are served from the (thread, msg_idx) → Element cache and
    // skip Turn::build() entirely on every frame after the first;
    // only the live tail rebuilds. Without this, maya::Conversation::
    // build() called `Turn{cfg}.build()` per visible turn per frame,
    // which laid out every tool card, every markdown block, and every
    // permission row from scratch — the dominant per-frame cost on a
    // long session. Mirrors what the agent_session example achieves
    // with `m.frozen` + `list_ref(...)`: build once per turn lifetime,
    // render-by-reference forever after.
    cfg.built_turns.reserve(total - start
                            + m.ui.composer.queued.size()
                            + divider_at.size());
    for (std::size_t i = start; i < total; ++i) {
        if (divider_at.contains(i)) {
            cfg.built_turns.push_back(compaction_divider_prebuilt());
        }
        const auto& msg = m.d.current.messages[i];
        // Continuation: a 2nd+ Assistant in a same-speaker run. The Turn
        // widget suppresses its header on continuations and the
        // Conversation widget skips the inter-turn divider, so the
        // per-API-response message structure stays intact (Anthropic's
        // protocol requires it) while three back-to-back agent rounds
        // visually flow as one block under one "Sonnet 4.5" header.
        const bool continuation =
            (msg.role == Role::Assistant) &&
            (i > 0) &&
            (m.d.current.messages[i - 1].role == Role::Assistant);

        cfg.built_turns.push_back(turn_element(msg, i, turn, m, continuation));

        // Increment the user-visible turn number only on the FIRST
        // assistant of a run.  Continuations share the run's number so
        // the next user message gets the next sequential turn (otherwise
        // a 5-round agent action would push the next user to turn N+5).
        if (msg.role == Role::Assistant && !continuation) ++turn;
    }

    // Queued-message previews: render typed-but-not-yet-sent messages as
    // user turns at the tail of the transcript so the user can SEE what
    // they queued, not just a count. Mirrors Claude Code's behaviour
    // (offset 80106500 in the 2.1.119 binary): no special glyph, no
    // "queued:" label, no dim modifier — visually identical to a real
    // user message. The "this is queued, not sent yet" cue comes from
    // (a) the absence of a following assistant turn and (b) the
    // composer's `❚ N queued` chip.
    //
    // `synthetic = true` opts these per-frame Message instances out of
    // the Element cache. Each call default-constructs the Message with
    // a fresh MessageId, so caching them would only fill the LRU with
    // garbage entries.
    if (!m.ui.composer.queued.empty()) {
        auto now = std::chrono::system_clock::now();
        for (std::size_t qi = 0; qi < m.ui.composer.queued.size(); ++qi) {
            Message synthetic;
            synthetic.role        = Role::User;
            synthetic.text        = m.ui.composer.queued[qi].text;
            // Copy (not move) — the model owns its queue; this is a
            // per-frame preview. The chip-substitution path in
            // turn.cpp's User-body builder reads `msg.attachments`
            // to caption each placeholder.
            synthetic.attachments = m.ui.composer.queued[qi].attachments;
            synthetic.timestamp   = now;
            // Per-item meta strip so the user can tell WHICH queued
            // message is which when cycling with Alt+↑/Alt+↓ —
            // otherwise the preview turns are indistinguishable from
            // each other and from a real prior turn. The active peek
            // gets a leading marker so the eye lands on it.
            std::string meta = "queued #" + std::to_string(qi + 1)
                             + " / "     + std::to_string(m.ui.composer.queued.size());
            if (static_cast<int>(qi) == m.ui.composer.queue_peek_idx)
                meta = "\xe2\x9c\x8e editing — " + meta;   // ✎
            cfg.built_turns.push_back(turn_element(synthetic, total + qi,
                                                   turn, m, /*continuation=*/false,
                                                   /*synthetic=*/true,
                                                   /*meta_override=*/meta));
            ++turn;
        }
    }

    // No in-thread activity indicator: the status-bar PhaseChip
    // already shows the live phase verb + elapsed time from the same
    // m.s.phase source, so a second copy under the assistant turn was
    // pure redundancy (and competed for the eye with the chip).
    cfg.in_flight = std::nullopt;
    return cfg;
}

} // namespace agentty::ui
