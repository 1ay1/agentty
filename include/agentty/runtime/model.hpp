#pragma once
// agentty::Model — the composed application state.
//
// This header imports each domain it aggregates and adds the UI-only
// sub-states that don't belong to any domain (composer, pickers, palette,
// modals).  Update / view code reach for domain-specific headers directly;
// only the runtime glue needs the full composite.

#include <optional>
#include <set>
#include <string>
#include <vector>

#include <maya/core/scroll_state.hpp>
#include <maya/element/element.hpp>

#include "agentty/domain/catalog.hpp"
#include "agentty/domain/conversation.hpp"
#include "agentty/diff/diff.hpp"
#include "agentty/domain/id.hpp"
#include "agentty/domain/profile.hpp"
#include "agentty/domain/session.hpp"
#include "agentty/domain/todo.hpp"
#include "agentty/runtime/command_palette.hpp"
#include "agentty/runtime/composer_attachment.hpp"
#include "agentty/runtime/mention_palette.hpp"
#include "agentty/runtime/symbol_palette.hpp"
#include "agentty/runtime/login.hpp"
#include "agentty/runtime/picker.hpp"
#include "agentty/runtime/view/cache.hpp"

namespace agentty {

// ============================================================================
// UI sub-states — one concern each, declared next to the Model that owns them
// ============================================================================

struct ComposerState {
    /// One queued, not-yet-sent message. Carries the same `text` +
    /// `attachments` pair the live composer does so a paste / @file
    /// chip that got queued (because the agent was busy) survives
    /// recall and resend as a chip rather than getting linearised
    /// into an inline blob the moment it left the composer. The
    /// fields here name-mirror ComposerState's text+attachments so a
    /// queue cycle is a structural swap, not a one-way collapse.
    struct QueuedMessage {
        std::string             text;
        std::vector<Attachment> attachments;
    };

    std::string text;
    int  cursor   = 0;
    bool expanded = false;
    std::vector<QueuedMessage> queued;
    /// Long pastes and @file picks live here as out-of-band bodies; the
    /// composer text holds a placeholder token (\x01ATT:N\x01) per
    /// attachment so cursor math and word-wrap stay plain-string.
    /// Submit-time expansion (attachment::expand) substitutes each
    /// placeholder with its body so the model sees the full bytes.
    std::vector<Attachment> attachments;

    /// Undo / redo. Each Snapshot is the WHOLE composer payload — text,
    /// cursor, attachments — captured before a mutating op. Cap depth
    /// at 64 entries; older snapshots are dropped FIFO. New edits clear
    /// the redo stack (standard editor semantics: branching from
    /// mid-history discards the old future).
    struct Snapshot {
        std::string             text;
        int                     cursor = 0;
        std::vector<Attachment> attachments;
    };
    std::vector<Snapshot> undo_stack;
    std::vector<Snapshot> redo_stack;

    /// History walking over previous USER messages in the current
    /// thread.
    ///   history_idx == -1 → live draft (composer is what the user is
    ///                        currently typing).
    ///   history_idx >=  0 → walking history; the value is the index
    ///                        into the reverse-chronological list of
    ///                        prior user messages. Press ↑ to walk
    ///                        further into the past, ↓ to walk back
    ///                        toward the live draft. The first ↑
    ///                        snapshots the live text into draft_save
    ///                        so the round-trip is non-destructive.
    /// Any text-mutating op (CharInput / Backspace / Paste / Kill /
    /// chip insertion) resets history_idx to -1 and discards
    /// draft_save — at that point the user is editing what they pulled
    /// from history, treating it as their new draft.
    int                        history_idx = -1;
    std::optional<std::string> draft_save;
    /// Companion to `draft_save`: the live draft's attachments[]
    /// captured at the same moment so a queue-peek round-trip
    /// (Alt+↑ … Alt+↓ past the tail) restores chips too, not just
    /// the text. Empty if there were no live attachments at the
    /// time of the snapshot, OR if the snapshot is for a history
    /// walk (history items never carry attachments — they're
    /// rendered turns whose chips were collapsed at submit time on
    /// previous schemas). Cleared together with `draft_save`.
    std::vector<Attachment>    draft_save_attachments;

