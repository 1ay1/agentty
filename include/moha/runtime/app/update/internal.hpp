#pragma once
// Shared internals for the update/* translation units. Not part of the public
// moha::app interface — external callers go through moha::app::update() in
// update.hpp. Lives under include/ rather than a private src/ header so the
// three update/*.cpp files and update.cpp can all see the same declarations.

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include <maya/maya.hpp>
#include <nlohmann/json.hpp>

#include "moha/runtime/model.hpp"
#include "moha/runtime/msg.hpp"

namespace moha::app {

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

// ── update/modal.cpp helpers ─────────────────────────────────────────────
Step           submit_message(Model m);
maya::Cmd<Msg> maybe_virtualize(Model& m);
void           persist_settings(const Model& m);

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
Step todo_update          (Model m, msg::TodoMsg           tm);
Step login_update         (Model m, msg::LoginMsg          lm);
Step diff_review_update   (Model m, msg::DiffReviewMsg     dm);
Step meta_update          (Model m, msg::MetaMsg           mm);

} // namespace detail
} // namespace moha::app
