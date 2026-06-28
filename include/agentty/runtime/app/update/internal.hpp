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

// Settle one Assistant message's StreamingMarkdown widget: feed the
// final bytes, finish() (flush tail → prefix, flip live_ off), apply the
// same auto-fold preset cached_markdown_for uses, and stamp the cache
// sizes so the per-frame settled fast-path engages. Defined in stream.cpp.
// Still used to lock reveal height before a new submit; maya's Strata
// renderer seals scrolled-off settled runs into native scrollback on its
// own, so there is no host snapshot to build or freeze flag to set.
void settle_message_md(Model& m, const Message& msg);

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

// ── Sealed-run immutability gate ─────────────────────────────────────────
//
// Under the Strata depositional model maya seals settled runs into
// native scrollback itself; once a message lands in a sealed
// (pre-live_run_start) run its rendered rows are owned by maya's
// scrollback and are immutable, so any post-seal mutation of the
// underlying ToolUse would be invisible. The five mutation sites that
// locate a tool by ToolCallId (ToggleToolExpanded, ToolExecOutput /
// apply_tool_output, ToolExecProgress, ToolTimeoutCheck,
// PermissionReject / mark_tool_rejected) must therefore refuse to touch
// any message in a sealed run — i.e. any message with index <
// live_run_start.
//
// `with_live_tool` is the only way to mutate a tool by id. It searches
// ONLY the live tail [live_run_start .. end) and returns true iff the
// mutation ran. A stale id (tool whose run was already sealed) returns
// false — caller treats it as a no-op, matching the existing
// "idempotent on terminal" behaviour of apply_tool_output.
template <class F>
bool with_live_tool(Model& m, const ToolCallId& id, F&& f) {
    for (std::size_t i = m.ui.live_run_start;
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
