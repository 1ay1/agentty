// frozen.cpp — append-only scrollback prefix.
//
// Mirrors the agent_session example's `m.frozen` discipline: settled
// turns are built into Element values ONCE at the moment they settle,
// pushed into m.ui.frozen, and rendered by maya via list_ref. The view
// (conversation_config) hands maya a borrowed pointer to this vector,
// so the per-frame cost is O(visible_live) regardless of how long the
// session runs — instead of O(visible_total_turns × tool_cards_per_turn).
//
// The producer is `freeze_through(m, live_start)`: walks
// messages[frozen_through .. live_start), applies the same tool-batch
// merge that conversation_config used to apply at view time, and pushes
// one Turn Element (preceded by a gap) per visual unit. Compaction
// dividers are inserted at their boundary indices.
//
// Lifecycle invariants:
//   • frozen.size() corresponds to messages[0 .. frozen_through).
//   • frozen entries are immutable once pushed (Element values are
//     read-only after construction; the underlying shared_ptr Element
//     caches inside view_cache may be evicted, but the snapshot copy
//     here keeps the rendered subtree alive).
//   • Any operation that mutates messages[i] for i < frozen_through is
//     forbidden; if such a mutation becomes necessary (checkpoint
//     restore, retroactive edit), call rehydrate_frozen() to rebuild
//     from scratch.

#include "agentty/runtime/app/update/internal.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

#include <maya/dsl.hpp>
#include <maya/platform/io.hpp>
#include <maya/render/cache_id.hpp>
#include <maya/widget/conversation.hpp>
#include <maya/widget/turn.hpp>

#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/thread/turn/turn.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_body_preview.hpp"

