#pragma once
#include <cstddef>
#include <span>
#include <string_view>
#include <maya/widget/conversation.hpp>
#include <maya/widget/turn.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

// Build the Turn config for one message — header + typed body slots
// (PlainText / MarkdownText / AgentTimeline / cached streaming-markdown
// Element). Set `continuation = true` when this message is the 2nd+
// assistant in a same-speaker run; the Turn widget then suppresses
// its header (glyph + label + meta) and the Conversation widget skips
// the inter-turn divider, so consecutive assistant messages from one
// agent action visually flow as one block.
//
// `tool_calls_override` lets the tool-batch merge in freeze_range /
// build_live_tail synthesise a single panel from multiple disjoint
// Messages' tool_calls without deep-copying `msg`. When non-empty the
// AgentTimeline scan uses this span instead of `msg.tool_calls`;
// when empty `msg.tool_calls` is used as before.
//
// Permission card is NOT emitted as a Turn body slot — the host
// floats it as a sibling under the live tail (agent_session shape).
[[nodiscard]] maya::Turn::Config turn_config(const Message& msg,
                                             std::size_t msg_idx,
                                             int turn_num,
                                             const Model& m,
                                             bool continuation = false,
                                             std::string_view meta_override = {},
                                             std::span<const ToolUse> tool_calls_override = {});

// Build ONE Turn::Config covering a run of consecutive Assistant messages.
// `run_first` is the index of the head message (Role::Assistant); `run_end`
// is exclusive — every message in [run_first, run_end) must be Assistant.
// The body interleaves each message's text (markdown) and tool batch in
// source order so a `text -> tool -> text -> tool` sequence renders as a
// single visual Turn with N body slots, matching agent_session's shape.
//
// Header (glyph, label, meta) is taken from the head message; meta carries
// the head's timestamp + (optionally) elapsed since the last user message.
//
// `continuation`: when true the run is the LIVE remainder of a turn whose
// completed leading sub-turns were already frozen mid-run
// (freeze_settled_subturns). The header row (glyph/label/meta/turn-number)
// is suppressed so the frozen prefix and this remainder read as one turn
// — only the rail is drawn. Default false (a self-contained run).
[[nodiscard]] maya::Turn::Config turn_config_for_assistant_run(
    std::size_t run_first, std::size_t run_end,
    int turn_num, const Model& m, bool continuation = false);

// Decide where the current speaker-run ends. For an Assistant head this
// walks forward over consecutive Assistant messages; for User / other roles
// it returns `from + 1`. Used by both the live-tail and frozen builders so
// they slice the message vector identically.
[[nodiscard]] std::size_t turn_run_end(const std::vector<Message>& messages,
                                       std::size_t from);

// Compute the freezable-prefix cut for the Assistant run [run_start,
// run_end): the exclusive upper bound of the contiguous leading
// sub-turns that are byte-stable and safe to render as a settled,
// hash-keyed Turn (settled terminal-tool batches and settled text-only
// blocks). The remaining [cut, run_end) is the still-live tail.
//
// SINGLE SOURCE OF TRUTH for the live/frozen split. Both build_live_tail
// (which renders [run_start, cut) as its own keyed Turn so the freeze
// handoff is a pure cache hit, even when the card overflows the
// viewport) and freeze_settled_subturns call this, so the live card and
// the frozen card cover exactly the same messages under exactly the
// same key — zero row shift at the seam.
//
// The last sub-turn is kept live (cut clamped to run_end-1) UNLESS it's
// a settled terminal-TOOL batch that is no longer msgs.back() (a
// continuation message already follows it), in which case the whole
// settled prefix is eligible. A tool-only message that is still the
// mutable back can grow a trailing text block, so it stays live until
// the continuation lands.
[[nodiscard]] std::size_t freezable_prefix_cut(
    const Model& m, std::size_t run_start, std::size_t run_end);

// Build the maya hash_id (component-cache key) for the Assistant run
// [run_start, run_end). SINGLE SOURCE OF TRUTH for the key shape, used
// by all three sites that must agree byte-for-byte so the freeze handoff
// is a cache HIT (zero row shift): conversation.cpp's live-tail prefix
// split, freeze_settled_subturns (the mid-run freeze), and freeze_range
// (the idle freeze). The `continuation` flag MUST be the same value all
// three pass for the same run — a head-vs-cont mismatch produces
// different keys, a cache miss, and the duplication ghost. Centralising
// the build here means a future edit to the key shape can't silently
// desync one caller.
[[nodiscard]] maya::CacheId assistant_run_hash_id(
    const Model& m, std::size_t run_start, std::size_t run_end,
    bool continuation);

// Decide whether the run beginning at message index `i` is the LIVE
// REMAINDER of a turn whose completed leading sub-turns were already
// frozen mid-run — i.e. a CONTINUATION (header suppressed, rail only).
//
// SINGLE SOURCE OF TRUTH for the continuation predicate. The `continuation`
// flag is baked into both the Turn body shape (turn_config_for_assistant_run)
// and the cache key (assistant_run_hash_id), so the live-tail builder and
// the freeze path MUST compute the same value for the same run or the
// freeze handoff becomes a cache miss (the duplication ghost). Both
// build_live_tail and freeze_settled_subturns/freeze_range previously
// hand-mirrored this boolean (`frozen_midrun && i == frozen_through &&
// role==Assistant`) in two files; centralising it here means a future
// edit can't desync one caller.
//
// True iff: the mid-run freeze flag is set, `i` is exactly the live-tail
// boundary (frozen_through), and that message is an Assistant head.
[[nodiscard]] bool is_midrun_continuation(const Model& m, std::size_t i);


} // namespace agentty::ui
