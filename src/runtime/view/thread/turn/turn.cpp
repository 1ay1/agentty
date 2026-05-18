#include "agentty/runtime/view/thread/turn/turn.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <maya/widget/agent_timeline.hpp>
#include <maya/widget/markdown.hpp>

#include "agentty/domain/catalog.hpp"
#include "agentty/runtime/composer_attachment.hpp"
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
//
//    Single rendering path: the StreamingMarkdown widget is used for
//    live AND settled messages. The widget's pre-finish output (prefix
//    ComponentElement + tail Element wrapped in vstack.gap(1)) has a
//    slightly different total height than the one-shot maya::markdown()
//    parser's output (flat blocks under the same vstack wrapper but no
//    ComponentElement seam). Swapping between them at StreamFinished
//    shifted the canvas by ~3 rows, which propagated through the per-row
//    diff and left the composer at a different terminal row — visible
//    as "composer pulled down + duplicate composer above it on the
//    first keypress."
//
//    Staying on the streaming widget keeps the height stable across the
//    streaming → idle transition. set_content with byte-identical bytes
//    is an internal no-op; build() returns cached_build_ when nothing
//    has dirtied, so the per-frame cost is the same as the finalized
//    path. The tail re-parses on each frame for finalized messages too,
//    but that's a single inline parse on the last few bytes — cheap.
maya::Element cached_markdown_for(const Message& msg, const Model& m) {
    auto& cache = m.ui.view_cache.message_md(m.d.current.id, msg.id);

    if (!cache.streaming)
        cache.streaming = std::make_shared<maya::StreamingMarkdown>();

    // msg.streaming_text grows during streaming; on StreamFinished
    // its bytes are std::move'd into msg.text. Feed whichever holds
    // the content. Byte-equality between the moved-into msg.text and
    // the widget's accumulated source_ makes set_content's fast-path
    // a no-op, so the transition costs nothing.
    const std::string& source =
        msg.text.empty() ? msg.streaming_text : msg.text;

    // Settled-message fast path. Once a message has settled
    // (msg.text is final, streaming_text empty) the source bytes are
    // immutable for the rest of the session. set_content's equal-
    // content check still costs O(source.size()) memcmp every frame;
    // on a long thread with many visible turns that adds up. Skip
    // the call entirely once we've fed the final bytes through once.
    //
    // Gate is (size + settled-once flag) only. No reducer rewrites
    // an Assistant's msg.text in place after StreamFinished moves
    // streaming_text → text; later edits replace the Message
    // wholesale (new MessageId, new cache slot). Hashing the bytes
    // every frame to guard against a same-length in-place rewrite
    // that doesn't exist cost O(text) per visible settled turn per
    // frame — the dominant per-frame cost on long sessions.
    const bool settled = !msg.text.empty() && msg.streaming_text.empty();
    const bool already_settled_into_cache =
        settled
        && cache.last_settled_size == source.size();
    if (!already_settled_into_cache) {
        cache.streaming->set_content(source);

        // Settled message → commit any trailing tail to the prefix's
        // block list. Necessary because find_block_boundary only commits
        // a fenced code block once its closing ``` is followed by a
        // newline; messages that end at the closing backticks (the
        // common case for Claude responses ending with a code example)
        // leave the last block stuck in the tail forever, rendered via
        // render_tail's inline path instead of the canonical
        // md_block_to_element. The two paths take the same border /
        // padding builder but feed it slightly different code strings
        // (render_tail's extractor vs the parser's stripping rules), so
        // their painted cells aren't byte-identical. Once that turn
        // settles and the renderer's cache_id-keyed cell blit picks up
        // the render_tail output, the layout quirk is locked in until a
        // resize invalidates the cache by width — which is exactly the
        // "code block border at the wrong column" symptom we saw.
        //
        // finish() is idempotent (no-op once committed_ == source_.size()),
        // so calling it every frame for a settled message is cheap.
        if (settled) {
            cache.streaming->finish();
            cache.last_settled_size = source.size();
        }
    }

    return cache.streaming->build();
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
                               bool continuation, bool synthetic,
                               std::string_view meta_override) {
    // agent_session pattern: build a fresh Config every call. Settled
    // turns get their Element snapshotted into m.ui.frozen at freeze
    // time and rendered from there; the live tail rebuilds each frame
    // but is bounded to the in-flight turn. No Config / Element
    // memoization here. `synthetic` is still consulted further down to
    // skip the agent_timeline panel-freeze cache for queued previews.
    (void)msg_idx;
    const std::string& model_id_ref = m.d.model_id.value;

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
    if (!meta_override.empty()) cfg.meta = std::string{meta_override};
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
        return cfg;
    }

    if (msg.role == Role::User) {
        // Substitute chip placeholders (\x01ATT:N\x01) with their
        // human-readable captions so a 400-line paste renders as
        // "[Pasted text · 412 lines · 14 KB]" in the transcript
        // instead of inlining the whole body. The wire still sees
        // the full bytes — the transport calls attachment::expand()
        // at request-build time. Image placeholders consult
        // msg.attachments (which still holds an entry per image with
        // path/media_type/byte_count populated even after the bytes
        // were lifted onto msg.images), so the same chip label
        // formula used in the composer applies here verbatim.
        std::string display;
        if (msg.attachments.empty()) {
            display = msg.text;
        } else {
            display.reserve(msg.text.size());
            std::size_t i = 0;
            while (i < msg.text.size()) {
                if (static_cast<unsigned char>(msg.text[i]) == attachment::kSentinel) {
                    auto len = attachment::placeholder_len_at(msg.text, i);
                    if (len > 0) {
                        auto idx = attachment::placeholder_index(msg.text, i);
                        if (idx < msg.attachments.size()) {
                            display.push_back('[');
                            display.append(attachment::chip_label(msg.attachments[idx]));
                            display.push_back(']');
                        }
                        i += len;
                        continue;
                    }
                }
                display.push_back(msg.text[i++]);
            }
        }
        cfg.body.emplace_back(maya::Turn::PlainText{.content = std::move(display), .color = fg});
    } else if (msg.role == Role::Assistant) {
        const bool has_body = !msg.text.empty() || !msg.streaming_text.empty();
        if (has_body) {
            // Cross-frame StreamingMarkdown cache requires holding the
            // widget instance; feed its built Element via the typed
            // Element variant of BodySlot.
            cfg.body.emplace_back(cached_markdown_for(msg, m));
        }
        if (!msg.tool_calls.empty()) {
            // agent_session-mirroring fast path: once every tool in the
            // batch is terminal AND no pending permission still targets
            // one of them, the panel's bytes are immutable for the rest
            // of this message's life. Snapshot it to an Element on the
            // FIRST such frame and serve the snapshot verbatim on every
            // subsequent frame — even while streaming_text continues to
            // grow below the panel.
            //
            // This matters because Anthropic's common shape is
            // tool_use_block_stop → content_block_delta (more text)
            // → message_stop. During the post-tool text window
            // `streaming_text` is non-empty so the full-turn cache stays
            // cold and `agent_timeline_config` rebuilds every frame:
            // the running-state status flips to terminal status, the
            // footer text mutates, the spinner frame index changes (the
            // border can flip from rail-cyan to muted). Each frame's
            // canvas mapping at a given row Y captures a slightly
            // different snapshot of "the panel right now", and any rows
            // that scroll off into native scrollback during this window
            // commit different bytes than the panel will eventually
            // render — visible as fragments / overlap at the seam.
            //
            // Freezing the panel at first-terminal-frame stops the
            // drift cold: the same Element is reused, so the same
            // bytes get painted, so what commits to scrollback matches
            // what stays in viewport.
            bool all_terminal = true;
            bool any_pending_perm = false;
            for (const auto& tc : msg.tool_calls) {
                if (!tc.is_terminal()) { all_terminal = false; break; }
                if (m.d.pending_permission
                    && m.d.pending_permission->id == tc.id) {
                    any_pending_perm = true;
                    break;
                }
            }
            const bool can_freeze_panel =
                !synthetic && all_terminal && !any_pending_perm;

            // Key the freeze on the SAME hash that the message's
            // render_key derives from its tool_calls. If a post-terminal
            // mutation lands (expand toggle, late re-execute output),
            // the key bumps and we re-snapshot.
            std::uint64_t panel_key = 0;
            if (can_freeze_panel) {
                panel_key = 1469598103934665603ULL;
                auto mix = [&](std::uint64_t v) {
                    panel_key = (panel_key ^ v) * 1099511628211ULL;
                };
                mix(msg.tool_calls.size());
                for (const auto& tc : msg.tool_calls)
                    mix(tc.compute_render_key());
                // style.color is baked into the panel border via
                // agent_timeline_config's rail_color argument; mix it
                // so a model-id switch (rail color follows model
                // family) invalidates the snapshot.
                mix(static_cast<std::uint64_t>(style.color.r()));
                mix(static_cast<std::uint64_t>(style.color.g()));
                mix(static_cast<std::uint64_t>(style.color.b()));
            }

            // Always emplace a pre-built Element into the body slot,
            // never an AgentTimeline::Config. agent_session pushes
            // `actions_panel(m, false)` which is `AgentTimeline{cfg}
            // .build()` — i.e. a built Element value. The Config
            // variant of BodySlot uses a different code path through
            // Turn::render_slot and was observably leaving the Turn
            // header invisible when this was the dominant body slot.
            if (can_freeze_panel) {
                auto& slot = m.ui.view_cache.turn_config(
                    m.d.current.id, msg.id);
                if (!slot.agent_timeline
                    || slot.agent_timeline_key != panel_key
                    || slot.agent_timeline_model_id != model_id_ref) {
                    auto built = maya::AgentTimeline{
                        agent_timeline_config(msg, /*spinner_frame=*/0,
                                              style.color)}.build();
                    slot.agent_timeline =
                        std::make_shared<maya::Element>(std::move(built));
                    slot.agent_timeline_key      = panel_key;
                    slot.agent_timeline_model_id = model_id_ref;
                }
                cfg.body.emplace_back(*slot.agent_timeline);
            } else {
                cfg.body.emplace_back(
                    maya::AgentTimeline{agent_timeline_config(
                        msg, m.s.spinner.frame_index(), style.color)}.build());
            }
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

    return cfg;
}

} // namespace agentty::ui
