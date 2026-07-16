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
#include "agentty/runtime/view/helpers.hpp"
#include "agentty/provider/registry.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/store/store.hpp"
#include "agentty/tool/skills.hpp"
#include "agentty/tool/subagent.hpp"
#include "agentty/workspace/checkpoint.hpp"

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

    // ── User-explicit skill activation (spec: slash-command syntax) ──
    // `/skill-name [rest of prompt]` — the harness intercepts the token,
    // splices the skill's full activation payload into the message, and
    // marks it active (so a later model-driven `skill` call dedups).
    // The model receives the instructions without having to take an
    // activation action itself. Only the FIRST token is considered, and
    // only when it exactly matches a discovered skill — `/compact` and
    // friends fall through to their existing handlers untouched.
    if (!user.text.empty() && user.text.front() == '/') {
        auto sp  = user.text.find_first_of(" \t\n");
        auto tok = user.text.substr(1, sp == std::string::npos
                                           ? std::string::npos : sp - 1);
        if (const auto* sk = tools::skills::find(tok)) {
            std::string rest = sp == std::string::npos
                ? std::string{}
                : std::string{user.text.substr(sp + 1)};
            std::string expanded;
            if (tools::skills::note_activated(sk->name)) {
                expanded  = tools::skills::activation_payload(*sk);
                expanded += "\n\nFollow the skill instructions above";
                expanded += rest.empty() ? "." : " for this task: " + rest;
            } else {
                // Already active this session — don't re-inject the body.
                expanded = "Apply the already-loaded '" + sk->name
                         + "' skill" + (rest.empty() ? "." : ": " + rest);
            }
            user.text = std::move(expanded);
        }
    }

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

    // ── Git checkpoint (Zed-agent behavior) ─────────────────────────
    // Stamp a checkpoint id on the user message and snapshot the whole
    // worktree (tracked + untracked, .gitignore respected) as a pinned
    // parentless commit BEFORE the agent starts mutating files. The
    // snapshot itself runs on an isolated worker (git add -A can take a
    // moment on a big repo); only the cheap repo-ness probe + id stamp
    // happen here. The view renders a checkpoint divider above the turn
    // (cfg.checkpoint_above), and "Rewind to checkpoint" in the palette
    // restores the files + truncates the transcript back to this point.
    // Outside a git repo this is a no-op — no id, no divider, no worker.
    std::optional<std::string> checkpoint_to_create;
    if (workspace::in_git_repo()) {
        user.checkpoint_id  = CheckpointId{user.id.value};
        checkpoint_to_create = user.id.value;
    }

    // Freeze the prior turn AND the freshly-pushed User in one pass —
    // the agent_session SessionStart analog (it pushes gap() + the user
    // Turn into m.frozen the moment the user submits). The prior turn
    // is fully settled by construction: submit only proceeds when
    // m.s.is_idle(), so no tool is running and no stream is in flight.
    // The user message is immutable from birth, so freezing it
    // immediately is always safe. After this, the live tail contains
    // ONLY the in-flight assistant run — exactly agent_session's shape:
    // the user Turn paints once from frozen (zero-copy list_ref blit)
    // instead of being re-built every frame for the whole run, and the
    // settle-time freeze has one fewer seam to hand off.
    m.d.current.messages.push_back(std::move(user));
    // Force the prior turn's reveal to settle BEFORE the freeze snapshot.
    // Normally the deferred settle-freeze (meta.cpp) waits for the reveal
    // to drain on its own, but a user can submit while it's still mid-
    // glide (pending_settle_freeze true). Freezing a still-`live_` widget
    // would snapshot a tree whose hash diverges from the on-screen live
    // frame — the post-stream duplicate. settle_message_md runs the
    // (now harmless) finish() so the widget is in its settled shape; the
    // freeze below then captures exactly what the next live frame would
    // paint. By submit time msg.text is final and streaming_text empty,
    // so this is shape-neutral for an already-drained turn and a clean
    // collapse for a still-animating one (the user moved on; cutting the
    // last ~100 ms of typewriter is the right call when they hit Enter).
    for (std::size_t i = m.ui.frozen_through;
         i + 1 < m.d.current.messages.size(); ++i) {
        auto& mm = m.d.current.messages[i];
        if (mm.role != Role::Assistant || mm.text.empty()) continue;
        settle_message_md(m, mm);
    }
    freeze_through(m, m.d.current.messages.size());
    // A deferred settle-freeze may still be pending from the prior turn
    // (user submitted before the next idle Tick fired). The freeze above
    // just covered it, so drop the flag to avoid a redundant no-op freeze
    // on the next Tick.
    m.ui.pending_settle_freeze = false;

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
    // No commit_scrollback_overflow here. Submit is not a wholesale
    // model swap — it appends to the existing transcript, so maya's
    // normal row diff handles the composer-shrink + new-turn-rows
    // transition correctly. agent_session.cpp fires commit_scrollback
    // only on the FROZEN_MAX trim; we mirror that here. Past versions
    // fired commit_scrollback_overflow on every submit to flush a
    // suspected composer-shrink seam, but the call advances prev_cells
    // past whatever the renderer thinks has overflowed — when nothing
    // has actually overflowed, the bookkeeping turns valid scrollback
    // mirror rows into "forget these" and the next diff re-emits
    // them, surfacing as duplicate cards in scrollback.
    std::vector<Cmd<Msg>> parts;
    if (!trim.is_none()) parts.push_back(std::move(trim));
    // Worktree snapshot rides the same batch as the stream launch: it
    // runs concurrently with the request's TTFB window, so by the time
    // the model asks for its first file edit the checkpoint is pinned.
    // Failure is silent by design — the rewind path re-verifies the ref
    // exists and surfaces a toast there instead.
    if (checkpoint_to_create) {
        parts.push_back(Cmd<Msg>::task_isolated(
            [id = std::move(*checkpoint_to_create)]
            (std::function<void(Msg)>) {
                (void)workspace::create_checkpoint(id);
            }));
    }
    parts.push_back(std::move(launch));
    auto cmd = parts.size() == 1
        ? std::move(parts.front())
        : Cmd<Msg>::batch(std::move(parts));
    return {std::move(m), std::move(cmd)};
}

