#pragma once
// Shared internals for the update/* translation units. Not part of the public
// agentty::app interface — external callers go through agentty::app::update() in
// update.hpp. Lives under include/ rather than a private src/ header so the
// three update/*.cpp files and update.cpp can all see the same declarations.

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include <maya/maya.hpp>
#include <nlohmann/json.hpp>

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"

namespace agentty::app {

using Step = std::pair<Model, maya::Cmd<Msg>>;
inline Step done(Model m) { return {std::move(m), maya::Cmd<Msg>::none()}; }

namespace detail {

// Hard cap on per-message live buffers. A misbehaving server (or adversarial
// proxy) emitting unbounded `text_delta`/`input_json_delta` would otherwise
// grow `streaming_text` / `args_streaming` until the process OOMs. 8 MiB is
// far above any realistic single-message body — hitting this cap means
// something genuinely broken upstream, not a real workload.
inline constexpr std::size_t kMaxStreamingBytes = 8 * 1024 * 1024;

// View virtualization thresholds — when the transcript exceeds kViewWindow
// messages, slice kSliceChunk of the oldest into terminal scrollback so the
// per-frame Yoga layout pass stays bounded.
//
// Per-Element caching (715679f) eliminated the per-frame Turn::build()
// rebuild for settled turns — but maya still walks the full visible
// element tree through Yoga every frame, and layout cost scales linearly
// with node count. Tool-heavy sessions (Read / Grep / Bash cards stacked
// under a single user message) easily blow past the per-message average:
// one assistant message with 5+ tool rounds is 100-200 nodes on its own.
// With kViewWindow = 40 the live canvas reached 5000+ nodes, render
// latency hit ~Tick interval at the bottom of the visible window, and the
// composer's redraw started to lag behind keystrokes (a "flicker that
// becomes stuck hiding the composer" — the next frame falls behind the
// terminal's actual cursor position).
//
// 20/8 caps the live tree to roughly 2-3 turns worth of tool cards (about
// 1000-1500 nodes), which fits inside one Tick on modest hardware. The
// trade-off is that the user sees fewer scrollback turns "above the fold"
// in the live canvas — but committed turns remain in the terminal's
// native scrollback, which is where Page-Up / mouse-wheel land anyway.
//
// History:
//   60/20 → 40/15 (rendered-canvas-rows-bound spike on long sessions)
//   40/15 → 20/8  (Yoga layout cost on tool-heavy turns)
inline constexpr int kViewWindow = 20;
inline constexpr int kSliceChunk = 8;

// ── update_stream.cpp ────────────────────────────────────────────────────
void update_stream_preview(ToolUse& tc);
bool guard_truncated_tool_args(ToolUse& tc);
nlohmann::json salvage_args(const ToolUse& tc);
maya::Cmd<Msg> finalize_turn(Model& m, StopReason stop_reason = StopReason::Unspecified);

// Sync the persistent plan state (m.ui.todo.items) from a `todo` tool
// call's args["todos"] array. Called live during arg streaming AND at
// tool-exec output so the modal + any global indicator track the
// in-progress item the instant the model writes it. No-op when args
// carries no todos array (partial early stream).
void sync_todo_state_from_args(Model& m, const nlohmann::json& args);

// ── update/modal.cpp helpers ─────────────────────────────────────────────
Step           submit_message(Model m);
void           persist_settings(const Model& m);

// ── Frozen-scrollback prefix helpers (frozen.cpp) ────────────────────────
//
// freeze_through_prior_turn: walk m.d.current.messages[frozen_through..end)
// and push built Turn Elements (with leading gaps) into m.ui.frozen,
// up to (but NOT including) the message at `live_start`. Applies the
// same tool-batch-merge logic conversation_config used to do at view
// time, so the frozen visual matches the live visual byte-for-byte.
//
// Typical call: at submit_message, freeze through the just-finished
// agent turn (live_start = messages.size() at the moment the new User
// is about to be pushed).
void freeze_through(Model& m, std::size_t live_start);

// freeze_settled_subturns: mid-run incremental freeze. During an ACTIVE
// auto-pilot run the trailing Assistant run can't be frozen by
// freeze_through (its tail is still streaming / has a running tool), so
// the WHOLE run — every completed edit/write sub-turn included — stays
// in the live tail and is re-laid-out + re-painted every frame. As edits
// accumulate the live canvas grows without bound and per-frame cost
// (canvas clear + layout + compose, all O(rows)) climbs with tool count.
//
// This freezes the COMPLETED leading sub-turns of the active run up to
// (but not including) the first non-terminal sub-turn, committing them
// into m.ui.frozen as a continuation entry. frozen_turn is NOT advanced
// (the run isn't finished); frozen_midrun is set so the live tail draws
// the remainder as a continuation (rail only, no repeated header/gap).
// The live frame then holds only the active sub-turn — flat per-frame
// cost regardless of how many tools the turn has run. No content is
// hidden: the frozen sub-turns render in full via the zero-copy frozen
// list, exactly as a settled turn would. No-op when the active run has
// no freezable completed prefix.
void freeze_settled_subturns(Model& m);

// freeze_streaming_text_prefix: mid-stream bound for a long PURE-TEXT
// sub-turn (a prose answer with no tool calls). freeze_settled_subturns
// can't touch it — there's no terminal tool to mark the sub-turn done,
// so the whole growing markdown body stays in the live tail and is
// re-laid-out + re-painted every frame. Render cost climbs with body
// size (~13 ms/frame at 5k lines, vs 0.26 ms flat for a tail-windowed
// tool card).
//
// This splits the active text-only message at the last SAFE markdown
// block boundary (a blank line outside an open code fence), keeping a
// trailing window live so the actively-revealing edge still animates.
// The committed prefix is moved into its own settled Assistant Message
// inserted just before the active one, producing a [settled-text]
// [growing-text] run — the exact shape a post-tool continuation makes,
// which freeze_settled_subturns then freezes with no new render logic.
// The active (tail) message stays messages.back() so the StreamTextDelta
// / StreamToolUseStart append-to-back invariant holds. No-op until the
// committed prefix exceeds a row budget.
void freeze_streaming_text_prefix(Model& m);

// rehydrate_frozen: rebuild m.ui.frozen from scratch from the current
// thread's messages + compaction records. Used on thread switch /
// thread load — anywhere the messages vector was replaced wholesale.
// Resets frozen_through and frozen_turn.
void rehydrate_frozen(Model& m);

// clear_frozen: drop the entire frozen vector and reset counters.
// For NewThread before a fresh-start submit.
void clear_frozen(Model& m);

// ensure_frozen_width: re-measure every frozen entry's stored row count
// (frozen_rows[]) at `term_cols` (the FULL terminal width) whenever it has
// changed since the counts were stamped (m.ui.frozen_cols). No-op when the
// width is unchanged — the common case.
//
// frozen_rows[k] equals what maya emits for entry k ONLY at the width it was
// measured at; push_frozen measures through maya's real layout engine, so a
// fresh stamp matches the wire by construction at that width. A terminal
// resize re-wraps every entry to a new height. Both trims size their
// commit_scrollback() off frozen_rows[]; a stale post-WIDEN stamp over-counts
// the wire, the trim over-commits, maya shifts prev_cells up by more rows
// than the live tree shrank, and the next compose "grows" — scrolling kept
// content into native scrollback as a duplicate just above the viewport.
// Re-measuring to the live width before each trim consumes the counts keeps
// the exact commit exact under any resize, making that over-commit
// structurally impossible. Called by push_frozen and both trims; exposed
// (rather than file-local) so the resize path is unit-testable. O(entries)
// layout, only on a width change.
void ensure_frozen_width(Model& m, int term_cols);

// trim_frozen_if_oversized: when frozen exceeds a soft cap, drop the
// oldest N entries to keep maya's prev_cells working set bounded.
// Returns a Cmd::commit_scrollback_overflow() to tell maya to release
// the cells that have provably overflowed the viewport. No-op if
// frozen is under the cap.
maya::Cmd<Msg> trim_frozen_if_oversized(Model& m);

// trim_frozen_above_viewport: mid-run-SAFE variant of the trim. Drops
// front entries ONLY while at least `term_h` rows of the most recent
// frozen content remain on the canvas, so every dropped entry has
// provably overflowed into native terminal scrollback already (its
// rows are committed there, identical to what we'd re-emit). This is
// the missing bound during a single long auto-pilot run: a turn with
// many big tool panels grows frozen_row_total past the budget, and
// without trimming, render_tree + canvas.clear() + the shadow verify
// all walk the whole oversized canvas every frame (the progressive
// slowdown). Unlike trim_frozen_if_oversized this NEVER drops an
// on-screen entry, so it can't trigger the mid-run duplication bug.
// Returns commit_scrollback(removed_rows) — EXACTLY the rows it
// dropped — when it drops anything; no-op otherwise.
//
// NOTE: wired into the live mid-run path — tool.cpp (after each tool
// settles) and meta.cpp's Tick (after the per-tick freeze). It keeps
// ~3x term_h rows on the canvas (a margin chosen to absorb the byte-
// based wrap over-count in estimate_msg_rows, so real-kept >= term_h
// even for all-multibyte prose) and only fires once frozen_row_total
// exceeds ~4x term_h, so it trims infrequently. Its commit is row-exact
// (commit_scrollback(removed), not commit_scrollback_overflow) AND maya
// clamps that count to (prev_rows - term_h) in commit_inline_prefix —
// two independent nets, so an on-screen row can never be committed and
// no duplicate is ever stranded. Also exercised by o1_probe + seam
// tests.
maya::Cmd<Msg> trim_frozen_above_viewport(Model& m);

// Set a transient status toast that auto-clears after `ttl`. Returns a
// Cmd that schedules the ClearStatus sentinel (stamp-matched so a newer
// status overwrites without being wiped). Use for "no-op" feedback like
// "no pending changes" / "nothing to copy" — anywhere the alternative
// is silent failure that leaves the user wondering if their keystroke
// even registered.
maya::Cmd<Msg> set_status_toast(Model& m, std::string text,
                                std::chrono::seconds ttl = std::chrono::seconds{3});

// ── update/stream.cpp helpers ────────────────────────────────────────────
// (declared at module scope above — `update_stream_preview`, `salvage_args`,
// `finalize_turn`. The stream_update reducer below uses them.)

// ── update/tool.cpp helpers ──────────────────────────────────────────────
void apply_tool_output(Model& m, const ToolCallId& id,
                       std::expected<std::string, tools::ToolError>&& result);
void mark_tool_rejected(Model& m, const ToolCallId& id,
                        std::string_view reason);

// ── Frozen-prefix immutability gate ──────────────────────────────────────
//
// `m.ui.frozen` is an append-only vector of fully-built Element
// snapshots; their `hash_id` is stamped at freeze time and never
// recomputed, so any post-freeze mutation of the underlying ToolUse
// is invisible until thread switch / rehydrate. The five mutation
// sites that locate a tool by ToolCallId (ToggleToolExpanded,
// ToolExecOutput / apply_tool_output, ToolExecProgress, ToolTimeoutCheck,
// PermissionReject / mark_tool_rejected) must therefore refuse to touch
// any tool whose enclosing message has index < frozen_through.
//
// `with_live_tool` is the only way to mutate a tool by id. It searches
// ONLY the live tail [frozen_through .. end) and returns true iff the
// mutation ran. A stale id (tool whose turn was already frozen) returns
// false — caller treats it as a no-op, matching the existing
// "idempotent on terminal" behaviour of apply_tool_output.
template <class F>
bool with_live_tool(Model& m, const ToolCallId& id, F&& f) {
    for (std::size_t i = m.ui.frozen_through;
         i < m.d.current.messages.size(); ++i) {
        for (auto& tc : m.d.current.messages[i].tool_calls) {
            if (tc.id == id) {
                std::forward<F>(f)(tc);
                return true;
            }
        }
    }
    return false;
}

// ── Per-domain reducers ──────────────────────────────────────────────────
// One per slice of `Msg`. update.cpp's top-level std::visit dispatches a
// Msg to the matching reducer below; each reducer has its own visit over
// its domain variant, instantiated in its own TU. Adding a leaf to one
// domain only recompiles that domain's TU plus msg.hpp's downstream
// includers — not the other nine reducers.
Step composer_update      (Model m, msg::ComposerMsg       cm);
Step stream_update        (Model m, msg::StreamMsg         sm);
Step tool_update          (Model m, msg::ToolMsg           tm);
Step model_picker_update  (Model m, msg::ModelPickerMsg    pm);
Step thread_list_update   (Model m, msg::ThreadListMsg     tm);
Step palette_update       (Model m, msg::CommandPaletteMsg pm);
Step mention_update       (Model m, msg::MentionPaletteMsg mm);
Step symbol_update        (Model m, msg::SymbolPaletteMsg  sm);
Step todo_update          (Model m, msg::TodoMsg           tm);
Step login_update         (Model m, msg::LoginMsg          lm);
Step diff_review_update   (Model m, msg::DiffReviewMsg     dm);
Step meta_update          (Model m, msg::MetaMsg           mm);

} // namespace detail
} // namespace agentty::app
