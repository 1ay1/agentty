#include "agentty/runtime/view/thread/turn/turn.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <maya/widget/agent_timeline.hpp>
#include <maya/widget/markdown.hpp>
#include <maya/app/app.hpp>

#include "agentty/domain/catalog.hpp"
#include "agentty/runtime/composer_attachment.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/agent_timeline.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_args.hpp"
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

    if (!cache.streaming) {
        cache.streaming = std::make_shared<maya::StreamingMarkdown>();
        // Animated live tail: gradient trail + scramble→resolve + pulsing
        // caret on the streaming edge (maya reveal_fx). Only animates while
        // the widget is live_; the settled build is untouched.
        cache.streaming->set_reveal_fx(true);
    }

    // Pick the source bytes for THIS frame:
    //   • settled: msg.text holds the final body, streaming_text empty.
    //   • mid-sub-turn-2: msg.text holds the PRIOR sub-turn's settled
    //     body and streaming_text holds the in-flight follow-up bytes.
    //     We need to feed BOTH so the live tail keeps growing — picking
    //     msg.text alone would freeze rendering at the prior sub-turn's
    //     size until finalize_turn appended the new bytes in one jump.
    //   • sub-turn-1 streaming: msg.text empty, streaming_text grows.
    // The joined buffer is cached on MessageMdCache so the string_view
    // we hand to set_content_async stays valid across the call.
    const std::string* source_ptr = &msg.text;
    if (!msg.streaming_text.empty()) {
        if (msg.text.empty()) {
            source_ptr = &msg.streaming_text;
        } else {
            cache.combined_source.clear();
            cache.combined_source.reserve(
                msg.text.size() + msg.streaming_text.size());
            cache.combined_source.append(msg.text);
            cache.combined_source.append(msg.streaming_text);
            source_ptr = &cache.combined_source;
        }
    }
    const std::string& source = *source_ptr;

    const bool settled = !msg.text.empty() && msg.streaming_text.empty();
    if (settled && !cache.combined_source.empty()) {
        // Reclaim the scratch buffer the moment streaming_text
        // drains — next freeze takes the snapshot off msg.text.
        std::string{}.swap(cache.combined_source);
    }

    // Fast-path: fully settled message whose reveal has completed.
    // Skip all per-frame work (typewriter advance, set_content,
    // finish, live-mode checks) and return the cached element tree
    // directly. This is the dominant case on long sessions — N
    // visible turns × every frame would otherwise pay constant
    // overhead for each one.
    if (settled
        && cache.last_settled_size == source.size()
        && cache.revealed_size    == source.size()) {
        return cache.streaming->build();
    }

    // ── Typewriter reveal ──
    //
    // While streaming, advance a reveal cursor at a fixed character
    // rate so chunky model output unfolds smoothly. The cursor only
    // reveals bytes that have actually arrived (capped at source.size())
    // so it idles when the model is slower than the reveal rate, and
    // never gets ahead of the model. On settle we snap to full size
    // so the final state matches the source exactly.
    //
    // Backlog policy: if the model is more than kBacklogSnap bytes
    // ahead of the cursor (the model just dumped a huge chunk), snap
    // the cursor forward to within kBacklogSnap of source.size() so
    // the reveal doesn't lag forever on a multi-KB paste. The reveal
    // is meant to be a smoothing filter, not a hard rate limit.
    constexpr double kRevealCharsPerSec = 220.0;   // ~ChatGPT cadence
    constexpr std::size_t kBacklogSnap   = 200;    // max bytes behind

    const auto now = std::chrono::steady_clock::now();
    if (settled) {
        // Stream finished — snap reveal to full size immediately.
        // The freeze path (stream.cpp's freeze_through on idle)
        // snapshots this message into m.ui.frozen and stops calling
        // cached_markdown_for for it; if revealed_size were still
        // catching up at that moment the snapshot would freeze
        // partial text + scramble glyphs forever. Smooth reveal is
        // a streaming-only effect.
        cache.revealed_size = source.size();
        cache.last_reveal_tick = now;
    } else if (source.empty()) {
        cache.revealed_size = 0;
        cache.last_reveal_tick = now;
    } else {
        if (cache.last_reveal_tick.time_since_epoch().count() == 0) {
            cache.last_reveal_tick = now;
        }
        // Advance by elapsed time × rate (rounded down).
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(
                now - cache.last_reveal_tick).count();
        const double new_chars =
            (static_cast<double>(elapsed) / 1'000'000.0)
            * kRevealCharsPerSec;
        std::size_t advance = static_cast<std::size_t>(new_chars);
        if (advance > 0) {
            cache.revealed_size += advance;
            // Move tick forward by the bytes we actually consumed to
            // avoid floating-point drift (don't reset to `now`, which
            // would silently drop sub-character fractions every frame).
            const auto consumed_us = static_cast<long long>(
                (static_cast<double>(advance) / kRevealCharsPerSec)
                * 1'000'000.0);
            cache.last_reveal_tick +=
                std::chrono::microseconds(consumed_us);
        }
        // Cap to source size (never reveal bytes that don't exist).
        if (cache.revealed_size > source.size())
            cache.revealed_size = source.size();
        // Snap forward if the backlog is huge — keep the reveal
        // bounded behind the model so a sudden chunk doesn't take
        // seconds to play out. Only applies while the model is still
        // streaming; once settled, let the cursor finish naturally
        // even if the final chunk was large.
        if (!settled && source.size() > kBacklogSnap &&
            cache.revealed_size + kBacklogSnap < source.size()) {
            cache.revealed_size = source.size() - kBacklogSnap;
            cache.last_reveal_tick = now;
        }
        // Round DOWN to a UTF-8 codepoint boundary so we never feed
        // a half-multibyte sequence to the markdown parser.
        while (cache.revealed_size > 0 &&
               cache.revealed_size < source.size() &&
               (static_cast<unsigned char>(source[cache.revealed_size])
                & 0xC0) == 0x80) {
            --cache.revealed_size;
        }
    }

    // What we actually feed the widget this frame: the revealed prefix.
    const std::string_view feed_source =
        std::string_view{source}.substr(0, cache.revealed_size);

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
    const bool already_settled_into_cache =
        settled
        && cache.last_settled_size == source.size()
        && cache.revealed_size == source.size();
    if (!already_settled_into_cache) {
        // Use the async variant: tiny appends stay on the sync
        // incremental path inside StreamingMarkdown (cheap), but a
        // diverging-prefix swap of >=16 KB (loading an old thread,
        // recovering scrollback, pasting a long markdown body)
        // gets offloaded to a worker so the render thread doesn't
        // stall on the parse. While the worker is in flight the
        // widget keeps returning its previous element tree, so
        // there's no visible blank during the handoff.
        cache.streaming->set_content_async(feed_source);

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
        if (settled && cache.revealed_size == source.size()) {
            cache.streaming->finish();
            cache.last_settled_size = source.size();

            // Auto-fold code blocks longer than ~40 lines so a wall
            // of code in a long conversation doesn't push every
            // other turn off-screen. The user can still unfold any
            // block; auto_fold_long_blocks respects an explicit
            // unfold (entry stored as `false`) and won't re-fold
            // on subsequent calls. Threshold deliberately generous
            // so short snippets keep their natural inline rendering.
            constexpr std::uint16_t kFoldLineThreshold = 40;
            constexpr std::uint32_t kFoldKinds =
                (1u << static_cast<unsigned>(maya::StreamingMarkdown::BlockKind::CodeBlock));
            cache.streaming->auto_fold_long_blocks(kFoldLineThreshold, kFoldKinds);
        }
    }

    // Live mode: while streaming OR while the reveal cursor is still
    // catching up to source.size(). The second condition keeps the
    // animation alive for a fraction of a second after StreamFinished
    // when bytes haven't fully unfolded yet (rare but visible at the
    // end of short replies). The widget's animation frame request
    // drives the per-frame advance of cache.revealed_size above.
    const bool reveal_complete = cache.revealed_size >= source.size();
    cache.streaming->set_live(!settled || !reveal_complete);
    if (!reveal_complete) ::maya::request_animation_frame();

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