    /// Per-item queue peek (Alt+↑ / Alt+↓). Mutually exclusive with
    /// history_idx — you're either walking past USER messages or
    /// editing a pending QUEUED one, never both.
    ///   queue_peek_idx == -1 → not peeking; composer holds the live
    ///                           draft (or history pick).
    ///   queue_peek_idx >=  0 → composer is showing
    ///                           m.ui.composer.queued[queue_peek_idx]
    ///                           for in-place editing. Submit removes
    ///                           that index from the queue and re-
    ///                           queues the edited bytes at the tail;
    ///                           Esc / Alt+↓ past the end restores the
    ///                           live draft from draft_save.
    /// Any text-mutating op while peeking is normal editing of the
    /// peeked item — only the queue slot is treated as the new
    /// pending content on submit.
    int                        queue_peek_idx = -1;
};

// Todo picker carries its own item list — separate concern from the
// open/closed state, which now lives in `open` as a typed variant.
struct TodoState {
    ui::pick::Modal       open;     // Closed | OpenModal
    std::vector<TodoItem> items;
};

// ============================================================================
// Model — the composed application state, split into three concerns:
//   d   — Domain: what the conversation is (persisted, sent to provider).
//   s   — Session: the in-flight request's state machine + cancel handle.
//   ui  — UI: picker/modal/view-virtualization state, pure ephemeral.
//
// The split lets call sites communicate their scope: a function that only
// touches `m.ui.*` can't accidentally mutate the conversation; a reducer
// fragment that reads `m.d.*` doesn't need to thread picker state through.
// ============================================================================

struct Model {
    struct Domain {
        Thread              current;
        std::vector<Thread> threads;
        Profile             profile = Profile::Write;

        std::vector<ModelInfo> available_models;
        ModelId                model_id{std::string{"claude-opus-4-5"}};

        // Reasoning effort tier, selected live in the model picker (←/→).
        // None = the default no-thinking wire; any other level makes the
        // Claude provider send adaptive thinking + output_config.effort.
        // Persisted in Settings; gated per-model at request-build time.
        Effort                 effort = Effort::None;

        std::vector<FileChange>          pending_changes;
        std::optional<PendingPermission> pending_permission;

        // Session-scoped "always allow" grants, keyed by tool name
        // (e.g. "bash", "write"). Set by PermissionApproveAlways;
        // consulted in kick_pending_tools BEFORE prompting. NOT
        // persisted — a grant lives for the lifetime of the process
        // run, mirroring Zed's per-session allow-list. Cleared on
        // profile change so tightening the profile re-arms prompts.
        std::set<std::string>            session_grants;
    };

    struct UI {
        ComposerState       composer;
        ui::pick::OneAxis   model_picker;     // Closed | OpenAt{index}
        ui::pick::OneAxis   provider_picker;  // Closed | OpenAt{index}
        ui::pick::OneAxis   thread_list;      // Closed | OpenAt{index}
        CommandPaletteState command_palette;
        MentionPaletteState mention_palette;  // Closed | Open{query, index, files}
        SymbolPaletteState  symbol_palette;   // Closed | Open{query, index, entries}
        ui::pick::TwoAxis   diff_review;      // Closed | OpenAtCell{file_index,hunk_index}
        TodoState           todo;
        ui::login::State    login;            // Closed | Picking | OAuthCode | OAuthExchanging | ApiKeyInput | Failed
        int                 thread_scroll = 0;