std::string active_provider_id() {
    const auto& sel = provider::active();
    if (sel.kind == provider::Kind::OpenAI)
        return sel.openai_endpoint.label;
    return std::string{provider::default_provider_id()};
}

std::string model_for_provider(std::string_view spec) {
    // 1) Recall the model the user last used on this provider.
    auto s = deps().load_settings();
    if (auto it = s.provider_models.find(std::string{spec});
        it != s.provider_models.end() && !it->second.empty())
        return it->second;

    // 2) No recall — fall back to a sane built-in default per provider.
    //    Local backends (Ollama) have no fixed default; return empty so the
    //    model-list refetch auto-selects the first available model.
    if (spec == "anthropic" || spec.empty()) return "claude-opus-4-5";
    if (spec == "openai")                    return "gpt-4o";
    return {};
}

void reset_composer_draft(ComposerState& c) {
    c.text.clear();
    c.cursor = 0;
    c.attachments.clear();
    c.undo_stack.clear();
    c.redo_stack.clear();
    c.history_idx = -1;
    c.draft_save.reset();
    c.draft_save_attachments.clear();
    c.queue_peek_idx = -1;
    c.queued.clear();
}

void persist_settings(const Model& m) {
    // Load-modify-save: preserve provider, provider_keys, and the
    // per-provider model map that this function doesn't own. Building a
    // fresh Settings{} here would silently wipe the active provider on
    // every model-picker select.
    auto s = deps().load_settings();
    s.model_id = m.d.model_id;
    s.profile  = m.d.profile;
    s.favorite_models.clear();
    for (const auto& mi : m.d.available_models)
        if (mi.favorite) s.favorite_models.push_back(mi.id);
    // Record this model as the active provider's last-used selection so a
    // later switch back to it restores exactly this model.
    if (!m.d.model_id.empty())
        s.provider_models[active_provider_id()] = m.d.model_id.value;
    s.effort = std::string{effort_wire(m.d.effort)};
    deps().save_settings(s);
}

std::pair<Model, maya::Cmd<Msg>>
commit_provider_switch(Model m, std::string_view spec,
                       auth::AuthHeader new_auth, std::string_view label) {
    using maya::Cmd;
    const std::string spec_s{spec};

    // (1) File the OUTGOING model under its canonical provider id BEFORE
    //     provider::select swaps active() out from under us, so a later
    //     switch back restores exactly this model.
    const std::string outgoing_id = active_provider_id();

    // (2) Install the new selection (process-global; the stream seam reads
    //     active() at call time).
    provider::select(provider::parse_selection(spec_s));

    {
        auto settings = deps().load_settings();
        if (!m.d.model_id.empty())
            settings.provider_models[outgoing_id] = m.d.model_id.value;
        settings.provider = spec_s;
        deps().save_settings(settings);
    }

    // (3) Make a valid model active for the NEW backend: recall → built-in
    //     default → empty (ModelsLoaded auto-selects the first available).
    if (auto next = model_for_provider(spec_s); !next.empty()) {
        m.d.model_id    = ModelId{next};
        m.s.context_max = ui::context_max_for_model(m.d.model_id.value);
        tools::subagent::set_model(m.d.model_id.value);
    }

    // (4) Re-clamp the reasoning-effort tier to what the (possibly new)
    //     active model supports — a stale Xhigh/High carried onto a model
    //     without that tier (or a non-reasoning model) would otherwise show a
    //     bogus chip and get silently dropped only at request time. When the
    //     new model isn't known yet (local, empty id) this is a no-op until
    //     ModelsLoaded, which is fine — the wire path re-clamps regardless.
    m.d.effort = clamp_effort(
        m.d.effort, ModelCapabilities::from_id(m.d.model_id.value));

    // (5) Persist the FULL settings shape (provider + per-provider model +
    //     effort + favorites) through the one owner so effort is never
    //     dropped on a hop, then swap the Deps auth and refetch models.
    persist_settings(m);

    app::switch_provider(std::move(new_auth));
    m.d.available_models.clear();
    m.s.models_loading = true;

    auto toast = set_status_toast(
        m, "provider \xe2\x86\x92 " + std::string{label}, std::chrono::seconds{3});
    return {std::move(m),
            Cmd<Msg>::batch(std::move(toast), cmd::fetch_models())};
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
