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

// Clear ALL transient composer draft state (text, cursor, attachments,
// undo/redo, history walk, queued messages, queue-peek/draft snapshots).
// Used on a wholesale thread swap (NewThread / ThreadLoaded): a draft —
// including a pasted-but-unsent image attachment — belongs to the thread
// the user was on, not the one they switched to. Leaking it carried the
// pasted image (with its bytes already drained into a prior Message, so
// the leftover Attachment body is EMPTY) into the next thread's first
// submit, which serialized an empty image block and 400'd the request.
void           reset_composer_draft(ComposerState& c);

// Canonical id of the currently-active provider ("anthropic" for the
// Claude path, else the OpenAI endpoint label — "openai" / "ollama" / …).
// Used to key per-provider model recall in Settings::provider_models.
std::string    active_provider_id();

// Pick the model to make active when switching TO provider `spec`. Prefers
// the model last used on that provider (Settings::provider_models), else a
// sane built-in default for the provider kind. Returns empty when no recall
// exists and the provider has no hardcoded default (the model list refetch
// will then auto-select the first available model).
std::string    model_for_provider(std::string_view spec);

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

// NOTE: the mid-stream carve API that used to be declared here
// (freeze_settled_subturns, freeze_streaming_text_prefix,
// trim_frozen_above_viewport) is DELETED. agent_session — the
// reference implementation with zero scrollback corruption — freezes
// exactly once per turn (MessageStop); the only production analog is
// finalize_turn → pending_settle_freeze → freeze_through. Mid-stream
// carves stamped frozen Turns whose hashes maya's cache had never
// seen, forcing cache-miss re-emits over committed scrollback. Do
// not reintroduce them.

// rehydrate_frozen: rebuild m.ui.frozen from scratch from the current
// thread's messages + compaction records. Used on thread switch /
// thread load — anywhere the messages vector was replaced wholesale.
// Resets frozen_through and frozen_turn.
void rehydrate_frozen(Model& m);

// clear_frozen: drop the entire frozen vector and reset counters.
// For NewThread before a fresh-start submit.
void clear_frozen(Model& m);

// Settle one Assistant message's StreamingMarkdown widget: feed the
// final bytes, finish() (flush tail → prefix, flip live_ off), apply the
// same auto-fold preset cached_markdown_for uses, and stamp the cache
// sizes so the per-frame settled fast-path engages. Defined in stream.cpp.
void settle_message_md(Model& m, const Message& msg);

// live_tail_reveal_settled: true iff EVERY Assistant message in the live
// tail [frozen_through..end) has fully drained its reveal animation — the
// widget flipped live_ off, the typewriter cursor reached the live edge,
// the finalize ramp completed, and no async parse is in flight. At that
// point the live tail painted the SETTLED tree into maya's prev_cells, so
// a freeze taken now is byte-and-hash-identical to what's on screen (cache
// HIT, zero re-emit). The predicate set is the EXACT mirror of
// build_live_tail's `reveal_settled` (is_live || reveal_in_progress ||
// is_finalizing || is_parsing): that gate decides whether the live tail
// STAMPS the cacheable assistant_run_hash_id, this one decides whether the
// freeze fires — they must agree or the freeze stamps a key the live tail
// never painted and freeze_range rebuilds (show_all) at a divergent height.
// Used to GATE the deferred settle-freeze: we never finalize+freeze a turn
// whose reveal is still animating, which is the structural root cause of
// the post-stream duplicate/ghost (freezing a post-finish shape that
// diverges from the still-animating live frame in prev_cells). Returns
// true when the tail has no Assistant md to drain (nothing to wait on).
bool live_tail_reveal_settled(const Model& m);

// ensure_frozen_width is GONE (ledger paint-recording re-stamps every
// sealed block's height at the live width each frame — resize heals
// itself; see maya/render/scrollback_ledger.hpp).

// trim_frozen_if_oversized: when frozen exceeds a soft cap, drop the
// oldest N blocks to keep maya's prev_cells working set bounded.
// Returns Cmd::commit_scrollback(ScrollbackDebt) minted by the ledger
// from maya's own paint-recorded heights. No-op if under the cap.
maya::Cmd<Msg> trim_frozen_if_oversized(Model& m);

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
Step provider_picker_update(Model m, msg::ProviderPickerMsg pm);
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
