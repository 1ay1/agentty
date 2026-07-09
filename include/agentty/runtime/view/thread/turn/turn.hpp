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
[[nodiscard]] maya::Turn::Config turn_config_for_assistant_run(
    std::size_t run_first, std::size_t run_end,
    int turn_num, const Model& m);

// Decide where the current speaker-run ends. For an Assistant head this
// walks forward over consecutive Assistant messages; for User / other roles
// it returns `from + 1`. Used by both the live-tail and frozen builders so
// they slice the message vector identically.
[[nodiscard]] std::size_t turn_run_end(const std::vector<Message>& messages,
                                       std::size_t from);

// Build the maya hash_id (component-cache key) for the Assistant run
// [run_start, run_end). SINGLE SOURCE OF TRUTH for the key shape, used
// by both sites that must agree byte-for-byte so the freeze handoff is
// a cache HIT (zero row shift): conversation.cpp's settled-run live
// tail and freeze_range (the single idle-freeze site). A key-shape
// mismatch between the two produces a cache miss and the duplication
// ghost; centralising the build here means a future edit can't
// silently desync one caller.
[[nodiscard]] maya::CacheId assistant_run_hash_id(
    const Model& m, std::size_t run_start, std::size_t run_end);


} // namespace agentty::ui
