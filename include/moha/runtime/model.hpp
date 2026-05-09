#pragma once
// moha::Model — the composed application state.
//
// This header imports each domain it aggregates and adds the UI-only
// sub-states that don't belong to any domain (composer, pickers, palette,
// modals).  Update / view code reach for domain-specific headers directly;
// only the runtime glue needs the full composite.

#include <optional>
#include <string>
#include <vector>

#include "moha/domain/catalog.hpp"
#include "moha/domain/conversation.hpp"
#include "moha/diff/diff.hpp"
#include "moha/domain/id.hpp"
#include "moha/domain/profile.hpp"
#include "moha/domain/session.hpp"
#include "moha/domain/todo.hpp"
#include "moha/runtime/command_palette.hpp"
#include "moha/runtime/composer_attachment.hpp"
#include "moha/runtime/mention_palette.hpp"
#include "moha/runtime/symbol_palette.hpp"
#include "moha/runtime/login.hpp"
#include "moha/runtime/picker.hpp"
#include "moha/runtime/view/cache.hpp"

namespace moha {

// ============================================================================
// UI sub-states — one concern each, declared next to the Model that owns them
// ============================================================================

struct ComposerState {
    std::string text;
    int  cursor   = 0;
    bool expanded = false;
    std::vector<std::string> queued;
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
        // Index of the first message the view should render.  Messages
        // before this point are committed to the terminal's native
        // scrollback (maya's InlineFrameState::commit_prefix was called
        // for their rows).  Advancing this counter + returning
        // Cmd::commit_scrollback keeps the Yoga/paint cost bounded to the
        // visible window, not the full transcript.
        int                 thread_view_start = 0;
        // Number of finalized assistant messages BEFORE thread_view_start.
        // The view shows the running turn number ("turn 42") and previously
        // recomputed it by walking m.d.current.messages from 0 to
        // thread_view_start every frame — O(thread_view_start), which grows
        // linearly with the conversation as virtualization advances.
        // Caching it here makes the per-frame turn-numbering cost O(1)
        // regardless of how long the session has run.  Mutated only in
        // maybe_virtualize alongside thread_view_start; resets to 0 on
        // thread switch (same lifecycle as thread_view_start).
        int                 thread_view_start_turn = 0;

        // Per-(thread, msg) render cache. View code reads + writes
        // through this — markdown rendering and per-turn Element
        // building both memoize here. The reducer evicts entries on
        // thread switch / NewThread / compaction, where the cached
        // Element trees would otherwise pin the previous turn's UI
        // forever.
        //
        // Mutable: filling a render cache during view doesn't change
        // observable Model state (every cache hit returns the same
        // Element it would have rebuilt from scratch), and the view
        // path takes `const Model&` by convention. Earlier this lived
        // as a process-global thread_local map outside Model — the
        // pointer-equivalence was identical but the reducer was
        // reaching outside Model to call evict_thread(), which type-
        // checked but lied about purity. Now every cache mutation is
        // visible in the returned Model.
        mutable ui::ViewCache view_cache;
    };

    Domain      d;
    StreamState s;
    UI          ui;
};

} // namespace moha
