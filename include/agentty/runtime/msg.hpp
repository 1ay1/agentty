#pragma once
// agentty::Msg — every event the runtime can process, as a closed grouped variant.
//
// History:
//   v1: 79 leaf alternatives in one std::variant. sizeof(Msg) = max over all 79
//       leaves; one std::visit call instantiated a 79-arm dispatch table; one
//       update.cpp TU compiled all 79 arms in a single overload{}. Tolerable at
//       first; began to wobble with scale — compile time of update.cpp climbed
//       past 15 s, sizeof(Msg) was pinned by the heaviest alternative no matter
//       which path was active, and any leaf change forced a full recompile of
//       the dispatch site.
//
//   v2 (this file): leaves grouped into 10 domain sub-variants. The top-level
//       Msg is a `std::variant` of those domains. The flat construction syntax
//       still works:
//
//           Msg m1 = ComposerEnter{};       // -> Msg{ComposerMsg{ComposerEnter{}}}
//           dispatch(StreamTextDelta{"x"}); // -> dispatch(Msg{StreamMsg{...}})
//
//       std::variant's converting constructor walks each alternative; only the
//       matching domain accepts a given leaf, so the wrap is unambiguous and
//       implicit. Call sites don't change.
//
//       Per-domain reducers live in update/<domain>.cpp; update.cpp's top-level
//       std::visit is now a 10-arm dispatcher that forwards into them. Each
//       domain's TU compiles independently — touching a composer leaf no
//       longer forces stream/login/diff to recompile.

#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "agentty/auth/auth.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/tool/registry.hpp"

