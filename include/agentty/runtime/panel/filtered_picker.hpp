#pragma once
// agentty::ui::FilteredPicker<Entry> — the shared state of every
// "snapshot + query + cursor" overlay.
//
// ── The design flaw this closes ──────────────────────────────────────────
//
// Three overlays are the same thing: `@` (files), `#` (symbols), `Ctrl+K`
// (commands). Each independently hand-wrote the same three mechanisms, and
// the hand-copies drifted in three different directions:
//
//   1. THE FILTER MEMO. filter_*() is O(N × query) and gets called two or
//      three times per keystroke (reducer clamps the cursor, reducer resolves
//      the selection, view draws the rows). mention.hpp and symbol.hpp each
//      grew an identical `cached_matches / cached_query / cached_valid` trio
//      plus an identical accessor. The palette never got one, so it re-filters
//      twice per key. Same bug, solved twice, missed once.
//
//   2. THE SNAPSHOT REFILL. A picker may open COLD (background index not
//      published yet) with an empty snapshot, and must refill when the index
//      lands. That refill was written into exactly ONE message arm — the
//      "printable typed" arm:
//
//          if (o->files.empty() && files_ready()) o->files = list_workspace_files();
//
//      So a user who opened `@` cold and pressed Backspace or ↓ instead of
//      typing never refilled, and the panel sat on "indexing…" forever. Worse:
//      once the query was non-empty the `empty()` guard could never fire
//      again, so a cold-opened picker kept whatever partial list it grabbed on
//      keystroke one for the rest of its life.
//
//      The flaw is that the refill was attached to an EVENT (one of ~6 arms)
//      when it is really a property of READING the snapshot. Every arm already
//      funnels through the accessor; putting it there makes "refill on access"
//      total by construction instead of true in one of six branches.
//
//   3. THE CURSOR CLAMP. Every arm re-derived `clamp(index, 0, size-1)` by
//      hand against a count it had to remember to recompute after filtering.
//
// FilteredPicker owns all three. An overlay declares its entry type and its
// filter function; it inherits a correct memo, a total refill, and a cursor
// that cannot point outside the filtered list.

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "agentty/runtime/visual.hpp"
#include "agentty/util/snapshot.hpp"

namespace agentty::ui {

// How a picker obtains (and re-obtains) its candidate set.
//
// `ready` and `fetch` are separate because the picker must distinguish
// "published but genuinely empty workspace" from "still indexing" — the two
// render different hints, and a single nullable snapshot cannot say which.
template <class Entry>
struct SnapshotSource {
    std::function<bool()>                              ready;
    std::function<util::Snapshot<std::vector<Entry>>()> fetch;
};

// The filter: entries + query → indices into entries, best match first.
template <class Entry>
using FilterFn =
    std::function<std::vector<std::size_t>(const std::vector<Entry>&, std::string_view)>;

template <class Entry>
class FilteredPicker {
public:
    FilteredPicker() = default;

    FilteredPicker(SnapshotSource<Entry> source, FilterFn<Entry> filter)
        : source_(std::move(source)), filter_(std::move(filter)) {}

    // ── Query mutation ───────────────────────────────────────────────────
    // Each of these invalidates the memo and resets the cursor in ONE place,
    // so no arm can mutate the query and forget one of the two.

    void type(char32_t ch) {
        // Only ASCII printables reach here today (the key router filters),
        // but the encode is UTF-8-safe regardless.
        if (ch < 0x20) return;
        if (ch < 0x80) query_.push_back(static_cast<char>(ch));
        else           append_utf8(ch);
        on_query_changed();
    }

    void type(std::string_view text) {
        if (text.empty()) return;
        query_.append(text);
        on_query_changed();
    }

    // Returns false when the query was already empty — the signal the `@` and
    // Ctrl+K pickers use to mean "backspace on an empty query closes me".
    // Returning it (rather than each caller re-testing `query().empty()`
    // before calling) keeps the test and the mutation atomic.
    bool backspace() {
        if (query_.empty()) return false;
        // Pop a whole UTF-8 code point, not a byte: popping one byte off a
        // multi-byte sequence leaves an invalid string that the filter then
        // scans.
        while (!query_.empty()
               && (static_cast<unsigned char>(query_.back()) & 0xC0) == 0x80)
            query_.pop_back();
        if (!query_.empty()) query_.pop_back();
        on_query_changed();
        return true;
    }

    void clear_query() {
        if (query_.empty()) return;
        query_.clear();
        on_query_changed();
    }

    [[nodiscard]] const std::string& query() const noexcept { return query_; }

    // ── Cursor ───────────────────────────────────────────────────────────
    // Clamped against the FILTERED count, computed here, so an overlay can no
    // longer move the cursor against a stale or unfiltered size.

    void move(int delta) const {
        const auto n = static_cast<int>(filtered().size());
        if (n <= 0) { index_ = 0; return; }
        index_ = std::clamp(index_ + delta, 0, n - 1);
    }

    // Wrapping variant, for the pickers whose lists are long enough that
    // walking back to the top should not take N keystrokes.
    void move_wrapping(int delta) const {
        const auto n = static_cast<int>(filtered().size());
        if (n <= 0) { index_ = 0; return; }
        index_ = ((index_ + delta) % n + n) % n;
    }

    void jump_to(int i) const {
        const auto n = static_cast<int>(filtered().size());
        index_ = n <= 0 ? 0 : std::clamp(i, 0, n - 1);
    }