namespace agentty::app::detail {

namespace {

// Single source of truth for the live terminal geometry. ONE TIOCGWINSZ
// ioctl returns both axes; every row-math / trim caller reads through
// this so width and height can never disagree across a frame. The
// row estimate (estimate_msg_rows) divides byte counts by `cols`, and
// the trims size their keep-margins off `rows` — both must reflect the
// SAME terminal state the renderer laid the canvas out against, or the
// "provably above the viewport" trim proof drifts (under-counted rows
// → drop an on-screen entry → re-emit committed scrollback shifted =
// the duplication ghost). Falls back to a sane 80x40 when the ioctl
// fails (piped output, detached tty).
struct TermDims { int cols; int rows; };
TermDims term_dims() {
    const auto sz = maya::platform::query_terminal_size(
        maya::platform::stdout_handle());
    return TermDims{
        sz.width.value  > 0 ? sz.width.value  : 80,
        sz.height.value > 0 ? sz.height.value : 40,
    };
}

// Effective wrap width used by the row estimate. Clamped to a sane
// floor so a transiently-bogus 1-col read can't blow the estimate up
// to thousands of rows (which would over-trim). The renderer reserves
// a couple of columns for the turn rail / gutter, so subtract a small
// margin to bias the estimate toward OVER-counting rows (keep more) —
// the safe direction for the trim's correctness.
int estimate_wrap_cols() {
    const int cols = term_dims().cols;
    // Rail + padding eat ~4 columns of real content width.
    return std::max(16, cols - 4);
}

// Live-canvas row budget = a small multiple of the terminal viewport.
// The live m.ui.frozen vector IS the inline canvas; every full repaint
// (resume swap, resize→Divergent wipe+repaint, Ctrl-L) walks it top to
// bottom and the user sees the paint. Bounding it to ~2 screens keeps
// all three cheap. Older rows live in native terminal scrollback (the
// terminal redraws them instantly) and on disk (recall via picker).
std::size_t frozen_row_budget() {
    const int h = term_dims().rows;
    // ~2 viewports, floored so a tiny window still keeps useful context.
    return static_cast<std::size_t>(std::max(48, h * 2));
}

// Thin dim ─ rule between turns. Pushed before each fresh-speaker
// turn so settled turns are visually separated.
maya::Element gap_row() {
    return maya::Conversation::divider();
}

// Compaction-boundary divider Element. Single-line `≡ Conversation
// compacted` rule, identical chrome to the inline-built version that
// conversation_config used to manufacture each frame.
maya::Element compaction_divider_row() {
    maya::Turn::Config cfg;
    cfg.glyph      = "\xe2\x89\xa1";   // ≡
    cfg.label      = "Conversation compacted";
    cfg.rail_color = ui::muted;
    return maya::Turn{std::move(cfg)}.build();
}

// Sentinel-check: is `mm` an Assistant message whose only content is
// tool_calls (no prose)? Retained as documentation of the
// classification; the actual run-merge policy now lives in
// `ui::turn_run_end`, called from both `freeze_range` and
// `build_live_tail`.
[[maybe_unused]] bool is_tool_only_assistant(const Message& mm) {
    return mm.role == Role::Assistant
        && mm.text.empty()
        && mm.streaming_text.empty()
        && !mm.tool_calls.empty();
}

// Cheap byte-based row estimate for a single message's contribution
// to a frozen Turn. NOT a render — a coarse proxy used only to BOUND
// the frozen canvas height, where over/under by a few rows is harmless.
// Shared by rehydrate_frozen (budget walk) and freeze_range (per-entry
// frozen_rows accounting).

// Wrapped-row count for a text body at the given content width: each
// hard newline starts a fresh row, and a line longer than `cols`
// soft-wraps to ceil(width/cols) rows. This is what the renderer
// actually does, so the estimate tracks the real canvas height instead
// of assuming a fixed 60-col line. Byte length (not display columns) is
// the wrap unit — a deliberate over-count for multibyte text, which is
// the SAFE direction (keeps slightly more frozen than strictly needed).
std::size_t wrapped_rows(std::string_view body, int cols) {
    if (cols < 1) cols = 1;
    const std::size_t w = static_cast<std::size_t>(cols);
    std::size_t rows = 0;
    std::size_t line_start = 0;
    while (true) {
        const std::size_t nl = body.find('\n', line_start);
        const std::size_t line_end =
            (nl == std::string_view::npos) ? body.size() : nl;
        const std::size_t len = line_end - line_start;
        rows += (len == 0) ? 1 : (len + w - 1) / w;   // ceil, blank = 1 row
        if (nl == std::string_view::npos) break;
        line_start = nl + 1;
    }
    return rows == 0 ? 1 : rows;
}

// Wrapped-row count contributed by every STRING value in a JSON args
// tree. A tool card renders its big payload (write `content`, edit
// `edits[].new_text`/`old_text`, etc.) one row per source line, so the
// real rendered height tracks newlines in those strings — NOT the JSON
// byte length. Summing per-string wrapped_rows captures multi-line
// payloads accurately and biases toward OVER-counting (the safe
// direction for the trim's "provably above the viewport" proof).
// Non-string scalars contribute nothing (they render inline in the
// header, not the body). Cheap: no allocation, no dump().
std::size_t estimate_json_string_rows(const nlohmann::json& j, int cols) {
    switch (j.type()) {
        case nlohmann::json::value_t::string:
            return wrapped_rows(j.get_ref<const std::string&>(), cols);
        case nlohmann::json::value_t::array: {
            std::size_t n = 0;
            for (const auto& e : j) n += estimate_json_string_rows(e, cols);
            return n;
        }
        case nlohmann::json::value_t::object: {
            std::size_t n = 0;
            for (const auto& [k, v] : j.items())
                n += estimate_json_string_rows(v, cols);
            return n;
        }
        default:
            return 0;
    }
}

std::size_t estimate_msg_rows(const Message& mm) {
    const int cols = estimate_wrap_cols();

    // Prose body: count real wrapped rows (newline-aware), not bytes/60.
    std::size_t rows = 0;
    if (!mm.text.empty())           rows += wrapped_rows(mm.text, cols);
    if (!mm.streaming_text.empty()) rows += wrapped_rows(mm.streaming_text, cols);

    for (const auto& tc : mm.tool_calls) {
        // The RENDERED body of a settled tool card is one ROW PER SOURCE
        // LINE (line-numbered write, per-hunk edit diff, read/grep
        // output), not bytes/width. The old `body_bytes / w` estimate
        // catastrophically UNDER-counted any body with many short lines:
        // a 300-line write of ~20-char lines is ~300 rendered rows but
        // bytes/width put it at ~79. That under-count is the root of the
        // scrollback duplication — trim_frozen_above_viewport's "provably
        // above the viewport" proof reads these row counts, and an
        // under-counted (still on-screen) entry gets dropped, re-emitting
        // committed scrollback rows shifted = the turn appears twice.
        // Count newlines in the rendered text sources instead; wrapping
        // only ADDS rows, so newline-count is a safe LOWER bound that's
        // far closer to reality, and we keep the byte term as an extra
        // over-count cushion for wrapped long lines.
        std::size_t tool_rows = 0;
        if (!tc.output().empty())
            tool_rows += wrapped_rows(tc.output(), cols);
        if (!tc.args_streaming.empty())
            tool_rows += wrapped_rows(tc.args_streaming, cols);
        // Args JSON: the big body is a string field (write `content`,
        // edit `edits[].new_text`/`old_text`). Count newlines across all
        // string values so a multi-line payload counts its real lines.
        if (!tc.args.is_null())
            tool_rows += estimate_json_string_rows(tc.args, cols);
        rows += tool_rows;
        // Header / footer / chrome rows per tool card (~4 rows even
        // for an empty body — title, divider, status, blank). Fixed
        // row count, width-independent.
        rows += 4;
    }
    // Per-message envelope (header, gap, divider) — a few fixed rows.
    rows += 3;
    return rows;
}

// Estimated rows for the run messages[from..to) that collapse into
// ONE frozen Turn entry.
int estimate_run_rows(const Model& m, std::size_t from, std::size_t to) {
    std::size_t rows = 0;
    for (std::size_t k = from; k < to && k < m.d.current.messages.size(); ++k)
        rows += estimate_msg_rows(m.d.current.messages[k]);
    return static_cast<int>(rows);
}

// Push a built frozen Element together with its estimated row count,
// keeping m.ui.frozen / m.ui.frozen_rows / m.ui.frozen_row_total in
// lockstep. EVERY push into m.ui.frozen must go through here so the
// row accounting never drifts from the element vector.
void push_frozen(Model& m, maya::Element e, int rows) {
    if (rows < 1) rows = 1;
    m.ui.frozen.push_back(std::move(e));
    m.ui.frozen_rows.push_back(rows);
    m.ui.frozen_row_total += static_cast<std::size_t>(rows);
}

// Run-level safety gate: a frozen turn captures an Element snapshot
// whose hash_id is stamped once and never recomputed. If we freeze a
// run that still contains a Pending / Approved / Running tool, that
// tool's status would mutate later (when ToolExecOutput finally
// lands) but the rendered Element in m.ui.frozen would keep the
// pre-mutation state forever — visible as a permanently-Running
// spinner in scrollback. Refuse to freeze any run that isn't fully
// terminal; the next freeze_through pass picks it up once the live
// path has settled it.
bool run_is_freezable(const Model& m, std::size_t from, std::size_t run_end) {
    for (std::size_t j = from; j < run_end; ++j) {
        for (const auto& tc : m.d.current.messages[j].tool_calls) {
            if (!tc.is_terminal()) return false;
        }
    }
    return true;
}

// Freeze messages[from .. to), pushing built Turn Elements (and any
// leading gap / compaction divider) into m.ui.frozen. One Turn per
// speaker-run: a User message is its own Turn; a run of consecutive
// Assistant messages collapses into ONE Turn whose body interleaves
// each sub-turn's text + tool batch (see
// `ui::turn_config_for_assistant_run`). The same `ui::turn_run_end`
// helper drives the boundary in `build_live_tail` so the frozen and
// live row shapes are identical for the same input.
//
// Advances `m.ui.frozen_turn` once per Assistant run (one logical
// agent turn equals one display number) so the running turn count
// the live tail will compute next stays in sync.
void freeze_range(Model& m, std::size_t from, std::size_t to) {
    const std::size_t total = m.d.current.messages.size();
    if (from >= to || to > total) return;
    // Frozen Elements are painted once then blitted forever, so tool
    // bodies render with full content (show_all) here — unlike the live
    // tail, which elides to a window for per-frame cheapness.
    ui::FrozenBuildScope frozen_scope;

    auto needs_compaction_divider = [&](std::size_t i) {
        for (const auto& rec : m.d.current.compactions) {
            if (rec.up_to_index == i && rec.up_to_index > 0
                && rec.up_to_index <= total) return true;
        }
        return false;
    };

    std::size_t i = from;
    while (i < to) {
        // Run boundary — shared with build_live_tail.
        const std::size_t run_end_global =
            ui::turn_run_end(m.d.current.messages, i);
        const std::size_t run_end = std::min(run_end_global, to);

        // Safety gate: if any tool in this run is not yet terminal,
        // stop here. frozen_through advances to `i` (the start of
        // the un-freezable run) so the next freeze_through call
        // resumes here once the live path has settled it.
        if (!run_is_freezable(m, i, run_end)) {
            m.ui.frozen_through = i;
            return;
        }

        if (needs_compaction_divider(i)) {
            push_frozen(m, compaction_divider_row(), 1);
        }

        // Leading gap: one blank row before every turn except the
        // very first frozen row (avoid a top-of-thread gap). Suppressed
        // when this run is the completion of a mid-run freeze — its
        // header (and the gap above it) were already pushed by
        // freeze_settled_subturns; this entry is a continuation.
        const bool first_overall = m.ui.frozen.empty();
        const bool completing_midrun =
            m.ui.frozen_midrun && i == m.ui.frozen_through
            && m.d.current.messages[i].role == Role::Assistant;
        if (!first_overall && !completing_midrun) {
            push_frozen(m, gap_row(), 1);
        }

        const Message& head = m.d.current.messages[i];

        if (head.role == Role::Assistant) {
            int turn_num = m.ui.frozen_turn + 1;
            auto cfg = ui::turn_config_for_assistant_run(
                i, run_end, turn_num, m, /*continuation=*/completing_midrun);

            // Hash key: settled assistant run. The merge inputs (every
            // run-member msg.id + the run length) all fold in so a
            // different run produces a different key. Once frozen,
            // none of the underlying bytes change — the key is stable
            // for the lifetime of this entry, and maya's hash-keyed
            // ComponentCache reuses the painted cells every frame.
            maya::CacheIdBuilder kb;
            kb.add(std::string_view{"agentty.turn.assistant_run"})
              .add(completing_midrun ? std::string_view{"cont"}
                                     : std::string_view{"head"})
              .add(static_cast<std::uint64_t>(run_end - i));
            for (std::size_t j = i; j < run_end; ++j) {
                kb.add(std::string_view{m.d.current.messages[j].id.value});
                kb.add(m.d.current.messages[j].compute_render_key());
            }
            cfg.hash_id = kb.build();
            push_frozen(m, maya::Turn{std::move(cfg)}.build(),
                        estimate_run_rows(m, i, run_end));
            ++m.ui.frozen_turn;
            // Run is now wholly frozen — the mid-run split (if any) is
            // resolved; the next run starts fresh with a header.
            m.ui.frozen_midrun = false;
        } else {
            // User / compaction-summary single-message Turn.
            int turn_num = m.ui.frozen_turn;
            auto cfg = ui::turn_config(head, i, turn_num, m,
                                       /*continuation=*/false,
                                       /*meta_override=*/{},
                                       /*tool_calls_override=*/{});
            cfg.hash_id = maya::CacheIdBuilder{}
                .add(std::string_view{"agentty.turn"})
                .add(std::string_view{head.id.value})
                .add(head.compute_render_key())
                .build();
            push_frozen(m, maya::Turn{std::move(cfg)}.build(),
                        estimate_run_rows(m, i, run_end));
        }

        i = run_end;
    }

    m.ui.frozen_through = to;
}

} // namespace

void freeze_through(Model& m, std::size_t live_start) {
    if (live_start <= m.ui.frozen_through) return;
    freeze_range(m, m.ui.frozen_through, live_start);
}

void freeze_settled_subturns(Model& m) {
    const auto& msgs = m.d.current.messages;
    const std::size_t total = msgs.size();
    if (m.ui.frozen_through >= total) return;

    // Only act on a live tail that begins with an Assistant run (the
    // auto-pilot case). A User head means a fresh turn boundary that
    // freeze_through handles on submit.
    const std::size_t run_start = m.ui.frozen_through;
    if (msgs[run_start].role != Role::Assistant) return;

    const std::size_t run_end = ui::turn_run_end(msgs, run_start);

    // Find the first sub-turn that is NOT yet freezable — i.e. carries a
    // non-terminal tool. We freeze the contiguous terminal prefix
    // [run_start, cut) and leave [cut, run_end) live. A sub-turn with no
    // tools at all (pure streaming text) is also a stop point: its text
    // may still be growing, so it must stay live.
    std::size_t cut = run_start;
    for (std::size_t i = run_start; i < run_end; ++i) {
        const Message& mm = msgs[i];
        bool terminal_tools = !mm.tool_calls.empty();
        for (const auto& tc : mm.tool_calls)
            if (!tc.is_terminal()) { terminal_tools = false; break; }
        // A settled TEXT-ONLY sub-turn is also freezable: no tools, a
        // committed `text` body, and nothing still streaming into it.
        // This is the prefix freeze_streaming_text_prefix carves off a
        // long prose answer — its bytes are final, so it can graduate
        // into the frozen prefix immediately. The ACTIVE tail keeps its
        // bytes in streaming_text, so it never matches here and stays
        // live.
        const bool settled_text_only =
            mm.tool_calls.empty()
            && !mm.text.empty()
            && mm.streaming_text.empty()
            && mm.pending_stream.empty();
        // Freeze a sub-turn only if it's a settled terminal-tool batch
        // OR a settled text-only block. The active streaming placeholder
        // (empty, or growing streaming_text) stays live.
        if (!terminal_tools && !settled_text_only) break;
        cut = i + 1;
    }

    // Never freeze the entire run here — the last sub-turn is the active
    // one (streaming tail / pending continuation) and must stay live so
    // freeze_through (at idle) does the final, turn-completing freeze
    // with the correct turn-number advance. Leaving >=1 sub-turn live
    // also means there's always a live remainder to render as the
    // continuation, so the header (painted in the frozen prefix) is
    // never duplicated.
    if (cut >= run_end) cut = run_end - 1;
    if (cut <= run_start) return;   // nothing freezable yet

    // The frozen entry is a CONTINUATION iff this run's header was
    // already committed to frozen by an earlier mid-run freeze this
    // turn. The FIRST mid-run freeze of a run carries the header; every
    // subsequent one continues it. Either way the live remainder is a
    // continuation (header already frozen), so frozen_midrun is set.
    const bool entry_continuation = m.ui.frozen_midrun;

    // Leading gap: same discipline as freeze_range — a gap before the
    // entry unless it's the very first frozen row OR it's a continuation
    // (continuations have no inter-turn seam; the header turn owns the
    // gap above it).
    if (!m.ui.frozen.empty() && !entry_continuation) {
        push_frozen(m, gap_row(), 1);
    }

    // Turn number mirrors what the live tail showed for this run
    // (frozen_turn + 1) so a continuation freeze that splits later does
    // not renumber. We do NOT ++frozen_turn: the run is unfinished.
    int turn_num = m.ui.frozen_turn + 1;
    ui::FrozenBuildScope frozen_scope;   // full body in the frozen snapshot
    auto cfg = ui::turn_config_for_assistant_run(
        run_start, cut, turn_num, m, entry_continuation);

    // Hash key identical in shape to freeze_range's so the freeze
    // handoff from the live tail is a cache HIT (same key the live tail
    // stamped while the prefix waited). Includes a continuation marker
    // so a prefix and its later self-freeze can't collide.
    maya::CacheIdBuilder kb;
    kb.add(std::string_view{"agentty.turn.assistant_run"})
      .add(entry_continuation ? std::string_view{"cont"} : std::string_view{"head"})
      .add(static_cast<std::uint64_t>(cut - run_start));
    for (std::size_t j = run_start; j < cut; ++j) {
        kb.add(std::string_view{msgs[j].id.value});
        kb.add(msgs[j].compute_render_key());
    }
    cfg.hash_id = kb.build();
    push_frozen(m, maya::Turn{std::move(cfg)}.build(),
                estimate_run_rows(m, run_start, cut));

    m.ui.frozen_through = cut;
    m.ui.frozen_midrun  = true;   // remainder of this run is a continuation
}

namespace {

// Find the last markdown block boundary (a blank line, "\n\n") in
// `s[0..limit)` that is NOT inside an open ``` code fence. Returns the
// byte offset just past the boundary (start of the next block), or 0 if
// no safe split point exists. Scanning fence state from the start is
// O(limit); `limit` is bounded by the committed prefix we're about to
// move out of the live tail (it happens at most once per ~budget rows of
// growth), so this is not a per-frame O(body) tax.
//
// Out-params describe the state at the returned boundary so the caller
// can handle the inside-a-fence fallback (see freeze_streaming_text_prefix):
//   • `fence_open`   — true iff `s[0..limit)` ends INSIDE an open fence
//                      with no clean boundary found (best stayed 0).
//   • `fence_marker` — the exact opening-fence line (e.g. "```python" or
//                      "~~~") so a fallback split can close + reopen the
//                      same fence kind / info string. Empty when not in a
//                      fence.
//   • `last_line_nl` — byte offset just past the last newline at or before
//                      `limit` (a fence-internal split point). 0 if none.
std::size_t last_safe_block_split(const std::string& s, std::size_t limit,
                                  bool& fence_open,
                                  std::string& fence_marker,
                                  std::size_t& last_line_nl) {
    if (limit == 0 || limit > s.size()) limit = s.size();
    bool in_fence = false;
    std::string open_marker;       // info line of the currently-open fence
    std::size_t best = 0;          // start-of-next-block offset, fence-closed
    std::size_t line_start = 0;
    std::size_t last_nl = 0;
    for (std::size_t i = 0; i < limit; ) {
        std::size_t eol = s.find('\n', i);
        const bool last_line = (eol == std::string::npos || eol >= limit);
        const std::size_t line_end = last_line ? limit : eol;
        // A line opening/closing a fence: starts with ``` or ~~~ after
        // optional leading spaces. Toggling on either bound is enough
        // for split safety (we only need to know we're between blocks).
        std::size_t p = line_start;
        while (p < line_end && (s[p] == ' ' || s[p] == '\t')) ++p;
        if (line_end - p >= 3
            && ((s[p] == '`' && s[p+1] == '`' && s[p+2] == '`')
             || (s[p] == '~' && s[p+1] == '~' && s[p+2] == '~'))) {
            if (!in_fence) {
                // Opening fence: remember the whole line (marker + info
                // string) so a fallback close/reopen reproduces it.
                open_marker.assign(s, line_start, line_end - line_start);
            }
            in_fence = !in_fence;
            if (!in_fence) open_marker.clear();
        }
        // Blank line = block boundary. Only a candidate split when the
        // fence is closed at this point (splitting mid-fence would
        // render two broken code blocks).
        if (p == line_end && !in_fence) {
            // The boundary is the byte just past this blank line's
            // newline, i.e. the start of the next block's first line.
            best = last_line ? line_end : (eol + 1);
        }
        if (!last_line) last_nl = eol + 1;
        if (last_line) break;
        i = eol + 1;
        line_start = i;
    }
    fence_open   = in_fence && best == 0;
    fence_marker = in_fence ? open_marker : std::string{};
    last_line_nl = last_nl;
    return best;
}

} // namespace

void freeze_streaming_text_prefix(Model& m) {
    auto& msgs = m.d.current.messages;
    if (msgs.empty()) return;

    // The active sub-turn is the back message. Only split a PURE-TEXT
    // assistant tail: no tool_calls (those are handled by
    // freeze_settled_subturns), and the growing body lives in
    // streaming_text. A message that already has settled `text` is a
    // mid-run continuation whose prior prefix is settled but not yet
    // frozen as its own message — we still only split the streaming
    // portion so the freeze stays append-only.
    Message& active = msgs.back();
    if (active.role != Role::Assistant) return;
    if (!active.tool_calls.empty()) return;
    if (active.streaming_text.empty()) return;

    // Keep a trailing window LIVE so the actively-revealing edge keeps
    // animating and we never split inside the block the model is still
    // writing. This window must stay SMALL: if the live (unfrozen)
    // portion grows past one viewport it overflows into committed
    // scrollback, and the markdown widget's per-block reveal can shift a
    // blank line by ±1 row as a `\n\n` boundary commits — rewriting a
    // row already emitted to native scrollback (a duplication ghost).
    // Splitting early keeps the oscillating region on-screen (mutable),
    // so the seam stays clean.
    //
    // Width-aware: "well under a viewport of prose" is a ROW budget, not
    // a fixed byte count — 1 KB of prose is ~13 rows at 80 cols but ~25+
    // rows at 40 cols. Size the live tail off the real wrap width so a
    // narrow terminal doesn't let the live region silently exceed a
    // viewport. ~half a screen of rows, floored at 1 KB.
    const int cols = estimate_wrap_cols();
    const std::size_t kLiveTailBytes = std::max<std::size_t>(
        1024,
        static_cast<std::size_t>(cols) *
            std::max<std::size_t>(8, frozen_row_budget() / 4));
    if (active.streaming_text.size() <= kLiveTailBytes) return;
    const std::size_t scan_limit = active.streaming_text.size() - kLiveTailBytes;

    // Split as soon as there's a committed block beyond the live window.
    // The freeze handoff is a cache hit (same hash key the live tail
    // stamped), so frequent small splits are cheap; the cost we're
    // avoiding is a tall live re-layout every frame. A modest floor
    // avoids churning on tiny prefixes.
    constexpr std::size_t kMinSplitBytes = 512;
    if (scan_limit < kMinSplitBytes) return;

    bool        fence_open = false;
    std::string fence_marker;
    std::size_t last_line_nl = 0;
    std::size_t split = last_safe_block_split(active.streaming_text, scan_limit,
                                              fence_open, fence_marker,
                                              last_line_nl);

    // Fence fallback. A single giant code block ("write the whole file")
    // has NO blank-line boundary outside an open fence, so the clean
    // split above returns 0 and the entire block stays live — re-laid-out
    // every frame, the exact unbounded cost this function exists to bound
    // and the content shape most likely to be huge. When we're stuck
    // inside one fence and the scan window is well past a viewport, split
    // at the last fence-internal newline and CLOSE + REOPEN the fence:
    // the frozen prefix gets a synthetic closing marker so it renders as
    // a complete code block, and the live tail is re-seeded with the same
    // opening marker so its remaining lines keep rendering as that same
    // language. Content the user sees is identical; only an internal
    // fence boundary is synthesized at a line break.
    std::string reopen_prefix;   // prepended to the live tail's text
    bool fence_fallback = false;
    if (split == 0 && fence_open && !fence_marker.empty()
        && last_line_nl >= kMinSplitBytes) {
        // Synthesize a CLOSING fence that the CommonMark engine will
        // actually accept. Per cm_block.cpp append_to_leaf, a line closes
        // a fence iff its leading fence-char run length is >= the OPENING
        // fence's length and the rest of the line is blank. So the close
        // must echo the opening run length, not a fixed 3 — a model that
        // opened with ````` ```` ````` (4+ backticks, used when the code
        // itself contains a triple-backtick) would NOT be closed by
        // ``` and the stray marker would render as code content.
        const char fc = (fence_marker.find('~') != std::string::npos) ? '~' : '`';
        std::size_t run = 0;
        while (run < fence_marker.size() && fence_marker[run] == fc) ++run;
        if (run < 3) run = 3;
        std::string close_marker(run, fc);
        split          = last_line_nl;
        reopen_prefix  = fence_marker + "\n";
        fence_fallback = true;
        // The committed prefix needs the closing marker appended below.
        fence_marker   = std::move(close_marker);
    }

    if (split == 0) return;   // no safe boundary yet (still one short fence)

    // Carve the committed prefix out of the active tail. If this is the
    // FIRST split of this sub-turn, `active.text` is empty and the
    // prefix is purely streamed bytes. On a later split `active.text`
    // may hold an earlier settled sub-turn's body (text→tool→text run);
    // that body belongs to a DIFFERENT logical message already, so we
    // never fold it here — we only move streaming_text bytes.
    std::string prefix_body = active.streaming_text.substr(0, split);
    active.streaming_text.erase(0, split);
    if (fence_fallback) {
        // Close the fence in the frozen half so it parses as a complete
        // code block, and reopen it in the live half so the remaining
        // lines keep rendering as that same language.
        prefix_body += fence_marker;   // synthetic closing ``` / ~~~
        prefix_body += '\n';
        active.streaming_text.insert(0, reopen_prefix);
    }

    // The new settled message carries the committed prefix as `text`
    // (settled), inheriting the active message's identity-relevant
    // fields. A fresh MessageId keeps the render-cache keyed per visual
    // unit. Inserted just before the active tail so the run reads
    // [settled-text][growing-text] — identical shape to a post-tool
    // continuation, which the live tail / freeze path already handle.
    Message settled;
    settled.role      = Role::Assistant;
    settled.text      = std::move(prefix_body);
    settled.timestamp = active.timestamp;
    // Insert before back(): msgs.end() - 1.
    msgs.insert(msgs.end() - 1, std::move(settled));

    // The settled prefix is now its own terminal (no tools, has text)
    // sub-turn in the live tail. freeze_settled_subturns will freeze it
    // on this same tick (called right after this in the Tick handler),
    // collapsing it into the zero-copy hash-keyed frozen prefix and
    // leaving only the small live tail to re-paint each frame.
}

void clear_frozen(Model& m) {
    m.ui.frozen.clear();
    m.ui.frozen_rows.clear();
    m.ui.frozen_row_total = 0;
    m.ui.frozen_through = 0;
    m.ui.frozen_turn    = 0;
    m.ui.frozen_midrun  = false;
}

void rehydrate_frozen(Model& m) {
    clear_frozen(m);
    const auto& msgs = m.d.current.messages;
    const std::size_t total = msgs.size();
    if (total == 0) return;

    // Bounded rehydrate. The live frozen vector IS the inline canvas:
    // every full repaint maya does (resume swap, RESIZE → Divergent
    // wipe+repaint, Ctrl-L) walks the whole canvas top-to-bottom, and
    // the user WATCHES that paint. So the live canvas height — not just
    // resume — is what must stay small. A generous 1500-row seed still
    // means every resize repaints 1500 rows from the top.
    //
    // Bound the seed to a SMALL multiple of the viewport (~2 screens).
    // Everything older was emitted to the terminal's NATIVE scrollback
    // on the very first paint (or while live), so scroll-up still shows
    // it — painted instantly by the terminal, never re-emitted by us.
    // The full transcript is intact on disk; recall older context via
    // the picker. Keeping the canvas ~2 screens makes resume, resize,
    // and redraw all paint only ~2 screens.
    //
    // Walk backward over whole speaker-runs accumulating estimated
    // rows; stop after the run that crosses the budget so the most
    // recent context is always whole. If the newest run alone exceeds
    // the budget, cut INSIDE it at sub-turn granularity (keep the
    // trailing sub-turns that fit) so even a giant final auto-pilot run
    // resumes fast.
    const std::size_t kRehydrateRowBudget = frozen_row_budget();
    std::size_t units      = 0;
    std::size_t row_budget = 0;
    std::size_t start      = total;
    std::size_t cursor     = total;
    while (cursor > 0) {
        std::size_t j = cursor;
        if (msgs[j - 1].role == Role::Assistant) {
            while (j > 0 && msgs[j - 1].role == Role::Assistant) --j;
        } else {
            --j;
        }
        std::size_t run_rows = 0;
        for (std::size_t k = j; k < cursor; ++k)
            run_rows += estimate_msg_rows(msgs[k]);

        if (units == 0 && run_rows > kRehydrateRowBudget
            && (cursor - j) > 1) {
            std::size_t kept = 0;
            std::size_t cut  = cursor;
            for (std::size_t k = cursor; k-- > j; ) {
                kept += estimate_msg_rows(msgs[k]);
                cut = k;
                if (kept >= kRehydrateRowBudget) break;
            }
            ++units;
            start = cut;
            row_budget += kept;
            break;
        }

        ++units;
        start = j;
        row_budget += run_rows;
        if (row_budget >= kRehydrateRowBudget) break;
        cursor = j;
    }

    // Seed frozen_turn to the count of assistant runs in the skipped
    // prefix so visible turn numbers reflect their true position
    // (e.g. "turn 87" not "turn 1" on a long reload).
    int skipped_assistant_runs = 0;
    for (std::size_t k = 0; k < start; ) {
        const std::size_t run_end = ui::turn_run_end(msgs, k);
        if (msgs[k].role == Role::Assistant) ++skipped_assistant_runs;
        k = run_end;
    }
    m.ui.frozen_turn = skipped_assistant_runs;

    freeze_range(m, start, total);
}

maya::Cmd<Msg> trim_frozen_if_oversized(Model& m) {
    // Soft cap on the frozen prefix. Above it, the oldest entries are
    // dropped — maya's row diff sees a shorter live tree and the
    // already-overflowed rows naturally commit to native scrollback.
    //
    // Why ROWS, not entries: the inline canvas auto-resizes to
    // `frozen_row_total + chrome`, and maya re-derives a full
    // O(rows x width) canvas witness EVERY frame (see maya
    // canvas_witness.cpp verify_canvas / verify_shadow). So the
    // per-frame render cost — and the animation lag the user feels on
    // a long thread — scales with TOTAL FROZEN ROWS, not entry count.
    // A single full `write`/`edit` body is hundreds of rows in ONE
    // entry, so an entry-count cap alone can't bound the canvas: 80
    // entries of fat tool panels still reach ~5000 rows and push
    // per-frame render past 15 ms. Capping rows keeps the canvas
    // bounded regardless of how tall any individual entry is.
    //
    // Older turns stay in the terminal's native scrollback (committed
    // there when they overflowed live), and the full message history
    // is intact on disk — only the in-app re-render window shrinks.
    // Composer history (↑) and thread reload are unaffected.
    //
    // The per-frame inline render cost is dominated by THREE passes
    // that are each O(canvas_rows x width) and run EVERY tick:
    //   1. render_tree over the full element tree (layout/measure),
    //   2. canvas_.clear() (streaming_fill over every cell),
    //   3. the canvas/shadow witness scan (verify_canvas).
    // canvas_rows tracks frozen_row_total, so to keep the spinner /
    // input latency flat on an arbitrarily long thread we must keep
    // frozen_row_total bounded to a SMALL multiple of the viewport.
    // Anything that has scrolled past the top of the viewport already
    // lives in the terminal's OWN scrollback (it was painted live
    // once, full body and all) — re-rendering it inside agentty every
    // frame buys nothing but lag. The user scrolls back through it
    // with the terminal, not the app.
    //
    // ~1500 rows ≈ several full viewports of recent work; bounds the
    // canvas enough to keep per-frame render in budget while trimming
    // INFREQUENTLY — each trim issues commit_scrollback_overflow and
    // shrinks the live frozen tree, which churns maya's inline diff, so
    // a too-tight cap that fires every few turns is its own source of
    // redraw stutter. The entry cap is a secondary guard against
    // pathological counts of tiny entries. Trimming drops whole
    // entries from the front until BOTH caps are satisfied, leaving at
    // least the most recent few entries no matter how tall they are —
    // full bodies are NEVER collapsed (the `show_all` UX is intact);
    // they simply graduate from the in-app re-render window into
    // native terminal scrollback.
    // Live-canvas row cap = ~2 viewports (same budget rehydrate seeds
    // to, so steady-state and resume agree). Above it, the oldest
    // entries are dropped; maya's row diff sees a shorter live tree and
    // the already-overflowed rows commit to native scrollback. Bounding
    // the canvas to ~2 screens keeps EVERY full repaint cheap — the
    // per-frame tick, a resize (Divergent wipe+repaint), and Ctrl-L all
    // walk only ~2 screens instead of thousands of rows.
    const std::size_t kFrozenMaxRows = frozen_row_budget();
    constexpr std::size_t kFrozenMaxEntries = 120;
    // Retention floor. Expressed in ROWS, not a fixed entry count: the
    // canvas blit cost is O(rows), and even on a cache HIT maya copies
    // every cached cell into the back buffer each frame (the hash_id
    // cache saves the body REBUILD — markdown parse, tool-card
    // construction — but not the per-frame cell copy). So a tail of a
    // few multi-thousand-row write/edit bodies keeps the canvas at
    // ~6000 rows and pins per-frame render near 50ms even though
    // nothing is changing. Those bodies already painted to native
    // terminal scrollback when they were live; re-blitting them every
    // frame buys nothing. The keep count is therefore ROW-driven: keep
    // trailing entries until they cover ~kFrozenMaxRows of recent work,
    // never fewer than 2 (always show some context) and never more
    // than kKeepMinEntries when the row budget is already met by fewer
    // — a tail of many TINY turns still shows kKeepMinEntries of them,
    // but a tail of giant bodies trims down to the 1–2 that fit the
    // budget instead of being floored at 8 fat panels.
    constexpr std::size_t kKeepMinEntries = 8;

    const bool over_rows    = m.ui.frozen_row_total > kFrozenMaxRows;
    const bool over_entries = m.ui.frozen.size() > kFrozenMaxEntries;
    if (!over_rows && !over_entries) return maya::Cmd<Msg>::none();

    // Row-driven keep count: walk back from the newest entry summing
    // rows until we've covered kFrozenMaxRows. That entry count is the
    // floor. Then widen to kKeepMinEntries ONLY if those entries were
    // small enough that the row budget wasn't yet spent (so small-turn
    // tails keep ample context); a single giant body that already
    // fills the budget keeps just itself (clamped to a 2-entry min so
    // the very latest exchange is always visible).
    std::size_t budget_entries = 0;
    std::size_t keep_rows      = 0;
    for (std::size_t k = m.ui.frozen.size(); k-- > 0; ) {
        ++budget_entries;
        keep_rows += static_cast<std::size_t>(m.ui.frozen_rows[k]);
        if (keep_rows >= kFrozenMaxRows) break;
    }
    // If the row budget was met before reaching kKeepMinEntries, the
    // budget count wins (giant bodies). If it wasn't (small turns),
    // allow up to kKeepMinEntries for context.
    std::size_t keep_entries =
        (keep_rows >= kFrozenMaxRows)
            ? budget_entries
            : std::max(budget_entries, kKeepMinEntries);
    if (keep_entries < 2)                  keep_entries = 2;
    if (keep_entries > m.ui.frozen.size()) keep_entries = m.ui.frozen.size();

    // Drop entries from the front until both caps are satisfied,
    // bounded by the retention floor computed above.
    std::size_t drop = 0;
    const std::size_t max_drop = m.ui.frozen.size() - keep_entries;
    std::size_t rows_after    = m.ui.frozen_row_total;
    std::size_t entries_after = m.ui.frozen.size();
    while (drop < max_drop
           && (rows_after > kFrozenMaxRows || entries_after > kFrozenMaxEntries)) {
        rows_after -= static_cast<std::size_t>(m.ui.frozen_rows[drop]);
        --entries_after;
        ++drop;
    }
    if (drop == 0) return maya::Cmd<Msg>::none();

    // Keep frozen / frozen_rows / frozen_row_total in lockstep.
    std::size_t removed_rows = 0;
    for (std::size_t k = 0; k < drop; ++k)
        removed_rows += static_cast<std::size_t>(m.ui.frozen_rows[k]);
    m.ui.frozen.erase(m.ui.frozen.begin(),
                      m.ui.frozen.begin() + static_cast<std::ptrdiff_t>(drop));
    m.ui.frozen_rows.erase(m.ui.frozen_rows.begin(),
                           m.ui.frozen_rows.begin() + static_cast<std::ptrdiff_t>(drop));
    m.ui.frozen_row_total -= removed_rows;

    // commit_scrollback_overflow lets maya derive the safe row count
    // itself (max(0, prev_rows - term_h)) — the Cmd is just a trigger
    // saying "please release whatever has already overflowed." This
    // is the safe variant; the row-counted commit_scrollback was
    // retired in the maya audit (see scrollback-corruption-audit.md
    // finding #1) because no caller outside the renderer can know
    // the right physical-row count.
    return maya::Cmd<Msg>::commit_scrollback_overflow();
}

maya::Cmd<Msg> trim_frozen_above_viewport(Model& m) {
    // Mid-run-safe trim. The standard trim_frozen_if_oversized can drop
    // an entry that is still ON SCREEN (its rows haven't overflowed the
    // viewport yet) — mid-run that re-emits already-committed scrollback
    // rows at a shifted position and the turn appears twice (the
    // duplication bug documented in tool.cpp). This variant is the
    // conservative version that runs DURING an active run: it only drops
    // entries that are provably above the viewport.
    //
    // Safety margin: keep at least kViewportKeepRows of the MOST RECENT
    // frozen content on the canvas, plus the live tail maya renders on
    // top of frozen. For a dropped entry to be off-screen, the kept
    // frozen rows + live tail must exceed term_h; keeping >= term_h of
    // frozen alone already guarantees the oldest KEPT entry's top sits
    // at-or-above the viewport top, so everything dropped is provably in
    // native scrollback. We keep ~1.5x viewport (a cushion over the 1x
    // floor) so the canvas stays near a single screen during a long run
    // — per-frame cost (clear + verify + blit) stays flat and minimal —
    // while never racing the visible region.
    // Query terminal geometry through the SAME helper the row estimate
    // uses (term_dims) so the keep-margin and the per-entry row counts
    // are computed against one consistent terminal state — a width/height
    // mismatch across a frame is exactly what desyncs the "provably above
    // the viewport" proof.
    const int term_h = term_dims().rows;
    const std::size_t kViewportKeepRows =
        static_cast<std::size_t>(std::max(64, (term_h * 3) / 2));

    // Only worth doing once the canvas is meaningfully over the keep
    // margin — trimming churns maya's inline diff (commit_scrollback_
    // overflow + a shorter frozen tree), so we don't want to fire it on
    // every settled sub-turn. Require the prefix to exceed the keep
    // margin by at least one viewport before acting.
    if (m.ui.frozen_row_total <= kViewportKeepRows + static_cast<std::size_t>(term_h))
        return maya::Cmd<Msg>::none();

    // Walk back from the newest entry, summing rows. The first entry
    // whose inclusion pushes the running total past kViewportKeepRows is
    // the OLDEST entry we must keep — everything before it is above the
    // viewport and safe to drop. Always keep at least the 2 most recent
    // entries (the active run's header + current sub-turn).
    std::size_t kept_rows = 0;
    std::size_t keep_from = m.ui.frozen.size();   // index of oldest kept entry
    for (std::size_t k = m.ui.frozen.size(); k-- > 0; ) {
        kept_rows += static_cast<std::size_t>(m.ui.frozen_rows[k]);
        keep_from = k;
        if (kept_rows >= kViewportKeepRows) break;
    }
    // Never drop the last 2 entries no matter what.
    if (m.ui.frozen.size() >= 2 && keep_from > m.ui.frozen.size() - 2)
        keep_from = m.ui.frozen.size() - 2;

    const std::size_t drop = keep_from;
    if (drop == 0) return maya::Cmd<Msg>::none();

    std::size_t removed_rows = 0;
    for (std::size_t k = 0; k < drop; ++k)
        removed_rows += static_cast<std::size_t>(m.ui.frozen_rows[k]);
    m.ui.frozen.erase(m.ui.frozen.begin(),
                      m.ui.frozen.begin() + static_cast<std::ptrdiff_t>(drop));
    m.ui.frozen_rows.erase(m.ui.frozen_rows.begin(),
                           m.ui.frozen_rows.begin() + static_cast<std::ptrdiff_t>(drop));
    m.ui.frozen_row_total -= removed_rows;

    return maya::Cmd<Msg>::commit_scrollback_overflow();
}

} // namespace agentty::app::detail
