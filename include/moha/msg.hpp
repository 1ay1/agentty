#pragma once
// moha::Msg — every event the runtime can process, as a closed variant.

#include <string>
#include <variant>

#include "moha/model.hpp"

namespace moha {

// ── Composer ─────────────────────────────────────────────────────────────
struct ComposerCharInput { char32_t ch; };
struct ComposerBackspace {};
struct ComposerEnter {};
struct ComposerNewline {};
struct ComposerSubmit {};
struct ComposerToggleExpand {};
struct ComposerCursorLeft {};
struct ComposerCursorRight {};
struct ComposerCursorHome {};
struct ComposerCursorEnd {};
struct ComposerPaste { std::string text; };

// ── Streaming from provider ──────────────────────────────────────────────
struct StreamStarted {};
struct StreamTextDelta { std::string text; };
struct StreamToolUseStart { ToolCallId id; ToolName name; };
struct StreamToolUseDelta { std::string partial_json; };
struct StreamToolUseEnd {};
struct StreamUsage { int input_tokens; int output_tokens; };
struct StreamFinished {};
struct StreamError { std::string message; };

// ── Tool execution (local) ───────────────────────────────────────────────
struct ToolExecOutput { ToolCallId id; std::string output; bool error; };

// ── Permission ───────────────────────────────────────────────────────────
struct PermissionApprove {};
struct PermissionReject {};
struct PermissionApproveAlways {};

// ── Navigation / modals ──────────────────────────────────────────────────
struct OpenModelPicker {};
struct CloseModelPicker {};
struct ModelPickerMove { int delta; };
struct ModelPickerSelect {};
struct ModelPickerToggleFavorite {};

struct OpenThreadList {};
struct CloseThreadList {};
struct ThreadListMove { int delta; };
struct ThreadListSelect {};
struct NewThread {};

struct OpenCommandPalette {};
struct CloseCommandPalette {};
struct CommandPaletteInput { char32_t ch; };
struct CommandPaletteBackspace {};
struct CommandPaletteMove { int delta; };
struct CommandPaletteSelect {};

// ── Profile / mode ───────────────────────────────────────────────────────
struct CycleProfile {};

// ── Diff review ──────────────────────────────────────────────────────────
struct OpenDiffReview {};
struct CloseDiffReview {};
struct DiffReviewMove { int delta; };
struct DiffReviewNextFile {};
struct DiffReviewPrevFile {};
struct AcceptHunk {};
struct RejectHunk {};
struct AcceptAllChanges {};
struct RejectAllChanges {};

// ── Checkpoint ───────────────────────────────────────────────────────────
struct RestoreCheckpoint { CheckpointId id; };

// ── Thread / misc ────────────────────────────────────────────────────────
struct ScrollThread { int delta; };
struct ToggleToolExpanded { ToolCallId id; };

// ── Tick / meta ──────────────────────────────────────────────────────────
struct Tick {};
struct Quit {};
struct NoOp {};

using Msg = std::variant<
    ComposerCharInput, ComposerBackspace, ComposerEnter, ComposerNewline,
    ComposerSubmit, ComposerToggleExpand,
    ComposerCursorLeft, ComposerCursorRight, ComposerCursorHome, ComposerCursorEnd,
    ComposerPaste,
    StreamStarted, StreamTextDelta,
    StreamToolUseStart, StreamToolUseDelta, StreamToolUseEnd,
    StreamUsage, StreamFinished, StreamError,
    ToolExecOutput,
    PermissionApprove, PermissionReject, PermissionApproveAlways,
    OpenModelPicker, CloseModelPicker, ModelPickerMove, ModelPickerSelect, ModelPickerToggleFavorite,
    OpenThreadList, CloseThreadList, ThreadListMove, ThreadListSelect, NewThread,
    OpenCommandPalette, CloseCommandPalette, CommandPaletteInput,
    CommandPaletteBackspace, CommandPaletteMove, CommandPaletteSelect,
    CycleProfile,
    OpenDiffReview, CloseDiffReview, DiffReviewMove,
    DiffReviewNextFile, DiffReviewPrevFile,
    AcceptHunk, RejectHunk, AcceptAllChanges, RejectAllChanges,
    RestoreCheckpoint,
    ScrollThread, ToggleToolExpanded,
    Tick, Quit, NoOp
>;

} // namespace moha
