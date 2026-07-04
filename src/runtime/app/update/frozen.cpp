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
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

#include <maya/dsl.hpp>
#include <maya/element/text.hpp>   // maya::string_width (display-column count)
#include <maya/layout/yoga.hpp>     // maya::layout::compute (exact rendered height)
#include <maya/platform/io.hpp>
#include <maya/render/cache_id.hpp>
#include <maya/render/renderer.hpp> // maya::render_detail::build_layout_tree
#include <maya/style/theme.hpp>
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

// Effective wrap width used by the row estimate, derived from an explicit
// terminal-column count. Clamped to a sane floor so a transiently-bogus
// 1-col read can't blow the estimate up to thousands of rows (which would
// over-trim). The renderer reserves a couple of columns for the turn rail
// / gutter, so subtract a small margin. Callers that already hold a
// term_dims() snapshot pass its .cols so every row-math call in one trim
// reflects ONE terminal state (the proof depends on it); the no-arg
// overload queries fresh for the one-shot freeze paths.
int estimate_wrap_cols(int term_cols) {
    // Rail + padding eat ~4 columns of real content width.
    return std::max(16, term_cols - 4);
}
int estimate_wrap_cols() { return estimate_wrap_cols(term_dims().cols); }

// EXACT content width a frozen Turn Element is laid out at by the real
// render. A frozen entry is nested under TWO horizontal-padding boxes
// before maya measures it, and BOTH must be subtracted here:
//
//   AppLayout::build:    vstack().padding(1)      -> left 1 + right 1 = 2
//   Conversation::build: v(rows) | padding(0, 1)  -> left 1 + right 1 = 2
//
// (Thread::build is a pass-through and list_ref adds no chrome.) So the
// frozen Element receives term_cols - 4 columns, NOT term_cols - 2. The
// earlier -2 counted only AppLayout's pad and MISSED Conversation's
// padding(0, 1); on a narrow viewport that 2-col over-estimate of the
// wrap width UNDER-counts every wrapped entry's height (fewer rows than
// maya actually paints), so the trim's commit_scrollback accounting
// undershoots and strands a duplicate row one screen up — the
// phone-over-SSH scrollback-duplication bug.
//
// The Turn owns its own rail + inner padding INSIDE the built Element
// `e`; the layout engine subtracts that itself when measure_element_rows
// runs compute() on `e`, identically to the real render — so only the
// EXTERNAL 4 cols belong here. This is the width measure_element_rows
// MUST use: frozen_rows[] equals the wire height by construction only
// when measured at the width maya actually wraps at. Verified empirically
// — for every width W, the real nested render assigns the frozen element
// exactly the height it has at W-4 (and the coarse estimate_wrap_cols
// already uses term_cols - 4 for the same reason).
int measure_cols(int term_cols) {
    return std::max(16, term_cols - 4);
}

// Live-canvas row budget = a small multiple of the terminal viewport,
// derived from an explicit terminal-row count (see estimate_wrap_cols for
// the one-snapshot rationale).
// The live m.ui.frozen vector IS the inline canvas; every full repaint
// (resume swap, resize→HardReset re-emit, Ctrl-L) walks it top to bottom
// and the user sees the paint. Older rows live on disk (recall via
// picker). On width-resize maya emits \x1b[3J which wipes native
// scrollback, so anything outside this budget at the moment of resize
// is gone — sized to keep a useful recent window restorable.
std::size_t frozen_row_budget(int term_rows) {
    // ~3 viewports, floored so a tiny window still keeps useful context.
    // Trade-off: bigger window = more recent turns restored on resize
    // and a larger one-shot HardReset paint. Per-frame cost is unchanged
    // (maya hash_id cache hits on settled entries). 3x picked so an 80x24
    // terminal restores ~72 rows of recent scrollback after a width
    // resize while still bounding the canvas.
    return static_cast<std::size_t>(std::max(48, term_rows * 3));
}
std::size_t frozen_row_budget() { return frozen_row_budget(term_dims().rows); }

// Inter-turn gap: a blank row, the thin dim ─ rule, then another
// blank row. Pushed before each fresh-speaker turn so settled turns
// are visually separated with breathing room — the bare rule alone
// reads as cramped, fused to the previous answer and the next header.
// Three rows; keep push_frozen's row count in sync.
maya::Element gap_row() {
    using namespace maya::dsl;
    return v(blank(),
             maya::Conversation::divider(),
             blank()).build();
}
constexpr int kGapRows = 3;

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

