#pragma once
// agentty::Model — the composed application state.
//
// This header imports each domain it aggregates and adds the UI-only
// sub-states that don't belong to any domain (composer, pickers, palette,
// modals).  Update / view code reach for domain-specific headers directly;
// only the runtime glue needs the full composite.

#include <optional>
#include <string>
#include <vector>

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

        std::vector<FileChange>          pending_changes;
        std::optional<PendingPermission> pending_permission;
    };

    struct UI {
        ComposerState       composer;
        ui::pick::OneAxis   model_picker;     // Closed | OpenAt{index}
        ui::pick::OneAxis   thread_list;      // Closed | OpenAt{index}
        CommandPaletteState command_palette;
        MentionPaletteState mention_palette;  // Closed | Open{query, index, files}
        SymbolPaletteState  symbol_palette;   // Closed | Open{query, index, entries}
        ui::pick::TwoAxis   diff_review;      // Closed | OpenAtCell{file_index,hunk_index}
        TodoState           todo;
        ui::login::State    login;            // Closed | Picking | OAuthCode | OAuthExchanging | ApiKeyInput | Failed
        int                 thread_scroll = 0;

        // ── Frozen scrollback prefix (agent_session pattern) ───────
        //
        // Append-only vector of fully-built Element rows that
        // represent the settled portion of the transcript. The view
        // passes a borrowed pointer to maya's Conversation widget
        // via `Config::frozen`, which renders it through `list_ref`
        // — zero-copy across frames regardless of how large this
        // grows. Each entry is one row in the conversation:
        //   • a gap (one-row blank) before each fresh-speaker turn,
        //   • a built Turn Element for a settled message (or run),
        //   • a compaction divider for a CompactionRecord boundary.
        //
        // The producer is `freeze_through_prior_turn()` (in
        // src/runtime/app/update/internal.hpp), called from
        // submit_message at the start of every new user turn. It
        // walks m.d.current.messages[frozen_through .. end), applies
        // the same tool-batch-merge logic as conversation_config used
        // to do at view time, and pushes one Turn Element per visual
        // unit — dividers and all.
        //
        // Cleared (and frozen_through reset) on thread switch /
        // NewThread / OpenThread; rebuilt by `rehydrate_frozen()`
        // when a saved thread is loaded.
        std::vector<maya::Element> frozen;

        // Exclusive upper bound into m.d.current.messages. Every
        // message with index < frozen_through has already been built
        // into `frozen` and need not be rendered live. The suffix
        // [frozen_through .. end) is the live tail — rebuilt every
        // frame (with the per-Turn shared_ptr Element cache keeping
        // settled-within-tail messages cheap).
        std::size_t         frozen_through = 0;

        // Running turn number for the next freshly-frozen Assistant
        // turn. The live tail reads (frozen_turn + assistant_messages_in_tail)
        // for its display numbers.
        int                 frozen_turn = 0;

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
    };

    Domain      d;
    StreamState s;
    UI          ui;
};

} // namespace agentty
