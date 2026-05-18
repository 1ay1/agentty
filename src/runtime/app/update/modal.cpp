// Composer-submission and settings-persistence helpers for the update
// reducer. submit_message is the entry point for ComposerEnter /
// ComposerSubmit and is also called from finalize_turn when flushing
// the composer's queued-message buffer, which is why it lives in a
// shared internal header rather than an anonymous namespace.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <chrono>
#include <utility>

#include "agentty/runtime/app/cmd_factory.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/composer_attachment.hpp"
#include "agentty/store/store.hpp"

namespace agentty::app::detail {

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
    // Drain composer.text + composer.attachments into a chip-form
    // payload — the placeholders STAY in the text and the attachment
    // bodies travel separately. Used by:
    //   • the queue-on-busy / queue-on-compact paths (queued items
    //     keep their chips so recall + resend renders as a chip too,
    //     not a linearised blob);
    //   • the actual submit path below (the new Message gets the
    //     same chip-form text and the attachments are moved onto
    //     `Message.attachments`).
    //
    // The transport calls `attachment::expand(...)` at request-build
    // time to splice the bodies back in so the model still sees
    // literal pasted bytes / file contents — the only thing that
    // changes is what the user sees in the rendered transcript.
    auto drain_composer = [](Model& mm) {
        ComposerState::QueuedMessage out;
        out.text        = std::exchange(mm.ui.composer.text, {});
        out.attachments = std::exchange(mm.ui.composer.attachments, {});
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
        mm.ui.composer.queue_peek_idx = -1;
        return out;
    };

    // Peeked-item submission: the user pressed Alt+↑ to load a queued
    // item, possibly edited it, and submitted. We remove the ORIGINAL
    // slot from the queue now; the drain-into-queued path below (when
    // the agent is still busy) will push the edited bytes back onto
    // the tail. If the agent is idle (rare — user would have to peek
    // while the agent was busy, then have it finish before they hit
    // Enter), the edited bytes go straight to the wire and the queue
    // just shrinks by one.
    if (m.ui.composer.queue_peek_idx >= 0
        && m.ui.composer.queue_peek_idx
               < static_cast<int>(m.ui.composer.queued.size())) {
        m.ui.composer.queued.erase(
            m.ui.composer.queued.begin() + m.ui.composer.queue_peek_idx);
        // draft_save (if any) is the live draft the user was typing
        // before they pressed Alt+↑. They've explicitly committed the
        // peeked item by submitting it, so the saved draft is now
        // homeless — drop it. (drain_composer clears the field too,
        // but only after we've decided to drain; doing it here keeps
        // the bail-out paths above tidy.)
        m.ui.composer.draft_save.reset();
        m.ui.composer.draft_save_attachments.clear();
        m.ui.composer.queue_peek_idx = -1;
    }

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

    // No auto-compaction on submit. Earlier versions queued the
    // user's message and fired a synchronous compaction round before
    // releasing it — user hits Enter, sees nothing for 30-60 s,
    // then their message finally goes out. That was an unacceptable
    // workflow break.
    //
    // The new shape: `launch_stream` soft-trims the wire payload to
    // fit ~95% of context_max on every normal turn, so submits NEVER
    // need to wait on compaction to be safe. The user's message goes
    // out immediately. A background auto-compact may still fire at
    // the next post-turn idle boundary (see `maybe_autocompact_after_turn`
    // in finalize_turn) — that's the right moment because the user
    // is reading the model's output, not typing, and the compaction
    // happens without blocking anything they're trying to do. The
    // /compact slash command also stays available for manual control.

    Message user;
    user.role = Role::User;
    // Drain composer → chip-form text + attachments. Image
    // attachments must reach the wire as Anthropic image content
    // blocks (NOT as the "[image: ...]" prose marker); we lift
    // their bytes onto user.images here and DROP them from
    // attachments so the on-Message attachments vector only
    // contains the kinds the wire expander handles textually
    // (Paste / FileRef / Symbol). The chip placeholder for the
    // image stays in user.text — the renderer treats a placeholder
    // pointing past attachments[] as an Image chip and consults
    // user.images[] for the caption.
    auto drained = drain_composer(m);
    // Image attachments: their bytes get lifted to `user.images` so
    // the transport can encode them as Anthropic image content
    // blocks. We KEEP the Attachment entry in `drained.attachments`
    // — just with `body` moved out — so placeholder indices in
    // `user.text` remain valid (a paste followed by an @file by an
    // image would have placeholders 0, 1, 2; renumbering after erase
    // would desynchronise the text with the vector). The wire
    // expander emits a textual marker for kind==Image; the renderer
    // surfaces the same chip caption it would for any other kind.
    for (auto& att : drained.attachments) {
        if (att.kind == Attachment::Kind::Image) {
            ImageContent img;
            img.media_type = att.media_type;     // copy: path/type stays on Attachment
            img.bytes      = std::move(att.body);
            user.images.push_back(std::move(img));
        }
    }
    user.text        = std::move(drained.text);
    user.attachments = std::move(drained.attachments);
    if (m.d.current.title.empty()) {
        // Title generation should see human-readable text, not raw
        // chip placeholders. Build a plain-text view of the user's
        // message: each `\x01ATT:N\x01` becomes `[<chip-label>]`,
        // matching what the user sees in the rendered turn.
        std::string title_src;
        title_src.reserve(user.text.size());
        std::size_t i = 0;
        while (i < user.text.size()) {
            if (static_cast<unsigned char>(user.text[i]) == attachment::kSentinel) {
                auto len = attachment::placeholder_len_at(user.text, i);
                if (len > 0) {
                    auto idx = attachment::placeholder_index(user.text, i);
                    if (idx < user.attachments.size()) {
                        title_src.push_back('[');
                        title_src.append(attachment::chip_label(user.attachments[idx]));
                        title_src.push_back(']');
                    }
                    i += len;
                    continue;
                }
            }
            title_src.push_back(user.text[i++]);
        }
        m.d.current.title = deps().title_from(title_src);
    }

    // Freeze everything currently in messages before pushing the new
    // User. The prior turn (whatever's there now) is fully settled by
    // construction: submit only proceeds when m.s.is_idle(), so no
    // tool is running and no stream is in flight. After this call,
    // m.ui.frozen contains built Elements for messages[0..end), and
    // m.ui.frozen_through == messages.size(). The freshly-pushed
    // user + assistant placeholder become the live tail.
    //
    // Mirrors the agent_session example's discipline: settled turns
    // are committed to m.frozen the moment the next thing arrives;
    // the view renders frozen as list_ref (zero copy) plus a tiny
    // live tail. Per-frame cost stops growing with session length.
    freeze_through(m, m.d.current.messages.size());

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

    auto trim = trim_frozen_if_oversized(m);
    auto launch = cmd::launch_stream(m);
    auto cmd = trim.is_none()
        ? std::move(launch)
        : Cmd<Msg>::batch(std::vector<Cmd<Msg>>{std::move(trim), std::move(launch)});
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