        // ── Strata depositional rendering (no host snapshot) ───────
        //
        // The host keeps NO frozen Element vector, NO frozen_through
        // cursor, NO frozen_turn counter, and NO settle-freeze timing.
        // strata_nodes() enumerates the transcript as logical-turn nodes
        // straight from m.d.current.messages each frame and hands them to
        // maya's Strata renderer, which measures, seals, bounds, and
        // caches every node itself (see src/runtime/view/view.cpp).
        //
        // The ONLY surviving piece of host bookkeeping is this hint:
        // the message index where the live (non-sealable) tail begins,
        // recomputed by strata_nodes every frame and read by
        // conversation_config / view_impl so the LIVE node renders
        // exactly the runs at/after it. It is a pure function of the
        // messages; persisting it is a convenience to avoid recomputing
        // the run walk in three places, not a stateful cursor. `mutable`
        // because strata_nodes runs under a const Model& (same
        // logical-const pattern as view_cache / the picker scroll slots).
        mutable std::size_t live_run_start = 0;

        // True when live_run_start lands MID-RUN: the in-flight assistant
        // run's settled sub-turn prefix [run head, live_run_start) was
        // split off into its own sealed strata node, so the live tail must
        // render its first run as a CONTINUATION rail (header suppressed,
        // lead-gap seam blank) of that sealed head — not as a fresh turn
        // with a duplicate header. Set by strata_nodes alongside
        // live_run_start; read by conversation.cpp's build_live_tail_from.
        // The matching sealed head node is built by build_settled_run with
        // the same [head, live_run_start) sub-range, so the two rails join
        // row-identically to the un-split single rail. `mutable` for the
        // same logical-const reason as live_run_start.
        mutable bool        live_run_is_continuation = false;

        // Cross-frame widget state cache. The only consumers now are:
        //   • StreamingMarkdown — keeps a per-Message widget instance
        //     alive across frames so its block boundary cache survives
        //     between live renders.
        //   • Agent-timeline panel freeze — snapshots the AgentTimeline
        //     Element once every tool call is terminal, then serves
        //     that frozen Element until the message gets pushed into
        //     m.ui.frozen.
        // No Turn-level Element cache anymore: settled turns live as
        // raw Element values inside m.ui.frozen, the live tail
        // rebuilds each frame (bounded by the active turn).
        mutable ui::ViewCache view_cache;

        // ── Scroll state for modal pickers ─────────────────────────────
        //
        // Storage for the scroll state of each modal picker. The picker
        // widget (maya::Picker) reads & mutates these via a borrowed
        // pointer in Picker::Config; the host owns the storage so the
        // adapter rule holds (maya owns chrome + behavior; agentty owns
        // model state).
        //
        // `mutable` is required because the view function takes
        // `const Model&` but maya's paint-time writeback mutates
        // max_y / bar_v_bounds / viewport_bounds. Same logical-const
        // pattern as view_cache above: filling a render-side cache /
        // bounds slot doesn't change observable Model behavior.
        //
        // Persisted across open→close→open by default. The reducer can
        // reset `.y = 0` on semantic transitions if desired (e.g. when
        // the filter query changes the match set).
        //
        // auto_dispatch = false: these pickers are selection-driven. The
        // reducer owns the cursor (ModelPickerMove / ThreadListMove /
        // CommandPaletteMove / …) and the Picker widget auto-scrolls the
        // viewport to keep the selected row visible every build. Leaving
        // auto_dispatch on (the default) would ALSO feed every ↑/↓/PageUp
        // arrow straight into ScrollState::handle, bumping scroll.y in
        // parallel — which then fights the widget's selection-follow
        // clamp. The visible symptom is arrow keys appearing to do
        // nothing until the offset saturates against max_y ("press 4-5
        // times before it registers once"). Scroll position here is a
        // pure function of the selected index, so the raw-key dispatch
        // is interference, not input.
        mutable maya::ScrollState model_picker_scroll{.auto_dispatch = false};
        mutable maya::ScrollState provider_picker_scroll{.auto_dispatch = false};
        mutable maya::ScrollState thread_list_scroll{.auto_dispatch = false};
        mutable maya::ScrollState command_palette_scroll{.auto_dispatch = false};
        mutable maya::ScrollState mention_palette_scroll{.auto_dispatch = false};
        mutable maya::ScrollState symbol_palette_scroll{.auto_dispatch = false};
        mutable maya::ScrollState todo_scroll{.auto_dispatch = false};
    };

    Domain      d;
    StreamState s;
    UI          ui;
};

} // namespace agentty
