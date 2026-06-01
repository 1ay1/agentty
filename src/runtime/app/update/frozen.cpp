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

namespace agentty::app::detail {

namespace {

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
// to a frozen Turn. NOT a render — a coarse proxy (avg ~60 cols/row)
// used only to BOUND the frozen canvas height, where over/under by a
// few rows is harmless. Shared by rehydrate_frozen (budget walk) and
// freeze_range (per-entry frozen_rows accounting).
// Cheap, non-allocating estimate of a JSON value's rendered byte
// footprint. Walks the node tree summing string lengths + a small
// constant per scalar/structural node. Used ONLY to bound the frozen
// canvas height, so precision doesn't matter — but it must be cheap:
// the previous version called j.dump(), which allocated a full
// serialized copy of the args for EVERY tool call on EVERY freeze. On
// a thread with large write/edit args that was megabytes of JSON
// serialization per resume (rehydrate_frozen freezes the whole tail)
// and per user turn (freeze_through) — the dominant cost of opening an
// old long thread. This walk allocates nothing.
std::size_t estimate_json_bytes(const nlohmann::json& j) {
    switch (j.type()) {
        case nlohmann::json::value_t::string:
            return j.get_ref<const std::string&>().size();
        case nlohmann::json::value_t::array: {
            std::size_t n = 2;  // brackets
            for (const auto& e : j) n += estimate_json_bytes(e) + 1;
            return n;
        }
        case nlohmann::json::value_t::object: {
            std::size_t n = 2;  // braces
            for (const auto& [k, v] : j.items())
                n += k.size() + 2 + estimate_json_bytes(v) + 1;
            return n;
        }
        case nlohmann::json::value_t::null:
            return 0;
        default:
            return 8;  // number / bool: a few bytes
    }
}

std::size_t estimate_msg_rows(const Message& mm) {
    std::size_t bytes = mm.text.size() + mm.streaming_text.size();
    for (const auto& tc : mm.tool_calls) {
        bytes += tc.output().size();
        bytes += tc.args_streaming.size();
        // The RENDERED body of a settled tool card comes from its
        // ARGS, not its output: a write card shows args["content"]
        // (the whole new file, show_all), an edit card shows every
        // hunk's old/new text under args["edits"], a read/grep card
        // shows its result text. tc.output() is only the one-line
        // "wrote N lines" footer. Counting just output() under-
        // estimated a 3000-line write as ~1 row, so the row cap never
        // tripped and the canvas ballooned to thousands of rows while
        // frozen_row_total still read tiny. Approximate the body by a
        // cheap (non-allocating) walk of the args JSON — NOT dump(),
        // which allocated a full serialized copy per freeze and made
        // resuming a long thread slow.
        if (!tc.args.is_null()) {
            bytes += estimate_json_bytes(tc.args);
        }
        // Header / footer / chrome rows per tool card (~4 rows even
        // for an empty body — title, divider, status, blank).
        bytes += 4 * 60;
    }
    // Per-message envelope (header, gap, divider).
    bytes += 3 * 60;
    return bytes / 60 + 1;
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
        // Freeze a sub-turn only if it has at least one tool and all of
        // them are terminal. A text-only or empty sub-turn (the active
        // streaming placeholder, or the tail the model is writing into)
        // stays live.
        if (!terminal_tools) break;
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

    // Bounded rehydrate. Two costs scale with the rehydrated tail:
    //
    //   1. Build cost: markdown parse + tool body preview + timeline
    //      layout, paid once per frozen Element at rehydrate time.
    //   2. Paint cost on the swap frame: every canvas row gets emitted
    //      to the wire row-by-row with \r\n between, and each \r\n at
    //      the viewport bottom edge scrolls the terminal one row. Rows
    //      that scroll past the top of the viewport during this paint
    //      are NOT in native scrollback (they were never live in this
    //      session) — pure wasted bytes that the user sees as the
    //      "paint from top, fast-scroll to bottom" lag on thread
    //      resume.
    //
    // Row budget = current terminal rows − composer reserve. Fitting
    // the rehydrated tail inside ONE viewport eliminates the visible
    // scroll animation entirely: maya's case-A (Fresh) compose path
    // emits every canvas row from row 0, and any row past the viewport
    // bottom triggers a \r\n scroll. When total_rows ≤ term_h, no row
    // scrolls — the screen paints in place. Older turns live in the
    // on-disk JSON (m.d.current.messages is intact); they're just
    // invisible inside agentty until the next live append shifts the
    // window. Composer history (↑ in the composer) still walks every
    // prior user prompt, so the recall path is unaffected.
    constexpr std::size_t kRehydrateTurns = 6;
    const auto term_size = maya::platform::query_terminal_size(
        maya::platform::stdout_handle());
    // Reserve ~6 rows for the composer + status line so the rehydrated
    // transcript leaves room for chrome without forcing a scroll.
    constexpr int kComposerReserve = 6;
    const std::size_t kRehydrateRowBudget = static_cast<std::size_t>(
        std::max(8, term_size.height.value - kComposerReserve));

    // Walk backward counting speaker-runs until EITHER cap trips.
    // A long auto-pilot Assistant run can be hundreds of rows in a
    // SINGLE run; including it whole would blow the one-viewport budget
    // (the resume paint emits every frozen row to the wire, and rows
    // past the viewport bottom scroll — the visible "render from top"
    // lag). So when a single run already exceeds the budget, cut INSIDE
    // it at sub-turn granularity: keep only the trailing sub-turns that
    // fit. freeze_range renders that partial run with a header (it's
    // the start of the rehydrated window); the elided leading sub-turns
    // live in the on-disk JSON and the terminal's own scrollback.
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
        // Estimate rows for THIS run before committing to include it.
        std::size_t run_rows = 0;
        for (std::size_t k = j; k < cursor; ++k)
            run_rows += estimate_msg_rows(msgs[k]);

        // If this is the FIRST (newest) run and it ALONE overflows the
        // budget, trim it from the front at sub-turn granularity so the
        // resume paint still fits ~one viewport. Walk its sub-turns
        // newest-first, keeping until the budget is met (always keep
        // >=1 sub-turn).
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
            break;   // budget spent inside this run
        }

        // Always include at least one run — even a giant one is what
        // the user just loaded and wants to see. Stop AFTER the run
        // that pushes us over budget so the most-recent context is
        // always visible.
        ++units;
        start = j;
        row_budget += run_rows;
        if (units >= kRehydrateTurns) break;
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
    constexpr std::size_t kFrozenMaxRows = 1500;
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

} // namespace agentty::app::detail