// Append one Assistant Message's body slots (markdown + tools panel +
// inline permission) to `cfg.body`. Pulled out of turn_config so a run
// of consecutive Assistant Messages can be rendered as ONE Turn (see
// turn_config_for_assistant_run), matching agent_session's discipline
// where every internal seam contributes a body slot to one Turn.
//
// Tool panel emission is split from text emission so a run of
// consecutive sub-turn Messages (each with one tool, no text) renders
// as ONE merged panel rather than N stacked single-tool panels. The
// panel cache key is the `anchor_msg_id` — stable across rebuilds
// because messages are append-only, and unique per merged group
// because each group is anchored at the FIRST contributing Message.
// Build the actions panel fresh every frame (agent_session pattern).
// Settled assistant runs get snapshotted into m.ui.frozen by
// freeze_range — they never re-enter this function. Live panels are
// bounded by the in-flight turn's tool count, so per-frame cost is
// O(active_tools). One AgentTimeline carrier per panel, born here,
// dropped when cfg.body goes out of scope: no shared_ptr identity
// flip races, no spinner-bucket cache, no freeze fast path. The old
// freeze cache was dead weight — the Element it built is snapshotted
// straight into m.ui.frozen the same frame the slot is populated.
//
// Permission card is NO LONGER appended here. The host floats it as
// its own live_tail entry below the active assistant Turn (mirrors
// agent_session's `Permission` sibling under the root vstack).
void append_assistant_tool_panel(maya::Turn::Config& cfg,
                                 std::span<const ToolUse> tool_calls,
                                 const Model& m,
                                 const SpeakerStyle& style)
{
    if (tool_calls.empty()) return;
    const int frame = m.s.spinner.frame_index();
    cfg.body.emplace_back(maya::AgentTimeline{
        agent_timeline_config(tool_calls, frame, style.color)}.build());
}