    [[nodiscard]] int index() const noexcept {
        const auto n = static_cast<int>(filtered().size());
        // Clamp on READ as well as on write: the snapshot can grow underneath
        // a stored cursor when a cold-opened picker refills.
        if (n <= 0) return 0;
        return std::clamp(index_, 0, n - 1);
    }

    // The currently highlighted entry, or nullptr when the list is empty.
    // Returning a pointer (not a reference + a separate empty() check) makes
    // the empty case impossible to skip.
    [[nodiscard]] const Entry* selected() const {
        const auto& idx = filtered();
        if (idx.empty()) return nullptr;
        const auto i = static_cast<std::size_t>(index());
        if (i >= idx.size()) return nullptr;
        return &entries()[idx[i]];
    }

    // ── Reading the candidate set ────────────────────────────────────────

    // THE accessor. Every read goes through here, which is exactly why the
    // refill lives here: a picker that opened before the background index
    // published tops itself up on the next read, whatever caused that read —
    // a keystroke, an arrow, a resize, a repaint. The old code attached this
    // to one message arm and so missed five.
    [[nodiscard]] const std::vector<Entry>& entries() const {
        if (!snapshot_.has_value() && source_.ready && source_.fetch && source_.ready()) {
            snapshot_ = source_.fetch();
            memo_valid_ = false;   // a bigger snapshot means new match indices
        }
        return snapshot_.get();
    }

    // Memoised filter result: indices into entries(), best first. Recomputed
    // only when the query or the snapshot actually changed, so the two or
    // three reads a single keystroke performs cost one pass, not three.
    [[nodiscard]] const std::vector<std::size_t>& filtered() const {
        const auto& src = entries();   // may refill, may invalidate the memo
        if (!memo_valid_ || memo_query_ != query_) {
            memo_ = filter_ ? filter_(src, query_) : identity_indices(src.size());
            memo_query_ = query_;
            memo_valid_ = true;
        }
        return memo_;
    }

    // True once the backing index has published. Distinct from
    // `filtered().empty()`: the picker's footer says "indexing…" for the
    // former and "workspace empty" for the latter, and conflating them is how
    // the cold-open path became invisible.
    [[nodiscard]] bool source_ready() const {
        return snapshot_.has_value() || (source_.ready && source_.ready());
    }

    // Force a refill on the next read — for an explicit "the index changed"
    // signal (a workspace switch, a post-tool-edit refresh).
    void invalidate() const {
        snapshot_   = {};
        memo_valid_ = false;
    }

private:
    void on_query_changed() const {
        memo_valid_ = false;
        index_      = 0;
    }

    void append_utf8(char32_t cp) {
        if (cp < 0x800) {
            query_.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            query_.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            query_.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            query_.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            query_.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            query_.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            query_.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            query_.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            query_.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    static std::vector<std::size_t> identity_indices(std::size_t n) {
        std::vector<std::size_t> v(n);
        for (std::size_t i = 0; i < n; ++i) v[i] = i;
        return v;
    }

    SnapshotSource<Entry> source_;
    FilterFn<Entry>       filter_;
    std::string           query_;

    // `mutable` for the same reason the old hand-rolled memos were: the view
    // takes `const Model&` and must be able to read (and therefore populate)
    // the cache. Logical constness — every mutable member here is a cache or
    // a clamp of state that is already determined by query_ + snapshot_.
    mutable util::Snapshot<std::vector<Entry>> snapshot_;
    mutable std::vector<std::size_t>           memo_;
    mutable std::string                        memo_query_;
    mutable bool                               memo_valid_ = false;
    mutable int                                index_      = 0;

    // ── The frame-hash contract ──────────────────────────────────────────
    // visual.hpp's walk brace-decomposes types to hash them, which it cannot
    // do here (private members, user-provided constructors). Rather than
    // letting that fail open, the walk fails CLOSED and demands an explicit
    // parts list — so the two visible axes are named here, deliberately:
    //
    //   query_  the text on screen.
    //   index_  the highlighted row.
    //
    // Everything else is a PROJECTION of those two plus a background
    // snapshot: memo_/memo_query_/memo_valid_ are a pure cache of
    // filter(snapshot, query), and hashing a cache would make the gate
    // depend on whether a repaint happened to populate it. `snapshot_` is
    // exempt for a subtler reason — it is republished by a background task,
    // so hashing it would let a thread outside the update loop move the
    // frame gate. The picker's cold-open refill is instead surfaced through
    // `index()`/`filtered()` being recomputed on read, and the tick that is
    // already running while an index warms is what carries it to screen.
    friend auto visual_parts(const FilteredPicker& p) {
        return std::make_tuple(std::cref(p.query_), p.index_);
    }
};

} // namespace agentty::ui

namespace agentty::visual {
// The reviewed claim, in the namespace that owns the concept.
//
// FilteredPicker is a non-aggregate (private members + a user-provided
// constructor), so the brace-arity probe reports 0 and the completeness
// proof cannot count its members. That fails CLOSED by design, which is why
// this opt-in exists: it is a deliberate, greppable statement that a human
// checked the parts list. The check: of the picker's eight members, exactly
// two are visible state (query_, index_) and six are cache or plumbing
// (source_, filter_, snapshot_, memo_, memo_query_, memo_valid_). See the
// friend visual_parts in filtered_picker.hpp for why each is excluded.
template <class Entry>
inline constexpr bool trusted_parts<ui::FilteredPicker<Entry>> = true;
} // namespace agentty::visual

namespace agentty::ui {
static_assert(visual::parts_cover_all<FilteredPicker<std::string>>);
} // namespace agentty::ui