namespace agentty {

// ============================================================================
// Leaf types — the actual events. Grouped by domain in source order; the
// runtime classification is enforced by the domain variants further down.
// ============================================================================

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
// Recall queued messages back into the composer for editing. Bound to
// Up-arrow when the composer is empty and queued messages exist.
// Mirrors Claude Code's `Lc_` (binary offset 76303220): drains every
// editable queued item into the composer in queue order joined by
// "\n", with the cursor landing at the boundary between recalled text
// and any pre-existing composer text. Destructive on the queue — the
// items only exist in the composer buffer afterwards, so the user
// must resubmit to re-queue. If the user clears the composer after
// recall, the items are gone (same as Claude Code's behaviour).
struct ComposerRecallQueued {};

// Per-item queue editor. The bigger sibling of ComposerRecallQueued:
// rather than drain-all, these let the user cycle THROUGH the queued
// items, load one at a time into the composer, edit it, and submit
// (which re-queues at the tail — the existing flow). Bound to Alt+↑ /
// Alt+↓ / Alt+Backspace.
//
//   Alt+↑  ComposerQueuePeekPrev  — load the previous queued item into
//                                    the composer for editing. On the
//                                    first press, the live draft is
//                                    snapshotted into composer.draft_save
//                                    so the round-trip back is non-
//                                    destructive (same idea as history
//                                    walk). The item stays in the queue
//                                    while being peeked; submitting
//                                    removes it from the queue and re-
//                                    queues the edited version at the
//                                    tail.
//   Alt+↓  ComposerQueuePeekNext  — load the next queued item; at the
//                                    end of the queue, restore the
//                                    live draft and exit peek mode.
//   Alt+Backspace (on empty composer with no peek active)
//          ComposerQueuePopLast   — pop the most recently queued item
//                                    off the tail entirely. Undo for an
//                                    accidental queue submit.
struct ComposerQueuePeekPrev {};
struct ComposerQueuePeekNext {};
struct ComposerQueuePopLast {};

// Word-wise cursor jumps (Ctrl+Left / Ctrl+Right). Word boundaries
// are whitespace runs; chip placeholders count as a single word.
struct ComposerCursorWordLeft {};
struct ComposerCursorWordRight {};
// Kill-line family (Ctrl+K / Ctrl+U). Kill-to-end deletes from the
// cursor to the next '\n' (or end-of-buffer); kill-to-beginning
// deletes from the previous '\n' (or start-of-buffer) to the cursor.
struct ComposerKillToEndOfLine {};
struct ComposerKillToBeginningOfLine {};
// Undo / redo (Ctrl+Z / Ctrl+Y). Each mutating composer op snapshots
// the prior state into a per-composer stack; new edits clear redo.
struct ComposerUndo {};
struct ComposerRedo {};
// History walking — ↑/↓ over previous user messages in the active
// thread. Prev steps further into the past; Next walks back toward
// the live draft (which was snapshotted on the first Prev).
struct ComposerHistoryPrev {};
struct ComposerHistoryNext {};
// Explicit "paste image from system clipboard" (Ctrl+V). Bracketed
// paste (Ctrl+Shift+V) only carries UTF-8 text — for an image-on-
// clipboard path the reducer shells out to wl-paste / xclip /
// pngpaste / PowerShell to capture the raw PNG bytes. See
// io/clipboard.{hpp,cpp}.
struct ComposerImagePasteFromClipboard {};

// ── Streaming from provider ──────────────────────────────────────────────
struct StreamStarted {};
struct StreamTextDelta { std::string text; };
struct StreamToolUseStart { ToolCallId id; ToolName name; };
struct StreamToolUseDelta { std::string partial_json; };
struct StreamToolUseEnd {};
// Mirrors Anthropic's message.usage shape. cache_* fields are non-zero only
// when the request hit a cache_control breakpoint. Fields default to 0 so
// callers that only care about input/output keep working.
struct StreamUsage {
    int input_tokens               = 0;
    int output_tokens              = 0;
    int cache_creation_input_tokens = 0;
    int cache_read_input_tokens    = 0;
};
// Why the stream ended. Maps Anthropic's wire string
// (`message_delta.delta.stop_reason`: "end_turn" | "tool_use" |
// "max_tokens" | "stop_sequence" | absent) into a closed enum so the
// reducer can `switch` on it without string compare. Anything the wire
// doesn't recognise (forward-compat: a future Anthropic stop reason)
// becomes `Unspecified` — handled identically to a missing field, which
// means "treat as a clean stream end."
enum class StopReason : std::uint8_t {
    EndTurn,        // model finished naturally
    ToolUse,        // model wants tool results before continuing
    MaxTokens,      // hit the output token cap mid-stream
    StopSequence,   // matched a configured stop_sequence
    Unspecified,    // wire absent, empty, or unknown
};

[[nodiscard]] constexpr std::string_view to_string(StopReason r) noexcept {
    switch (r) {
        case StopReason::EndTurn:      return "end_turn";
        case StopReason::ToolUse:      return "tool_use";
        case StopReason::MaxTokens:    return "max_tokens";
        case StopReason::StopSequence: return "stop_sequence";
        case StopReason::Unspecified:  return "";
    }
    return "";
}

// Inverse: parse a wire string into the typed enum. Used at the
// dynamism boundary in `provider/anthropic/transport.cpp`. Unrecognised
// values become `Unspecified` so a future Anthropic addition doesn't
// crash the reducer.
[[nodiscard]] constexpr StopReason parse_stop_reason(std::string_view s) noexcept {
    if (s == "end_turn")      return StopReason::EndTurn;
    if (s == "tool_use")      return StopReason::ToolUse;
    if (s == "max_tokens")    return StopReason::MaxTokens;
    if (s == "stop_sequence") return StopReason::StopSequence;
    return StopReason::Unspecified;
}

struct StreamFinished { StopReason stop_reason = StopReason::Unspecified; };
// Stream-level failure. `message` is human-readable (used for both the
// status banner and `provider::classify(string)` fallback). `retry_after`
// is the server's Retry-After hint when present — Anthropic sets it on
// 429 (rate_limit_error) and 529 (overloaded_error); the runtime prefers
// this over its hardcoded backoff schedule because the server knows
// better than we do how long the brown-out will last (see Zed's
// `parse_retry_after`, anthropic.rs:574-580). std::chrono::seconds
// because Anthropic always emits whole seconds; clamped at the use site
// so a buggy proxy can't pin us for an hour.
struct StreamError {
    std::string message;
    std::optional<std::chrono::seconds> retry_after;
};
// Wire-alive heartbeat. Emitted by the transport for SSE frames that
// carry no reducer-visible payload but prove the connection is healthy
// and the model is still working: SSE `ping` events (Anthropic's proxy
// keepalive, every 10-15 s), and `thinking_delta` blocks (extended-
// thinking models emit these while reasoning silently — no `text` /
// `input_json` for seconds or minutes at a time). Without this Msg the
// reducer's stall watchdog can't distinguish "model is thinking" from
// "transport is wedged" and fires spurious "stream stalled" errors.
// The handler does nothing but bump `last_event_at` — no render
// churn, no state transitions, no visible UI effect.
struct StreamHeartbeat {};
// User-driven cancel of the in-flight stream (Esc while streaming). The
// reducer trips the StreamState cancel token; the http layer notices within
// ~200 ms and the worker thread eventually emits a StreamError("cancelled").
struct CancelStream {};
// Scheduled re-launch of the in-flight stream after a transient-error
// backoff (Overloaded / 429 / 5xx / network blip). The reducer issues
// `Cmd::after(delay, RetryStream{})` from the StreamError handler;
// when this Msg fires, the stream is re-launched on the same context.
// The user can intercept with Esc → CancelStream during the wait.
struct RetryStream {};

// ── Tool execution (local) ───────────────────────────────────────────────
// Tool finished executing. `result` is `expected<output_text, ToolError>`
// — the success/failure distinction is the type, not a parallel `bool error`
// flag. Reducer dispatches via `std::visit` (or the `if (e.result)` short
// form for the common case); the typed `ToolError::kind` flows all the way
// to the view, where it could drive different rendering per category.
struct ToolExecOutput {
    ToolCallId id;
    std::expected<std::string, tools::ToolError> result;
};
// Live progress snapshot from a running tool (e.g. bash stdout+stderr so far).
// Contains the FULL accumulated output, not a delta — the update handler can
// assign unconditionally without maintaining append state. Coalesced at the
// subprocess boundary (~100 ms) so a chatty command doesn't flood the event
// queue with micro-updates.
struct ToolExecProgress { ToolCallId id; std::string snapshot; };
// Wall-clock watchdog for tool execution. Scheduled by kick_pending_tools
// via Cmd::after when a non-subprocess tool transitions to Running. If the
// tool has reached a terminal state by the time the check fires, this is
// a no-op; otherwise the tool is force-failed so the UI doesn't sit on a
// hung filesystem call / blocked syscall forever. The worker thread that
// owns the tool may keep running — its eventual ToolExecOutput is silently
// discarded by apply_tool_output's idempotent guard.
struct ToolTimeoutCheck { ToolCallId id; };

// Permission-prompt resolution from the user. Tied to ToolMsg because a
// permission prompt is always about a specific pending tool call and the
// resolution feeds straight back into the tool state machine.
struct PermissionApprove {};
struct PermissionReject {};
struct PermissionApproveAlways {};

// ── Model picker ─────────────────────────────────────────────────────────
struct OpenModelPicker {};
struct CloseModelPicker {};
struct ModelPickerMove { int delta; };
struct ModelPickerSelect {};
struct ModelPickerToggleFavorite {};
struct ModelsLoaded { std::vector<ModelInfo> models; };

// ── Thread list ──────────────────────────────────────────────────────────
struct OpenThreadList {};
struct CloseThreadList {};
struct ThreadListMove { int delta; };
struct ThreadListSelect {};
struct NewThread {};
// Result of the background thread-history load kicked off from
// `AgenttyApp::init()`. The on-disk thread JSON walk used to run
// synchronously on startup; with hundreds of multi-MB files (real-world
// usage) it was the dominant startup cost (~1.7 s for 643 threads at
// 376 MB total). Now `init()` returns immediately with an empty
// `m.d.threads` and a `Cmd::task` that does the directory walk +
// JSON parse off the UI thread; this Msg lands when it's done.
struct ThreadsLoaded    { std::vector<Thread> threads; };

// ── Command palette ──────────────────────────────────────────────────────
struct OpenCommandPalette {};
struct CloseCommandPalette {};
struct CommandPaletteInput { char32_t ch; };
struct CommandPaletteBackspace {};
struct CommandPaletteMove { int delta; };
struct CommandPaletteSelect {};

// ── @file mention picker ────────────────────────────────────────────────
struct OpenMentionPalette {};
struct CloseMentionPalette {};
struct MentionPaletteInput { char32_t ch; };
struct MentionPaletteBackspace {};
struct MentionPaletteMove { int delta; };
struct MentionPaletteSelect {};

// ── #symbol picker (parallel to @file) ──────────────────────────────────
struct OpenSymbolPalette {};
struct CloseSymbolPalette {};
struct SymbolPaletteInput { char32_t ch; };
struct SymbolPaletteBackspace {};
struct SymbolPaletteMove { int delta; };
struct SymbolPaletteSelect {};

// ── Todo modal ───────────────────────────────────────────────────────────
struct OpenTodoModal {};
struct CloseTodoModal {};
struct UpdateTodos { std::vector<TodoItem> items; };

// ── In-app login modal ───────────────────────────────────────────────────
// Shown when the user starts agentty with no valid credentials, OR
// triggered explicitly from the command palette to switch accounts.
// Same state-machine flavor as the other modals: closed → picking →
// {oauth_code | api_key_input} → done. The async OAuth exchange runs
// on a worker thread (Cmd::task) and reports back via LoginExchanged.
struct OpenLogin {};
struct CloseLogin {};
struct LoginPickMethod  { char32_t key; };          // '1' = OAuth, '2' = ApiKey
struct LoginCharInput   { char32_t ch; };
struct LoginBackspace   {};
struct LoginPaste       { std::string text; };
struct LoginCursorLeft  {};
struct LoginCursorRight {};
struct LoginSubmit      {};
// Result of the async OAuth code-exchange. Carries the typed
// `auth::TokenResult` so the reducer can distinguish ApiError /
// Network / MissingToken without parsing strings.
struct LoginExchanged   { agentty::auth::TokenResult result; };
// Result of the background OAuth refresh kicked off from init() when
// `auth::resolve()` returned an expired token paired with a refresh
// token. Same TokenResult shape as LoginExchanged, handled in
// update/login.cpp::token_refreshed: success installs the new creds via
// `update_auth` + saves to disk + drains any queued composer text;
// failure surfaces an `error: token refresh failed: ...` toast and
// leaves the queue intact so the user can retry through the in-app
// login modal.
struct TokenRefreshed   { agentty::auth::TokenResult result; };

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

// ── Meta / session-level ─────────────────────────────────────────────────
// CompactContext, Tick, Quit, NoOp, ClearStatus, CycleProfile,
// RestoreCheckpoint, ScrollThread, ToggleToolExpanded — all events that
// are conceptually "above" any single domain (the session itself, the
// tick clock, profile mode, etc.).

// Auto / manual conversation compaction. Mirrors Claude Code 2.1.119's
// `BetaToolRunner.compactionControl` (binary near offset 134600). When
// the running input-token total approaches the model's context window,
// or the user invokes "Compact context" from the palette, the runtime
// appends a synthetic User message asking the model to summarise the
// conversation per a structured schema; the resulting assistant text is
// then promoted to a single User message that REPLACES the entire
// conversation history. The next turn proceeds against the compacted
// prefix as if the summary were the only prior context. m.s.compacting
// is the in-flight flag — set on dispatch, cleared on the StreamFinished
// that lands the summary.
struct CompactContext {};

struct CycleProfile {};
struct RestoreCheckpoint { CheckpointId id; };
struct ScrollThread { int delta; };
struct ToggleToolExpanded { ToolCallId id; };

struct Tick {};
struct Quit {};
struct NoOp {};
// User-triggered "drop the renderer's cell cache and repaint from
// scratch". Bound to Ctrl-L (universal terminal redraw convention).
// Dispatches `Cmd::force_redraw()`, which mirrors the SIGWINCH
// coherence-collapse — no scrollback wipe, just an in-place rebuild
// of `prev_cells` from the current canvas. Doubles as a debug hatch
// when something visibly desyncs.
struct RedrawScreen {};
// Delayed sentinel that clears `m.s.status` iff it hasn't been
// overwritten since the toast was scheduled. `stamp` is the value
// `m.s.status_until` had at schedule time; if the reducer has since
// written a newer status, stamps won't match and this Msg is a no-op.
struct ClearStatus { std::chrono::steady_clock::time_point stamp; };

// ============================================================================
// Domain variants — one per orthogonal slice of the runtime. Each is a
// `std::variant` over its leaves; per-domain reducers visit on these.
// ============================================================================
namespace msg {

using ComposerMsg = std::variant<
    ComposerCharInput, ComposerBackspace, ComposerEnter, ComposerNewline,
    ComposerSubmit, ComposerToggleExpand,
    ComposerCursorLeft, ComposerCursorRight, ComposerCursorHome, ComposerCursorEnd,
    ComposerCursorWordLeft, ComposerCursorWordRight,
    ComposerKillToEndOfLine, ComposerKillToBeginningOfLine,
    ComposerUndo, ComposerRedo,
    ComposerHistoryPrev, ComposerHistoryNext,
    ComposerImagePasteFromClipboard,
    ComposerPaste, ComposerRecallQueued,
    ComposerQueuePeekPrev, ComposerQueuePeekNext, ComposerQueuePopLast>;

using StreamMsg = std::variant<
    StreamStarted, StreamTextDelta,
    StreamToolUseStart, StreamToolUseDelta, StreamToolUseEnd,
    StreamUsage, StreamFinished, StreamError, StreamHeartbeat,
    CancelStream, RetryStream>;

using ToolMsg = std::variant<
    ToolExecOutput, ToolExecProgress, ToolTimeoutCheck,
    PermissionApprove, PermissionReject, PermissionApproveAlways>;

using ModelPickerMsg = std::variant<
    OpenModelPicker, CloseModelPicker, ModelPickerMove, ModelPickerSelect,
    ModelPickerToggleFavorite, ModelsLoaded>;

using ThreadListMsg = std::variant<
    OpenThreadList, CloseThreadList, ThreadListMove, ThreadListSelect,
    NewThread, ThreadsLoaded>;

using CommandPaletteMsg = std::variant<
    OpenCommandPalette, CloseCommandPalette, CommandPaletteInput,
    CommandPaletteBackspace, CommandPaletteMove, CommandPaletteSelect>;

using MentionPaletteMsg = std::variant<
    OpenMentionPalette, CloseMentionPalette, MentionPaletteInput,
    MentionPaletteBackspace, MentionPaletteMove, MentionPaletteSelect>;

using SymbolPaletteMsg = std::variant<
    OpenSymbolPalette, CloseSymbolPalette, SymbolPaletteInput,
    SymbolPaletteBackspace, SymbolPaletteMove, SymbolPaletteSelect>;

using TodoMsg = std::variant<
    OpenTodoModal, CloseTodoModal, UpdateTodos>;

using LoginMsg = std::variant<
    OpenLogin, CloseLogin, LoginPickMethod, LoginCharInput, LoginBackspace,
    LoginPaste, LoginCursorLeft, LoginCursorRight, LoginSubmit,
    LoginExchanged, TokenRefreshed>;

using DiffReviewMsg = std::variant<
    OpenDiffReview, CloseDiffReview, DiffReviewMove,
    DiffReviewNextFile, DiffReviewPrevFile,
    AcceptHunk, RejectHunk, AcceptAllChanges, RejectAllChanges>;

using MetaMsg = std::variant<
    CompactContext, CycleProfile, RestoreCheckpoint,
    ScrollThread, ToggleToolExpanded,
    Tick, Quit, NoOp, ClearStatus, RedrawScreen>;

} // namespace msg

// ============================================================================
// Msg — top-level grouped variant. Construction from any leaf works because
// std::variant's converting constructor walks each alternative; only the
// matching domain accepts a given leaf, so the wrap is unambiguous and
// implicit:
//
//   Msg m = ComposerEnter{};       //  -> Msg{ComposerMsg{ComposerEnter{}}}
//   Cmd<Msg>::after(d, RetryStream{}); // same path
//
// std::visit on a Msg dispatches on domain, not leaf — see update.cpp.
// ============================================================================
using Msg = std::variant<
    msg::ComposerMsg,
    msg::StreamMsg,
    msg::ToolMsg,
    msg::ModelPickerMsg,
    msg::ThreadListMsg,
    msg::CommandPaletteMsg,
    msg::MentionPaletteMsg,
    msg::SymbolPaletteMsg,
    msg::TodoMsg,
    msg::LoginMsg,
    msg::DiffReviewMsg,
    msg::MetaMsg
>;

} // namespace agentty
