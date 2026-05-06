#pragma once
// View-side render cache.
//
// Keeps mutable UI state out of the pure domain types (Message, ToolUse).
// The domain describes *what* a conversation is; this cache describes
// *what we've already painted for it* so we can skip rebuilding identical
// Elements every frame.
//
//   • Message markdown — finalized assistant messages whose `text` is
//     immutable. Keyed by (thread_id, msg_idx). Streaming messages carry a
//     separate `StreamingMarkdown` that caches block boundaries so each
//     delta costs O(new_chars) instead of O(total).
//
//   • Turn config — the FULL maya::Turn::Config built for a message that
//     has a successor (i.e. is by construction settled — moha only appends
//     a new message once the current turn fully resolves).  Caches the
//     turn header, agent timeline (every tool card config), permissions,
//     etc.  Without this, each frame walks every message in the visible
//     window and rebuilds N tool cards × M turns from scratch.  After
//     ~10 turns with a few tool calls each, frame time grows to where
//     even direct mode feels sluggish; over an SSH-tunnelled airgap the
//     bigger frames also pay per-byte transmission cost.  Reusing the
//     cached Config makes the cost per frame O(active_turn) instead of
//     O(total_turns × tools_per_turn).

#include <cstddef>
#include <memory>

#include <maya/widget/turn.hpp>

#include "moha/domain/id.hpp"

namespace maya {
    struct Element;
    class  StreamingMarkdown;
}

namespace moha::ui {

struct MessageMdCache {
    std::shared_ptr<maya::Element>            finalized;
    std::shared_ptr<maya::StreamingMarkdown>  streaming;
};

struct TurnConfigCache {
    std::shared_ptr<maya::Turn::Config>       cfg;
};

[[nodiscard]] MessageMdCache&  message_md_cache(const ThreadId& tid,
                                                std::size_t msg_idx);
[[nodiscard]] TurnConfigCache& turn_config_cache(const ThreadId& tid,
                                                 std::size_t msg_idx);

} // namespace moha::ui
