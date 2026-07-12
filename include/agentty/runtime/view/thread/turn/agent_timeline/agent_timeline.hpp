#pragma once
#include <cstdint>
#include <span>
#include <string_view>
#include <maya/widget/agent_timeline.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

// Build the assistant turn's "Actions" panel config. Aggregates state
// (total/done/elapsed/category counts), picks per-category colors,
// computes title/footer, walks tool_calls into events.
//
// Takes the tool_calls by borrowed span so the tool-batch merge in
// turn_config / freeze_range / build_live_tail can synthesise a panel
// from multiple disjoint Messages' tool_calls without deep-copying a
// Message every frame.
[[nodiscard]] maya::AgentTimeline::Config agent_timeline_config(
    std::span<const ToolUse> tool_calls,
    int spinner_frame,
    maya::Color rail_color);

// Build the panel as a ready-to-emit Element. For an ALL-TERMINAL batch
// (every tool Done/Failed/Rejected) the batch bytes are immutable, so the
// fully-built Element is memoized under a content-address of the batch
// (each tool's id + status + output size + render key, plus the grep-hits
// signature + rail color). On hit it returns a copy of the cached Element
// — skipping the entire agent_timeline_config rebuild (grep scan, detail
// strings, footer, per-event CacheIdBuilder) AND the maya layout walk.
//
// This is the fix for the long-in-flight-turn lag: an edit/write-heavy
// autopilot turn accumulates MANY already-settled tool panels in the live
// tail; the run carries no hash_id while the tail still streams, so the
// host previously rebuilt EVERY settled panel every frame — O(Σ batch
// sizes) per frame. A non-terminal batch (a tool still running/pending,
// or an animated spinner) is built fresh every frame as before.
//
// Emitted bytes are byte-identical to a fresh build, so the freeze handoff
// (freeze_range stamps the same assistant_run_hash_id) stays a pure maya
// cache hit — no scrollback-corruption surface.
[[nodiscard]] maya::Element agent_timeline_element(
    std::span<const ToolUse> tool_calls,
    int spinner_frame,
    maya::Color rail_color);

// Memoized variant. Same result as agent_timeline_element, but keyed
// FIRST on the sub-turn's stable MessageId + render_key (a single
// uint64 compare) via a dedicated per-message panel memo. On a settled
// sub-turn this skips the O(tools) content-key string build that even a
// g_panel_cache hit would otherwise pay, so a long in-flight run whose
// settled panels are re-emitted every frame stays flat with turn depth.
// `msg_id` is Message::id.value; `render_key` is
// Message::compute_render_key(). Falls through to the content-addressed
// path (g_panel_cache) on a memo miss.
[[nodiscard]] maya::Element agent_timeline_element_memoized(
    std::string_view msg_id,
    std::uint64_t render_key,
    std::span<const ToolUse> tool_calls,
    int spinner_frame,
    maya::Color rail_color);

} // namespace agentty::ui