// Escape hatch for the rehydrate-time off-screen body collapse below.
//
// OFF BY DEFAULT. The collapse only runs from rehydrate_frozen, whose ONLY
// caller is ThreadLoaded — loading a thread from disk. In that path the giant
// body is read from disk and was NEVER painted to THIS terminal's native
// scrollback, so replacing it with "⋯ N rows collapsed — scroll up in your
// terminal to view" hides content that simply is not there: scrolling up
// shows the previous thread / the picker, not the collapsed rows. That is the
// "wtf is this" regression — a loaded message silently swallowed behind a
// stub. The perf win (bounding a one-off cold repaint of a just-loaded
// oversized body) is not worth losing a loaded message, so the default is
// now full fidelity.
//
// Opt IN with AGENTTY_FROZEN_COLLAPSE=1 (or t/y) only if your workflow
// restarts agentty in the SAME terminal, where the body IS still in the
// native scrollback and the stub's pointer is truthful.
bool frozen_collapse_enabled() {
    const char* v = std::getenv("AGENTTY_FROZEN_COLLAPSE");
    if (!v || !*v) return false;   // default OFF — render every body full
    return v[0] == '1' || v[0] == 't' || v[0] == 'T'
        || v[0] == 'y' || v[0] == 'Y';
}

// Compact placeholder for an OFF-SCREEN body collapsed out of the in-app
// re-render window (see collapse_oversized_offscreen_entries). One dim ⋯ row,
// identical chrome to compaction_divider_row, so measure_element_rows reports
// ~1 row. The full body is untouched in the terminal's native scrollback
// (painted there when it was live) and on disk.
maya::Element collapsed_body_stub(std::size_t orig_rows) {
    maya::Turn::Config cfg;
    cfg.glyph      = "\xe2\x8b\xaf";   // ⋯
    cfg.label      = std::to_string(orig_rows)
                   + " rows collapsed \xe2\x80\x94 scroll up in your terminal to view";
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

// Cheap display-column row estimate for a single message's contribution
// to a frozen Turn. NOT a render — a coarse proxy used only to BOUND
// the frozen canvas height. Built to be one-sided (exact or UNDER, never
// OVER) so both trims' keep-loops are safe by construction. Shared by
// rehydrate_frozen (budget walk) and freeze_range (per-entry frozen_rows
// accounting).

// Display columns occupied by a single physical line (no embedded
// newline), and the wrapped row count it produces at width `cols`.
// Uses maya::string_width — the SAME UCD-backed width table the renderer
// wraps with — so the estimate tracks real columns, not bytes. A CJK /
// emoji line that is 3 bytes per glyph no longer inflates to ~3x its
// real row count; the byte-based over-count that forced the trims'
// asymmetric safety margins is gone, and the estimate is one-sided
// (exact or under) as the trim proofs require.
std::size_t line_display_rows(std::string_view line, std::size_t cols) {
    if (cols < 1) cols = 1;
    const int w = maya::string_width(line);
    if (w <= 0) return 1;                 // blank / zero-width line = 1 row
    return (static_cast<std::size_t>(w) + cols - 1) / cols;   // ceil
}

// Wrapped-row count for a text body at the given content width: each
// hard newline starts a fresh row, and a line wider than `cols` display
// columns soft-wraps to ceil(width/cols) rows. This mirrors what the
// renderer actually does (display columns, not bytes), so the estimate
// tracks the real canvas height closely and never OVER-counts multibyte
// text — the safe direction for both trims' keep-loops.
std::size_t wrapped_rows(std::string_view body, int cols) {
    const std::size_t w = static_cast<std::size_t>(cols < 1 ? 1 : cols);
    std::size_t rows = 0;
    std::size_t line_start = 0;
    while (true) {
        const std::size_t nl = body.find('\n', line_start);
        const std::size_t line_end =
            (nl == std::string_view::npos) ? body.size() : nl;
        rows += line_display_rows(
            body.substr(line_start, line_end - line_start), w);
        if (nl == std::string_view::npos) break;
        line_start = nl + 1;
    }
    return rows == 0 ? 1 : rows;
}

// Wrapped-row count contributed by every STRING value in a JSON args
// tree. A tool card renders its big payload (write `content`, edit
// `edits[].new_text`/`old_text`, etc.) one row per source line, so the
// real rendered height tracks newlines in those strings — NOT the JSON
// byte length. Summing per-string wrapped_rows counts each payload's
// real source lines (plus wrap), which tracks the rendered height
// closely without OVER-counting — the safe side for the mid-run trim's
// keep-loop (see estimate_msg_rows). Non-string scalars contribute
// nothing (they render inline in the header, not the body). Cheap: no
// allocation, no dump().
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

// Max rendered BODY rows for a tool whose maya renderer ELIDES output to
// a fixed head/tail budget regardless of show_all. tool_body_preview.hpp:
//   bash/diagnostics → bash_output_tail, bash_tail = 4
//   grep/glob/list_dir/web_search/git_* → code_block head+tail = 4+3 = 7
//   read/find_definition → file_read, read_head = 5
//   web_fetch → Json, head+tail ~ 7
// These render the SAME elided height live AND frozen (their render
// methods call elide() with fixed budgets and never honor show_all), so
// counting full wrapped_rows(output) OVER-counts by the elided amount —
// exactly what makes the mid-run keep-loop drop an on-screen entry and
// strand a committed-scrollback ghost. Returns 0 when the tool renders
// its full body (write/edit args path, git_diff show_all) so the caller
// counts it whole.
std::size_t tool_output_render_cap(std::string_view name) {
    if (name == "bash" || name == "diagnostics")          return 4;
    if (name == "read" || name == "find_definition")      return 5;
    if (name == "grep" || name == "glob" || name == "list_dir"
     || name == "web_search" || name == "web_fetch"
     || name == "git_status" || name == "git_log"
     || name == "git_commit")                             return 7;
    return 0;   // 0 ⇒ no cap (render full output)
}

// Wrapped-row count for PROSE rendered as markdown, accounting for the
// frozen build's auto-fold of long code blocks. cached_markdown_for
// folds any fenced code block longer than kFoldLineThreshold (40) lines
// to ~1 row in the SETTLED (frozen) render. A plain wrapped_rows() would
// count every line of such a block — a 100-line code block in a reply
// estimates ~100 rows but renders ~1 folded, OVER-counting by ~99 and
// tripping the mid-run keep-loop into a scrollback ghost (same class as
// the bash output cap). Walk the text: outside a fence count wrapped
// rows normally; inside a fence buffer the line count and, on close,
// add min(real_rows, fold_cap) so a long block collapses to the folded
// height. Mirrors maya's fold (markdown.hpp auto_fold_long_blocks).
std::size_t prose_rows(std::string_view body, int cols) {
    if (body.empty()) return 0;
    constexpr std::size_t kFoldLineThreshold = 40;
    std::size_t rows = 0;
    std::size_t line_start = 0;
    bool in_fence = false;
    std::size_t fence_lines = 0;
    std::size_t fence_wrapped = 0;
    std::size_t open_fence_rows = 0;
    const std::size_t w = static_cast<std::size_t>(cols < 1 ? 1 : cols);
    while (true) {
        const std::size_t nl = body.find('\n', line_start);
        const std::size_t line_end =
            (nl == std::string_view::npos) ? body.size() : nl;
        std::string_view line = body.substr(line_start, line_end - line_start);
        // Detect a fence delimiter: a line whose first non-space run is
        // ``` or ~~~ (info string allowed after the opening fence).
        std::string_view trimmed = line;
        while (!trimmed.empty() && trimmed.front() == ' ')
            trimmed.remove_prefix(1);
        const bool is_fence =
            trimmed.rfind("```", 0) == 0 || trimmed.rfind("~~~", 0) == 0;
        const std::size_t line_rows = line_display_rows(line, w);
        if (is_fence) {
            if (!in_fence) {
                in_fence = true;
                fence_lines = 0;
                fence_wrapped = 0;
                open_fence_rows = line_rows;
            } else {
                in_fence = false;
                // A folded block (body > threshold) collapses the WHOLE
                // block — open fence + body + close fence — to a single
                // "▸ N lines hidden" stub row (maya build.cpp). An
                // unfolded block renders open + body + close.
                rows += (fence_lines > kFoldLineThreshold)
                            ? 1
                            : open_fence_rows + fence_wrapped + line_rows;
            }
        } else if (in_fence) {
            ++fence_lines;
            fence_wrapped += line_rows;
        } else {
            rows += line_rows;
        }
        if (nl == std::string_view::npos) break;
        line_start = nl + 1;
    }
    // Unterminated fence (streaming snapshot) — count its buffered body
    // uncapped: it isn't folded until the block closes + settles.
    if (in_fence) rows += open_fence_rows + fence_wrapped;
    return rows == 0 ? 1 : rows;
}

// Body estimate against an EXPLICIT content width. The trims capture one
// term_dims() snapshot and pass its derived cols so every entry in a
// single keep-walk is measured against the SAME terminal state the
// margin was computed from — a width that drifts mid-walk is what
// desyncs the "provably above the viewport" proof.
std::size_t estimate_msg_rows(const Message& mm, int cols) {
    // Prose body: count real wrapped rows (newline-aware), folding long
    // code blocks to match the frozen render's auto-fold.
    std::size_t rows = 0;
    if (!mm.text.empty())           rows += prose_rows(mm.text, cols);
    if (!mm.streaming_text.empty()) rows += prose_rows(mm.streaming_text, cols);

    for (const auto& tc : mm.tool_calls) {
        // The RENDERED body of a tool card is one ROW PER SOURCE LINE
        // for write (line-numbered) / edit (per-hunk diff) — those echo
        // the full args payload (show_all). OUTPUT-based tools (bash,
        // read, grep, …) DON'T: their maya renderers elide output to a
        // fixed head/tail budget (see tool_output_render_cap), so a
        // 200-line bash dump renders ~4 rows. Counting the full output
        // for those over-counts — capped below.
        //
        // This count must NOT OVER-estimate. It feeds the trim's
        // keep-loop, which walks the NEWEST entries summing rows until
        // it has "kept the budget", then drops the rest. If a kept
        // entry over-counts, the keep sum reaches the margin before the
        // real rows do, so the trim drops an entry whose real rows are
        // STILL on screen — re-emitting committed scrollback rows = the
        // duplication ghost. An UNDER-count only keeps more (a taller
        // canvas, never a dropped on-screen entry), so under/exact is
        // the safe side.
        //
        // The body shows up in exactly one source per tool state:
        //   • settled  → parsed `args` (write content / edit hunks) or
        //                `output()` (read/grep result). args wins; we do
        //                NOT also add args_streaming, which holds the
        //                SAME bytes in raw-JSON form (append-only, never
        //                cleared) and would double the count.
        //   • streaming→ args not parsed yet; the live card renders from
        //                args_streaming, so count that instead.
        //
        // ELIDE CAP applies to the WHOLE body. For a tool whose maya
        // renderer elides to a fixed head/tail budget (bash/read/grep/
        // …), BOTH the args-derived rows AND the output() rows render
        // capped, not just output — counting either at full height
        // OVER-counts and trips the mid-run keep-loop into dropping an
        // on-screen entry (the duplication ghost). So compute the raw
        // body rows from whichever source the card draws from, then
        // clamp the SUM to the cap. Uncapped tools (write/edit args,
        // git_diff show_all) have cap==0 and count whole.
        const std::size_t out_cap = tool_output_render_cap(tc.name.value);
        std::size_t tool_rows = 0;
        if (!tc.args.is_null())
            tool_rows += estimate_json_string_rows(tc.args, cols);
        else if (!tc.args_streaming.empty())
            tool_rows += wrapped_rows(tc.args_streaming, cols);
        if (!tc.output().empty())
            tool_rows += wrapped_rows(tc.output(), cols);
        if (out_cap > 0 && tool_rows > out_cap)
            tool_rows = out_cap;
        // write / edit render their FULL body when settled (no head+tail
        // cap), so the estimate counts one row per source line via
        // estimate_json_string_rows above — no ceiling clamp. The estimate
        // is display-column accurate and one-sided UNDER (string_width,
        // ceil-wrap), the safe direction for the trim keep-loops.
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

// Convenience forwarder for the one-shot freeze paths (freeze_range,
// rehydrate_frozen) that don't already hold a dims snapshot.
std::size_t estimate_msg_rows(const Message& mm) {
    return estimate_msg_rows(mm, estimate_wrap_cols());
}

// Estimated rows for the run messages[from..to) that collapse into
// ONE frozen Turn entry, against an EXPLICIT content width. Returns
// std::size_t — a pathological run (huge thread, tiny terminal inflating
// wrap counts) can sum past INT_MAX, and truncating to a negative int
// would clamp the entry to 1 row in push_frozen, badly under-counting a
// giant entry so it never trims.
std::size_t estimate_run_rows(const Model& m, std::size_t from,
                              std::size_t to, int cols) {
    std::size_t rows = 0;
    for (std::size_t k = from; k < to && k < m.d.current.messages.size(); ++k)
        rows += estimate_msg_rows(m.d.current.messages[k], cols);
    return rows;
}
std::size_t estimate_run_rows(const Model& m, std::size_t from, std::size_t to) {
    return estimate_run_rows(m, from, to, estimate_wrap_cols());
}

// EXACT rendered height of a built frozen Element, measured through
// maya's REAL layout engine at the live content width — the same
// build_layout_tree + compute path render_tree runs. This is the ground
// truth: the number maya WILL emit for this entry, not a reimplemented
// guess (estimate_msg_rows). Storing the measured height in frozen_rows[]
// is what makes the trims scrollback-safe by CONSTRUCTION: the keep-loop
// margin and the commit count both derive from the same number maya
// renders, so they can never drift from the wire (the drift was the
// entire root cause of the recurring mid-run duplication / wipe bugs —
// agentty estimating a height maya renders differently).
//
// Cost: one build_layout_tree + compute per freeze (NOT per frame).
// Frozen entries are hash-keyed ComponentElements, so the measure is an
// O(1) cached-height leaf for the heavy bodies; the surrounding chrome
// is a handful of nodes. Freezes happen at most once per ~budget rows of
// growth, far off the per-frame hot path.
//
// `cols` is the FULL terminal width: the frozen Turn owns its rail +
// padding in its own FlexStyle, so the layout engine subtracts that
// chrome itself — measuring at term_cols reproduces exactly what the
// real thread-column render does (the thread fills the terminal width).
// Returns 0 only on a degenerate width / empty tree, so callers fall
// back to the estimate.
std::size_t measure_element_rows(const maya::Element& e, int cols) {
    if (cols < 1) return 0;
    thread_local std::vector<maya::layout::LayoutNode> nodes;
    nodes.clear();
    const std::size_t root =
        maya::render_detail::build_layout_tree(e, nodes, maya::theme::dark);
    if (root >= nodes.size()) return 0;
    maya::layout::compute(nodes, root, cols);
    const int h = nodes[root].computed.size.height.raw();
    return h > 0 ? static_cast<std::size_t>(h) : 0;
}

// Push a built frozen Element together with its rendered row count,
// keeping m.ui.frozen / m.ui.frozen_rows / m.ui.frozen_is_separator /
// m.ui.frozen_row_total in lockstep. EVERY push into m.ui.frozen must
// go through here so the parallel vectors never drift.
//
// GROUND TRUTH: the stored row count is maya's REAL rendered height
// (measure_element_rows runs the actual layout engine on the built
// Element at the live width), NOT the estimate. The `rows` argument is
// now only a FALLBACK for the degenerate case where measurement returns
// 0 (zero/invalid width). This is THE fix for the recurring mid-run
// scrollback corruption: frozen_rows[] drove both trim keep-loops and
// their commit counts, and an estimate that disagreed with maya's render
// by even one row dropped an on-screen entry (duplicate) or committed a
// still-visible row (wipe). Measuring through the same layout maya
// renders with makes frozen_rows[] equal to the wire by construction —
// drift is structurally impossible.
//
// `separator` is true for inter-turn gap rows / compaction dividers
// (entries that must never lead the prefix). Clamps to INT_MAX before
// storing in the int-typed frozen_rows[].
void push_frozen(Model& m, maya::Element e, std::size_t rows,
                 bool separator = false) {
    const int term_cols = term_dims().cols;
    // Heal any width drift in the existing prefix BEFORE measuring the new
    // entry, so every frozen_rows[] stamp shares ONE (the live) width — the
    // single-width invariant the trims' commit accounting depends on. (If a
    // resize landed between this freeze and the previous one without a trim
    // in between, the old stamps would otherwise stay at the prior width.)
    ensure_frozen_width(m, term_cols);
    const std::size_t measured =
        measure_element_rows(e, measure_cols(term_cols));
    if (measured > 0) rows = measured;
    if (rows < 1) rows = 1;
    const std::size_t clamped = std::min<std::size_t>(
        rows, static_cast<std::size_t>(std::numeric_limits<int>::max()));
    m.ui.frozen.push_back(std::move(e));
    m.ui.frozen_rows.push_back(static_cast<int>(clamped));
    m.ui.frozen_is_separator.push_back(separator);
    m.ui.frozen_row_total += clamped;
    m.ui.frozen_cols = term_cols;   // stamp the width this prefix is valid at
}

// Debug-time invariant: the three parallel vectors and the running sum
// must always agree. A single edit that erases one vector but forgets
// another is silent UB (an erase running past end()); this converts it
// to a loud abort in debug builds and is a no-op in release.
void assert_frozen_lockstep([[maybe_unused]] const Model& m) {
#ifndef NDEBUG
    assert(m.ui.frozen.size() == m.ui.frozen_rows.size());
    assert(m.ui.frozen.size() == m.ui.frozen_is_separator.size());
    std::size_t sum = 0;
    for (int r : m.ui.frozen_rows) sum += static_cast<std::size_t>(r);
    assert(sum == m.ui.frozen_row_total);
#endif
}

// Drop the first `drop` frozen entries, keeping all four parallel
// structures in lockstep. THE ONLY front-eviction primitive — every
// trim routes through here so a future edit can't desync one vector
// (which would make a later erase() run past end() = UB, the crash the
// long_session_bench documents). Returns the exact row count removed so
// the caller can size its commit_scrollback().
std::size_t pop_front_frozen(Model& m, std::size_t drop) {
    if (drop == 0) return 0;
    if (drop > m.ui.frozen.size()) drop = m.ui.frozen.size();
    std::size_t removed_rows = 0;
    for (std::size_t k = 0; k < drop; ++k)
        removed_rows += static_cast<std::size_t>(m.ui.frozen_rows[k]);
    const auto d = static_cast<std::ptrdiff_t>(drop);
    m.ui.frozen.erase(m.ui.frozen.begin(), m.ui.frozen.begin() + d);
    m.ui.frozen_rows.erase(m.ui.frozen_rows.begin(),
                           m.ui.frozen_rows.begin() + d);
    m.ui.frozen_is_separator.erase(m.ui.frozen_is_separator.begin(),
                                   m.ui.frozen_is_separator.begin() + d);
    m.ui.frozen_row_total -= removed_rows;
    assert_frozen_lockstep(m);
    return removed_rows;
}

// Strip leading separator entries (gap rows / compaction dividers) so
// the frozen prefix always opens on a real turn. A separator at index 0
// renders as a blank gap or an orphan rule at the top of the canvas
// (the "saved thread opens with a hole" / "trim left a gap" bug). Routes
// through pop_front_frozen so all four structures stay in lockstep. Does
// NOT touch frozen_through (that is a message-index bound, unrelated to
// leading-chrome trimming). Returns the row count removed.
std::size_t pop_front_frozen_leading_separators(Model& m) {
    std::size_t drop = 0;
    while (drop < m.ui.frozen_is_separator.size()
           && m.ui.frozen_is_separator[drop])
        ++drop;
    return pop_front_frozen(m, drop);
}

// Back-compat shim for the void-returning callers (rehydrate_frozen,
// freeze paths) that don't need the removed-row count.
void drop_leading_separators(Model& m) {
    (void)pop_front_frozen_leading_separators(m);
}

// Rehydrate-time collapse of OFF-SCREEN giant bodies. THE COLD-REPAINT BOUND.
//
// rehydrate_frozen rebuilds the in-app canvas on a FRESH paint (ThreadLoaded
// only — never mid-session), so shrinking entries here is scrollback-safe by
// construction: there is no committed prefix that must stay byte-accurate.
// (The mid-session trim is the OPPOSITE — its commit accounting must not be
// perturbed, so do NOT add a collapse there; that is the documented
// corruption trap.) The dominant cold-render cost on resume / resize / Ctrl-L
// is a single tool body taller than the whole frozen budget — e.g. a
// 3000-line write. When such a body is NOT the trailing entry (it has scrolled
// off-screen and the current result sits below it), re-rendering all its rows
// on every full repaint buys nothing: it is intact in the terminal's own
// scrollback. Replace it with a one-row stub; measure_element_rows re-stamps
// the true (now tiny) height so frozen_rows[] / frozen_row_total stay ground
// truth. The trailing entry (the result you're looking at) is ALWAYS full.
void collapse_oversized_offscreen_entries(Model& m) {
    if (!frozen_collapse_enabled())      return;
    if (m.ui.frozen.size() < 2)          return;  // only the current result
    const std::size_t budget = frozen_row_budget();
    if (m.ui.frozen_row_total <= budget) return;  // already fits — leave full

    // Trailing non-separator entry = the current result; never collapse it.
    std::size_t last_real = m.ui.frozen.size();
    for (std::size_t k = m.ui.frozen.size(); k-- > 0; )
        if (!m.ui.frozen_is_separator[k]) { last_real = k; break; }

    const int cols = measure_cols(term_dims().cols);
    for (std::size_t k = 0; k < m.ui.frozen.size(); ++k) {
        if (k == last_real)               continue;  // keep current result whole
        if (m.ui.frozen_is_separator[k])  continue;  // gaps / dividers, not bodies
        const std::size_t cur =
            static_cast<std::size_t>(m.ui.frozen_rows[k]);
        if (cur <= budget)                continue;  // not a giant — leave full

        maya::Element stub = collapsed_body_stub(cur);
        std::size_t h = measure_element_rows(stub, cols);
        if (h < 1) h = 1;
        const std::size_t clamped = std::min<std::size_t>(
            h, static_cast<std::size_t>(std::numeric_limits<int>::max()));
        m.ui.frozen[k]        = std::move(stub);
        m.ui.frozen_row_total = m.ui.frozen_row_total - cur + clamped;
        m.ui.frozen_rows[k]   = static_cast<int>(clamped);
    }
    assert_frozen_lockstep(m);
}

// Run-level safety gate: a frozen turn captures an Element snapshot
// whose hash_id is stamped once and never recomputed. Two ways a
// not-yet-settled run could poison that snapshot:
//
//  • a Pending / Approved / Running tool — its status mutates later
//    (when ToolExecOutput lands) but the rendered Element in
//    m.ui.frozen keeps the pre-mutation state forever, visible as a
//    permanently-Running spinner in scrollback;
//
//  • bytes still in streaming_text / pending_stream — the body is
//    still growing, so the snapshot would freeze a prefix while the
//    live tail re-renders the longer body, shifting every row below
//    (the duplication ghost).
//
// Refuse to freeze any run that isn't fully terminal AND fully
// settled. This makes the agent_session guarantee hold BY
// CONSTRUCTION rather than by caller discipline: even a mis-called
// freeze_through during an active stream stops at the run boundary
// (frozen_through parks at `i`; the settle-time freeze picks the run
// up once it's quiescent). Production callers are all idle-gated, so
// this gate never fires there — it exists to make the unsafe call
// impossible, mirroring how agent_session only ever freezes in its
// MessageStop handler.
bool run_is_freezable(const Model& m, std::size_t from, std::size_t run_end) {
    for (std::size_t j = from; j < run_end; ++j) {
        const Message& mm = m.d.current.messages[j];
        if (!mm.streaming_text.empty() || !mm.pending_stream.empty())
            return false;
        for (const auto& tc : mm.tool_calls) {
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
            push_frozen(m, compaction_divider_row(), 1, /*separator=*/true);
        }

        // Leading gap: one blank row before every turn except the
        // very first frozen row (avoid a top-of-thread gap).
        const bool first_overall = m.ui.frozen.empty();
        if (!first_overall) {
            push_frozen(m, gap_row(), kGapRows, /*separator=*/true);
        }

        const Message& head = m.d.current.messages[i];

        if (head.role == Role::Assistant) {
            int turn_num = m.ui.frozen_turn + 1;
            auto cfg = ui::turn_config_for_assistant_run(
                i, run_end, turn_num, m);

            // Hash key: settled assistant run — built through the shared
            // ui::assistant_run_hash_id so the live tail and the freeze
            // stamp a byte-identical key (cache HIT, zero row shift at
            // the freeze seam). Once frozen, none of the underlying
            // bytes change — the key is stable for the lifetime of this
            // entry.
            cfg.hash_id = ui::assistant_run_hash_id(m, i, run_end);
            push_frozen(m, maya::Turn{std::move(cfg)}.build(),
                        estimate_run_rows(m, i, run_end));
            ++m.ui.frozen_turn;
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

// NOTE: the mid-stream carve machinery that used to live here
// (freeze_settled_subturns, last_safe_block_split,
// freeze_streaming_text_prefix) has been DELETED, not just unwired.
// agent_session — the reference implementation with provably zero
// scrollback corruption — has exactly ONE freeze instant per turn
// (MessageStop). Every mid-stream carve stamped a frozen Turn whose
// hash maya's component cache had not seen on the previous live
// frame, forcing a cache-miss re-emit over committed scrollback —
// the corruption these functions repeatedly reintroduced each time
// someone re-wired them "for performance". Per-frame cost is bounded
// by maya's hash-keyed component cache on a STABLE live-tail hash
// (verified flat in o1_probe / long_session_bench), so there is
// nothing left for a carve to buy. Do not bring them back.

void ensure_frozen_width(Model& m, int term_cols) {
    if (term_cols < 1) return;                 // degenerate ioctl — keep stamps
    if (m.ui.frozen_cols == term_cols) return; // already current (common case)
    const int cols = measure_cols(term_cols);
    std::size_t total = 0;
    for (std::size_t k = 0; k < m.ui.frozen.size(); ++k) {
        std::size_t h = measure_element_rows(m.ui.frozen[k], cols);
        // A degenerate measure (0) keeps the prior stamp rather than
        // collapsing the entry to a wrong height.
        if (h < 1) h = static_cast<std::size_t>(std::max(1, m.ui.frozen_rows[k]));
        const std::size_t clamped = std::min<std::size_t>(
            h, static_cast<std::size_t>(std::numeric_limits<int>::max()));
        m.ui.frozen_rows[k] = static_cast<int>(clamped);
        total += clamped;
    }
    m.ui.frozen_row_total = total;             // re-derive the running sum
    m.ui.frozen_cols      = term_cols;
}

void clear_frozen(Model& m) {
    m.ui.frozen.clear();
    m.ui.frozen_rows.clear();
    m.ui.frozen_is_separator.clear();
    m.ui.frozen_row_total = 0;
    m.ui.frozen_cols    = 0;   // unstamped — next push re-stamps at live width
    m.ui.frozen_through = 0;
    m.ui.frozen_turn    = 0;
    m.ui.pending_settle_freeze   = false;
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

    // A mid-run cut (giant final run) can make the first frozen entry a
    // continuation whose own header was skipped, OR the run-boundary
    // gap can land first — either opens the canvas on a hole. Strip any
    // leading separator so the prefix opens on a real turn.
    drop_leading_separators(m);

    // Bound the cold-repaint cost: any OFF-SCREEN body taller than the whole
    // frozen budget is replaced with a one-row stub (fresh-paint only, so
    // scrollback-safe). The trailing entry — the current result — stays full.
    collapse_oversized_offscreen_entries(m);
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
    // Live-canvas row cap = frozen_row_budget() (~1.5 viewports of the
    // accurate row estimate; same budget rehydrate seeds to, so steady-
    // state and resume agree). Above it, the oldest
    // entries are dropped; maya's row diff sees a shorter live tree and
    // the already-overflowed rows commit to native scrollback. Bounding
    // the canvas to ~2 screens keeps EVERY full repaint cheap — the
    // per-frame tick, a resize (Divergent wipe+repaint), and Ctrl-L all
    // walk only ~1.5 screens instead of thousands of rows.
    // Re-measure the prefix to the live width if a resize moved it since the
    // counts were stamped. Both the keep-loop below and the commit count it
    // feeds read frozen_rows[]; a stale (post-widen) stamp would over-count
    // and over-commit, stranding a duplicate in scrollback. Do this FIRST so
    // every count this function reads reflects the wire.
    ensure_frozen_width(m, term_dims().cols);

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

    // EXACT COMMIT COUNT — a frozen-front trim is a TOP-DELETION, and the
    // only corruption-free commit for a top-deletion is EXACTLY the number
    // of rows removed. `pop_front_frozen` returns that number as the sum of
    // frozen_rows[] for the dropped entries — and frozen_rows[] is maya's
    // OWN measured render height at the live width (push_frozen measures
    // through the real layout engine at measure_cols(term_cols), and
    // ensure_frozen_width above healed any resize drift). So removed_rows is
    // byte-for-byte what maya physically painted and scrolled off; it is the
    // ground truth, not an estimate.
    //
    // Why EXACT and not the old 8192-width conservative UNDER-estimate:
    // commit_inline_prefix shifts maya's prev_cells up by the committed
    // count BEFORE the next render. If we commit FEWER than removed_rows,
    // prev_cells keeps (removed_rows - committed) stale top rows that the
    // dropped entries left behind — rows that no longer exist anywhere in
    // the new (shorter) canvas. The next render's prefix memcmp then
    // mismatches, and maya's overflow-recovery commits+repaints leaving
    // those stale physical rows STRANDED one screen up in native scrollback:
    // the exact "duplicate composer / turn one screen up" symptom. The
    // over-commit-duplicates fear that motivated the under-estimate was a
    // WIDTH-DRIFT artifact (freeze-time ioctl width ≠ maya's render width on
    // a mobile soft-keyboard); ensure_frozen_width + measured frozen_rows[]
    // now make removed_rows exact at the live width, so neither direction of
    // drift remains. Commit exactly what was dropped; maya's
    // scrollback_marker clamps it to prev_rows so an over-count is
    // structurally impossible at the maya boundary too.
    //
    // Drop the front in lockstep (frozen / frozen_rows /
    // frozen_is_separator / frozen_row_total), then strip any leading
    // separator the drop exposed — both count toward the exact commit.
    std::size_t removed_rows = pop_front_frozen(m, drop);
    removed_rows += pop_front_frozen_leading_separators(m);
    if (removed_rows == 0) return maya::Cmd<Msg>::none();

    return maya::Cmd<Msg>::commit_scrollback(
        static_cast<int>(std::min<std::size_t>(
            removed_rows,
            static_cast<std::size_t>(std::numeric_limits<int>::max()))));
}

} // namespace agentty::app::detail
