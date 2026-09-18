// picker_cursor_visible_test — the invariant every list overlay owes its user:
//
//     THE HIGHLIGHTED ROW IS ON SCREEN.
//
// This is the test that would have caught the theme-browser bug, and it is
// written against the PRIMITIVE rather than against one panel, so it holds
// for every picker that uses FilteredPicker rather than only the one that
// broke.
//
// The bug it pins: the theme browser kept `index` and `scroll` as two loose
// fields. The reducer advanced the index; nothing ever advanced the scroll;
// the view passed a THIRD scroll object belonging to the pane behind it. So
// the cursor walked past the bottom of a 615-row list and the user was left
// steering a selection they could not see — Down appeared dead, and Enter
// committed a scheme from far below the last visible row.
//
// FilteredPicker::visible() closes that by DERIVING the window from the
// cursor instead of storing it. There is no offset to forget to advance, so
// the property below is not merely true today, it is true by construction.

#include <cstdio>
#include <string>
#include <vector>

#include "agentty/runtime/panel/filtered_picker.hpp"
#include "agentty/util/snapshot.hpp"

using namespace agentty;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

// A picker over N synthetic rows. N is deliberately far larger than any
// viewport, because that is the regime the bug lived in.
ui::FilteredPicker<std::string> make_picker(int n) {
    std::vector<std::string> rows;
    rows.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) rows.push_back("row-" + std::to_string(i));

    auto snap = util::make_snapshot(std::move(rows));
    ui::SnapshotSource<std::string> src{
        .ready = [] { return true; },
        .fetch = [snap] { return snap; },
    };
    ui::FilterFn<std::string> filter =
        [](const std::vector<std::string>& entries, std::string_view q) {
            std::vector<std::size_t> out;
            for (std::size_t i = 0; i < entries.size(); ++i)
                if (q.empty() || entries[i].find(q) != std::string_view::npos)
                    out.push_back(i);
            return out;
        };
    return ui::FilteredPicker<std::string>{std::move(src), std::move(filter)};
}

// THE invariant, as a predicate: whatever the cursor is, the window we would
// render contains it, and the entry at that offset is the selected entry.
bool cursor_is_visible(const ui::FilteredPicker<std::string>& p, int vh) {
    const auto w   = p.visible(vh);
    const auto ent = p.visible_entries(vh);
    if (w.total == 0) return w.cursor == -1 && ent.empty();

    if (w.cursor < 0) return false;                        // must name a row
    if (static_cast<std::size_t>(w.cursor) >= ent.size()) return false;
    if (ent.size() != w.count) return false;

    // The row the window points at must BE the selection. This is the part
    // that catches an off-by-one window: a slice can contain the cursor's
    // index while the offset arithmetic still names the wrong row.
    const std::string* sel = p.selected();
    return sel != nullptr && *ent[static_cast<std::size_t>(w.cursor)] == *sel;
}

} // namespace

int main() {
    std::printf("=== picker_cursor_visible_test ===\n\n");

    // ── 1. Walking the whole list, one step at a time ────────────────────
    // The exact gesture from the bug report: hold Down and watch. Every
    // single position must be renderable, not just the first screen.
    {
        auto p = make_picker(615);          // the real scheme count
        const int vh = 8;
        int offscreen = 0;
        for (int i = 0; i < 615; ++i) {
            if (!cursor_is_visible(p, vh)) ++offscreen;
            p.move_wrapping(1);
        }
        std::printf("615 rows, viewport 8 — every cursor position\n");
        std::printf("    positions where the highlight was off screen: %d / 615\n",
                    offscreen);
        check(offscreen == 0, "the highlight is never off screen");
        std::printf("\n");
    }

    // ── 2. Wrapping ─────────────────────────────────────────────────────
    // Wrapping is where a stored offset goes most obviously wrong: the
    // cursor jumps to the far end and a scroll that only ever increments
    // is left pointing at the other side of the list.
    {
        auto p = make_picker(615);
        const int vh = 8;
        p.move_wrapping(-1);                 // Up from row 0 → last row
        const auto w = p.visible(vh);
        std::printf("wrap past the top\n");
        std::printf("    cursor landed in window [%zu, %zu) of %zu\n",
                    w.first, w.first + w.count, w.total);
        check(cursor_is_visible(p, vh), "wrapping keeps the highlight visible");
        check(w.scrolled_above(), "the window followed the cursor to the end");
        std::printf("\n");
    }

    // ── 3. Filtering ────────────────────────────────────────────────────
    // Narrowing changes what row N means. A cursor left where it was would
    // point into a list that no longer exists.
    {
        auto p = make_picker(615);
        const int vh = 8;
        p.move_wrapping(400);                // deep into the list
        p.type("row-61");                    // collapse to a handful
        std::printf("filtering after scrolling deep\n");
        std::printf("    filtered to %zu rows, cursor=%d\n",
                    p.filtered().size(), p.index());
        check(cursor_is_visible(p, vh), "a narrowed list re-homes the highlight");

        p.clear_query();                     // widen again
        check(cursor_is_visible(p, vh), "widening keeps it visible too");
        std::printf("\n");
    }

    // ── 4. Viewport sizes, including degenerate ones ────────────────────
    // A panel's viewport is computed from the terminal; a tiny window and a
    // window larger than the list are both reachable by resizing.
    {
        std::printf("assorted viewport heights\n");
        int bad = 0;
        for (int vh : {1, 2, 3, 5, 8, 13, 40, 1000}) {
            auto p = make_picker(615);
            for (int step = 0; step < 50; ++step) {
                p.move_wrapping(13);         // a stride coprime-ish with vh
                if (!cursor_is_visible(p, vh)) { ++bad; break; }
            }
        }
        check(bad == 0, "holds at every viewport height");
        std::printf("\n");
    }

    // ── 5. The window never runs off the end ────────────────────────────
    // A window that slides past the last row renders blank strips below a
    // list that has more rows above — the "half empty picker" symptom.
    {
        auto p = make_picker(100);
        const int vh = 8;
        int overrun = 0;
        for (int i = 0; i < 100; ++i) {
            const auto w = p.visible(vh);
            if (w.first + w.count > w.total) ++overrun;
            p.move_wrapping(1);
        }
        check(overrun == 0, "the window never extends past the last row");
        std::printf("\n");
    }

    // ── 6. Short lists ──────────────────────────────────────────────────
    // Fewer rows than the viewport must not scroll at all, and an empty
    // list must report no cursor rather than row 0.
    {
        auto p = make_picker(3);
        const auto w = p.visible(8);
        check(w.first == 0 && w.count == 3, "a short list fills from the top");
        check(!w.scrolled_above() && !w.scrolled_below(),
              "a short list reports no scroll affordances");
        check(cursor_is_visible(p, 8), "short lists satisfy the invariant too");

        auto e = make_picker(0);
        const auto ew = e.visible(8);
        check(ew.cursor == -1 && ew.empty(),
              "an empty list names no row (not row 0)");
        std::printf("\n");
    }

    if (failures == 0) {
        std::printf("PASS — the highlighted row is always on screen\n");
        return 0;
    }
    std::printf("FAILED: %d check(s)\n", failures);
    return 1;
}
