#include "agentty/runtime/view/thread/turn/turn.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <maya/widget/markdown.hpp>

#include "agentty/domain/catalog.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/agent_timeline.hpp"
#include "agentty/runtime/view/cache.hpp"
#include "agentty/runtime/view/helpers.hpp"
#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/thread/turn/permission.hpp"

namespace agentty::ui {

namespace {

// ── Cached markdown render. The ONE Element-returning helper kept in
//    agentty — strictly because cross-frame cache state lives in the
//    StreamingMarkdown widget instance, which we keep alive across
//    frames so its block cache survives.
maya::Element cached_markdown_for(const Message& msg, const Model& m) {
    auto& cache = m.ui.view_cache.message_md(m.d.current.id, msg.id);
    if (msg.text.empty()) {
        if (!cache.streaming)
            cache.streaming = std::make_shared<maya::StreamingMarkdown>();
        cache.streaming->set_content(msg.streaming_text);
        return cache.streaming->build();
    }
    if (!cache.finalized) {
        cache.finalized = std::make_shared<maya::Element>(maya::markdown(msg.text));
        cache.streaming.reset();
    }
    return *cache.finalized;
}

// ── Per-speaker visual identity: rail color + glyph + display name.
//    Centralized so the rail color, the header glyph, and the bottom
//    streaming indicator stay in lockstep.
struct SpeakerStyle {
    maya::Color color;
    std::string glyph;
    std::string label;
};

SpeakerStyle speaker_style_for(Role role, const Model& m) {
    if (role == Role::User) {
        // User rail is `role_brand` (magenta) — distinct from code-reference
        // cyan and matching the composer's accent color when has-text, so
        // the user's typed message visually flows into their turn header.
        return {role_brand, "\xe2\x9d\xaf", "You"};                  // ❯
    }
    const auto& id = m.d.model_id.value;
    const auto caps = ModelCapabilities::from_id(id);
    maya::Color c;
    std::string label;
    // Model rails use ROLE colors (persistent identity), never status
    // colors. Opus is the bright-magenta variant so it's visually
    // distinguishable from the user-turn magenta (same hue family,
    // different intensity — flagship gets the brighter shade). Haiku
    // used to render in `success` (green) which collided with the ✓
    // done icon; bright_cyan keeps the "fast/agile" feel without the
    // status collision.
    if      (caps.is_opus())   { c = role_brand_alt; label = "Opus";   } // bright_magenta
    else if (caps.is_sonnet()) { c = role_info;      label = "Sonnet"; } // blue
    else if (caps.is_haiku())  { c = code_path;      label = "Haiku";  } // bright_cyan
    else                       { c = highlight;      label = id;       } // cyan (fallback)
    // Extract a short version run like "4-5" → "4.5" from model ids.
    // Reject segments longer than 2 digits so date suffixes (8-digit
    // YYYYMMDD on ids like "claude-sonnet-4-20250514") don't get
    // displayed as `Sonnet 4.20250514`. Segments are 1–2 digits
    // joined by `-`/`.`; once a 3-digit run appears we stop the
    // version at the boundary before it (so `4-5-20250514` → `4.5`,
    // `4-20250514` → `4` only).
    for (std::size_t i = 0; i + 2 < id.size(); ++i) {
        char ch = id[i];
        if (ch >= '0' && ch <= '9') {
            char delim = id[i + 1];
            if ((delim == '-' || delim == '.') && id[i + 2] >= '0' && id[i + 2] <= '9') {
                std::size_t cursor = i;
                std::string ver;
                // Read 1–2 digits for the first segment.
                {
                    std::size_t start = cursor;
                    while (cursor < id.size() && id[cursor] >= '0' && id[cursor] <= '9'
                           && (cursor - start) < 2)
                        ++cursor;
                    ver.append(id, start, cursor - start);
                    // If more digits follow than 2, this is a date — bail.
                    if (cursor < id.size() && id[cursor] >= '0' && id[cursor] <= '9')
                        goto have_ver;
                }
                // Optional further segments, each 1–2 digits.
                while (cursor + 1 < id.size()
                       && (id[cursor] == '-' || id[cursor] == '.')
                       && id[cursor + 1] >= '0' && id[cursor + 1] <= '9')
                {
                    std::size_t sep_pos = cursor;
                    std::size_t start   = cursor + 1;
                    std::size_t end     = start;
                    while (end < id.size() && id[end] >= '0' && id[end] <= '9'
                           && (end - start) < 2)
                        ++end;
                    // If more than 2 digits in this segment, it's a date —
                    // stop BEFORE the separator so the prior version stands.
                    if (end < id.size() && id[end] >= '0' && id[end] <= '9') break;
                    ver += '.';
                    ver.append(id, start, end - start);
                    cursor = end;
                    (void)sep_pos;
                }
              have_ver:
                if (!ver.empty())
                    label += " " + ver;
                break;
            }
        }
    }
    return {c, "\xe2\x9c\xa6", std::move(label)};                    // ✦
}

// ── Trailing meta strip for the turn header — `12:34 · 4.2s · turn N`.
std::string format_turn_meta(const Message& msg, int turn_num,
                             std::optional<float> elapsed_secs) {
    std::string meta = timestamp_hh_mm(msg.timestamp);
    if (elapsed_secs && *elapsed_secs > 0.0f)
        meta += "  \xc2\xb7  " + format_duration_compact(*elapsed_secs);
    if (turn_num > 0)
        meta += "  \xc2\xb7  turn " + std::to_string(turn_num);
    return meta;
}

// ── Compute the assistant turn's wall-clock elapsed: from previous
//    user message timestamp to this one.
std::optional<float> assistant_elapsed(const Message& msg, const Model& m) {
    if (msg.role != Role::Assistant) return std::nullopt;
    for (std::size_t i = m.d.current.messages.size(); i-- > 0;) {
        if (&m.d.current.messages[i] == &msg) continue;
        if (m.d.current.messages[i].role == Role::User) {
            auto dt = std::chrono::duration<float>(
                msg.timestamp - m.d.current.messages[i].timestamp).count();
            if (dt > 0.0f && dt < 3600.0f) return dt;
            return std::nullopt;
        }
    }
    return std::nullopt;
}

} // namespace

maya::Turn::Config turn_config(const Message& msg, std::size_t msg_idx,
                               int turn_num, const Model& m,
                               bool continuation) {
    // Settled-turn cache.  A message that has a successor in the messages
    // vector is by construction fully resolved — agentty only appends a new
    // message once the current turn's text is final, all tools terminal,
    // and any permission prompt resolved.  Reusing the prior frame's
    // built Config skips per-frame rebuilding of the turn header, the
    // entire agent_timeline (every tool card), and the permission /
    // markdown wiring.
    //
    // Note: this only caches the CONFIG. Even with this cache, a callsite
    // that does `Turn{cfg}.build()` per frame still pays the Element
    // reconstruction cost (every tool card laid out into glyphs, every
    // markdown block re-emitted). For the per-frame fast path, callers
    // should use `turn_element()` below instead — that caches the BUILT
    // Element and skips Turn::build() entirely on settled turns.
    const bool can_cache = (msg_idx + 1 < m.d.current.messages.size());
    if (can_cache) {
        auto& slot = m.ui.view_cache.turn_config(m.d.current.id, msg.id);
        if (slot.cfg && slot.cfg->continuation == continuation) return *slot.cfg;
    }

    auto style = speaker_style_for(msg.role, m);

    maya::Turn::Config cfg;
    cfg.glyph        = style.glyph;
    cfg.label        = style.label;
    cfg.rail_color   = style.color;
    cfg.continuation = continuation;
    cfg.meta         = format_turn_meta(msg, turn_num,
                          msg.role == Role::Assistant
                              ? assistant_elapsed(msg, m)
                              : std::nullopt);
    cfg.checkpoint_above = (msg.role == Role::User && msg.checkpoint_id.has_value());
    cfg.checkpoint_color = warn;

    // Compact-boundary turn: a thin one-line divider only. The
    // summary body itself can be many KB / dozens of rows when the
    // model is verbose, and rendering it inline would (a) push the
    // preserved-tail and any subsequent assistant turn off-screen
    // immediately after compaction lands and (b) tempt the user to
    // read it (it's prose written for the model, not for them). The
    // model still receives the full summary text on the wire because
    // msg.text isn't mutated; the view just elides it. CC does the
    // same — its `compact_boundary` transcript line type renders as
    // chrome, not content (binary near offset 114920224).
    if (msg.is_compact_summary) {
        cfg.glyph      = "\xe2\x89\xa1";              // ≡
        cfg.label      = "Conversation compacted";
        cfg.rail_color = muted;
        // Empty body → the Turn frame collapses to just the header
        // row + bottom rule, ~2 rows total. The user sees a clear
        // divider where the boundary is and nothing more.
        if (can_cache) {
            auto& slot = m.ui.view_cache.turn_config(m.d.current.id, msg.id);
            slot.cfg = std::make_shared<maya::Turn::Config>(cfg);
        }
        return cfg;
    }

    if (msg.role == Role::User) {
        cfg.body.emplace_back(maya::Turn::PlainText{.content = msg.text, .color = fg});
    } else if (msg.role == Role::Assistant) {
        const bool has_body = !msg.text.empty() || !msg.streaming_text.empty();
        if (has_body) {
            // Cross-frame StreamingMarkdown cache requires holding the
            // widget instance; feed its built Element via the typed
            // Element variant of BodySlot.
            cfg.body.emplace_back(cached_markdown_for(msg, m));
        }
        if (!msg.tool_calls.empty()) {
            cfg.body.emplace_back(
                agent_timeline_config(msg, m.s.spinner.frame_index(), style.color));
            // In-flight permission card under the timeline.
            for (const auto& tc : msg.tool_calls) {
                if (m.d.pending_permission && m.d.pending_permission->id == tc.id) {
                    cfg.body.emplace_back(inline_permission_config(
                        *m.d.pending_permission, tc));
                }
            }
        }
        if (msg.error) cfg.error = *msg.error;
    }

    if (can_cache) {
        auto& slot = m.ui.view_cache.turn_config(m.d.current.id, msg.id);
        slot.cfg = std::make_shared<maya::Turn::Config>(cfg);
    }
    return cfg;
}

maya::Conversation::PreBuilt turn_element(const Message& msg,
                                          std::size_t msg_idx,
                                          int turn_num, const Model& m,
                                          bool continuation) {
    // Settled-turn fast path: serve the BUILT Element from cache so a
    // long session doesn't re-run Turn::build() for every visible turn
    // every frame. The build itself laid out the agent_timeline + every
    // tool card + markdown body + permission rows into the inline-frame
    // glyph stream — that's the dominant cost on a long thread, NOT
    // building the Config. Settled (`msg_idx + 1 < total`) means agentty
    // has appended a successor message, which by construction means the
    // turn fully resolved (text final, all tools terminal, any
    // permission prompt closed). The Element is therefore safe to
    // memoize for the lifetime of the cache entry.
    //
    // The cached Element is held via shared_ptr. We hand the
    // shared_ptr straight to maya — Element has an implicit
    // converting constructor from shared_ptr<const Element> that
    // keeps the renderer's cross-frame work bounded automatically.
    // No cache identity strings, no helper wrappers; the host just
    // hands maya what it already has.
    const bool can_cache = (msg_idx + 1 < m.d.current.messages.size());
    if (can_cache) {
        auto& slot = m.ui.view_cache.turn_config(m.d.current.id, msg.id);
        if (slot.element && slot.element_continuation == continuation) {
            return {slot.element, continuation};
        }
    }
    // Miss (or live turn): build Config (this hits the Config cache for
    // settled turns regardless), then run Turn::build() and stash.
    auto cfg = turn_config(msg, msg_idx, turn_num, m, continuation);
    auto built = maya::Turn{std::move(cfg)}.build();
    if (can_cache) {
        auto& slot = m.ui.view_cache.turn_config(m.d.current.id, msg.id);
        slot.element = std::make_shared<maya::Element>(std::move(built));
        slot.element_continuation = continuation;
        return {slot.element, continuation};
    }
    return {std::move(built), continuation};
}

} // namespace agentty::ui