// Single-message body slot append: text (if any) then this message's
// tool panel (if any). Used by `turn_config` for non-run-merged
// renders (User turns delegate to a different branch; this is hit by
// the single-Message Assistant path that pre-dates the run merge).
void append_assistant_body_slots(maya::Turn::Config& cfg,
                                 const Message& msg,
                                 std::span<const ToolUse> tool_calls,
                                 const Model& m,
                                 const SpeakerStyle& style)
{
    const bool has_body = !msg.text.empty() || !msg.streaming_text.empty();
    if (has_body) {
        cfg.body.emplace_back(cached_markdown_for(msg, m));
    }
    append_assistant_tool_panel(cfg, tool_calls, m, style);
}

} // namespace

maya::Turn::Config turn_config(const Message& msg, std::size_t msg_idx,
                               int turn_num, const Model& m,
                               bool continuation,
                               std::string_view meta_override,
                               std::span<const ToolUse> tool_calls_override) {
    // agent_session pattern: build a fresh Config every call. Settled
    // turns get their Element snapshotted into m.ui.frozen at freeze
    // time and rendered from there; the live tail rebuilds each frame
    // but is bounded to the in-flight turn. No Config / Element
    // memoization here.
    (void)msg_idx;

    // Tool-batch merge plumbing: when the caller passes an override
    // span, treat it as the effective tool_calls for this turn. Saves
    // an O(N) deep-copy of `msg` every frame on the live tail's merged
    // path — the originals stay in `m.d.current.messages[*]` and we
    // borrow them through the span.
    std::span<const ToolUse> tool_calls = tool_calls_override.empty()
        ? std::span<const ToolUse>{msg.tool_calls}
        : tool_calls_override;

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
        append_assistant_body_slots(cfg, msg, tool_calls, m, style);
        if (msg.error) cfg.error = *msg.error;
    }

    return cfg;
}

maya::Turn::Config turn_config_for_assistant_run(
    std::size_t run_first, std::size_t run_end,
    int turn_num, const Model& m, bool continuation)
{
    const auto& msgs = m.d.current.messages;
    // Pre-conditions defended at the only two call sites (build_live_tail
    // / freeze_range), but guard anyway so this function can be reused
    // without subtle row-shape corruption if the range ever turns out empty.
    if (run_first >= run_end || run_first >= msgs.size())
        return {};
    const std::size_t end = std::min(run_end, msgs.size());

    const Message& head = msgs[run_first];
    auto style = speaker_style_for(head.role, m);

    maya::Turn::Config cfg;
    cfg.glyph        = style.glyph;
    cfg.label        = style.label;
    cfg.rail_color   = style.color;
    // A self-contained run is one logical Turn; a continuation run is the
    // live remainder of a turn whose prefix was frozen mid-run — its
    // header is suppressed (maya draws the rail only) so the two pieces
    // read as one turn with no repeated glyph/label/meta.
    cfg.continuation = continuation;
    cfg.meta         = continuation ? std::string{}
                     : format_turn_meta(head, turn_num,
                          head.role == Role::Assistant
                              ? assistant_elapsed(head, m)
                              : std::nullopt);

    if (head.role != Role::Assistant) {
        // Defensive: only Assistant runs use the multi-message path.
        // For a User head this collapses to the single-message build.
        return turn_config(head, run_first, turn_num, m,
                           /*continuation=*/false,
                           /*meta_override=*/{},
                           /*tool_calls_override=*/{});
    }

    // Walk the run, emitting one tool panel per Message that carries
    // tools. agent_session's discipline: one tool batch = one panel,
    // flushed when the next text block starts OR the run ends. In
    // agentty terms a "batch" is the tools attached to a single
    // Anthropic Message; the post-tool placeholder model gives us
    // one Message per sub-turn already, so each Message's tool_calls
    // are conceptually one batch (the model's reply text for that
    // sub-turn either precedes them in the same Message or arrives
    // on the following Message).
    //
    // Order in the rendered body mirrors wire order: text(i)
    // → panel(i) → text(i+1) → panel(i+1) …  Sub-turns whose only
    // contribution is a tool batch (no text) emit just a panel.
    // No cross-Message merging: every sub-turn gets its own panel,
    // matching agent_session where each ev::ToolEnd batch becomes
    // its own actions_panel(...).
    std::string error_accum;

    for (std::size_t i = run_first; i < end; ++i) {
        const Message& m_i = msgs[i];
        if (m_i.role != Role::Assistant) break;   // run boundary

        const bool has_text = !m_i.text.empty() || !m_i.streaming_text.empty();
        if (has_text) {
            cfg.body.emplace_back(cached_markdown_for(m_i, m));
        }
        if (!m_i.tool_calls.empty()) {
            append_assistant_tool_panel(
                cfg,
                std::span<const ToolUse>{m_i.tool_calls},
                m, style);
        }
        if (m_i.error && error_accum.empty()) error_accum = *m_i.error;
    }

    if (!error_accum.empty()) cfg.error = std::move(error_accum);

    return cfg;
}

std::size_t turn_run_end(const std::vector<Message>& messages,
                         std::size_t from)
{
    if (from >= messages.size()) return from;
    if (messages[from].role != Role::Assistant) return from + 1;
    std::size_t end = from + 1;
    while (end < messages.size()
           && messages[end].role == Role::Assistant) {
        ++end;
    }
    return end;
}

} // namespace agentty::ui
