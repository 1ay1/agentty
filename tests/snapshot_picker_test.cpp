// SPDX-License-Identifier: Apache-2.0
//
// snapshot_picker_test.cpp — locks the two primitives that replaced the
// hand-copied picker plumbing:
//
//   util::Snapshot      — closes the use-after-free that list_workspace_symbols
//                         had (it copied the owning shared_ptr into a
//                         block-scoped local and returned a reference that
//                         outlived it) without paying files.cpp's deep copy.
//
//   ui::FilteredPicker  — closes the cold-open refill hole: the refill used to
//                         live in ONE message arm behind an `entries.empty()`
//                         guard, so a picker opened before the index published
//                         and then ARROWED (not typed into) never filled, and
//                         once the query was non-empty the guard could never
//                         fire again.

#include "agentty/runtime/panel/filtered_picker.hpp"
#include "agentty/util/snapshot.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using agentty::ui::FilteredPicker;
using agentty::ui::SnapshotSource;
using agentty::util::Snapshot;
using agentty::util::make_snapshot;

namespace {

// Substring filter, enough to exercise the memo without pulling in the real
// fuzzy scorer.
std::vector<std::size_t> substr_filter(const std::vector<std::string>& v,
                                       std::string_view q) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < v.size(); ++i)
        if (q.empty() || v[i].find(q) != std::string::npos) out.push_back(i);
    return out;
}

} // namespace

TEST_CASE("Snapshot: a default snapshot is empty, not null-and-dangerous") {
    Snapshot<std::vector<std::string>> s;
    CHECK(!s.has_value());
    CHECK(s.empty());
    CHECK(s.size() == 0);
    // Every accessor is well-defined on an unpublished snapshot — "the walk
    // hasn't landed yet" needs no special-casing at any call site.
    CHECK(s.get().empty());
    for ([[maybe_unused]] const auto& x : s) FAIL("empty snapshot must not iterate");
}

TEST_CASE("Snapshot: a reader keeps its buffer alive across a republish") {
    // THE use-after-free, directly. A reader holds a snapshot; the cache then
    // publishes a whole new value. The reader's view must remain valid and
    // unchanged — under the old const-ref contract this was a dangling read.
    auto published = make_snapshot(std::vector<std::string>{"a", "b", "c"});

    const auto reader = published;         // O(1) refcount bump, not a copy
    published = make_snapshot(std::vector<std::string>{"z"});   // republish

    REQUIRE(reader.size() == 3);
    CHECK(reader[0] == "a");
    CHECK(reader[2] == "c");
    CHECK(published.size() == 1);
    CHECK(published[0] == "z");
}

TEST_CASE("Snapshot: has_value and empty are different questions") {
    // "published but genuinely empty workspace" vs "still indexing" — the two
    // render different hints, and conflating them is what made the cold-open
    // path invisible.
    const auto indexing = Snapshot<std::vector<std::string>>{};
    const auto empty_ws = make_snapshot(std::vector<std::string>{});

    CHECK(!indexing.has_value());
    CHECK(indexing.empty());

    CHECK(empty_ws.has_value());
    CHECK(empty_ws.empty());
}

TEST_CASE("FilteredPicker: refills when the index publishes LATE") {
    // The regression. Open cold, then only ever ARROW — never type. The old
    // code refilled solely in the "printable typed" arm, so this picker stayed
    // on "indexing…" forever.
    bool ready = false;
    std::vector<std::string> backing{"alpha.cpp", "beta.cpp", "gamma.hpp"};

    FilteredPicker<std::string> p{
        SnapshotSource<std::string>{
            .ready = [&] { return ready; },
            .fetch = [&] { return make_snapshot(backing); },
        },
        substr_filter,
    };

    // Cold: nothing to show, and the picker says so.
    CHECK(!p.source_ready());
    CHECK(p.entries().empty());
    CHECK(p.selected() == nullptr);

    // The background walk lands. No message is delivered to the picker.
    ready = true;

    // A pure cursor move — the arm that used to skip the refill entirely.
    p.move(1);

    CHECK(p.source_ready());
    REQUIRE(p.entries().size() == 3);
    REQUIRE(p.selected() != nullptr);
    CHECK(*p.selected() == "beta.cpp");
}

TEST_CASE("FilteredPicker: refills even when the query is already non-empty") {
    // The second half of the same bug: the old guard was
    // `if (entries.empty())`, so once a query had been typed the refill could
    // never fire again and the picker kept a partial list for its whole life.
    bool ready = false;
    std::vector<std::string> backing{"alpha.cpp", "beta.cpp"};

    FilteredPicker<std::string> p{
        SnapshotSource<std::string>{
            .ready = [&] { return ready; },
            .fetch = [&] { return make_snapshot(backing); },
        },
        substr_filter,
    };

    p.type(U'a');                 // typed while still cold
    CHECK(p.filtered().empty());

    ready = true;
    CHECK(p.entries().size() == 2);
    CHECK(p.filtered().size() == 2);   // both contain 'a'
}

TEST_CASE("FilteredPicker: the cursor cannot point outside the filtered list") {
    FilteredPicker<std::string> p{
        SnapshotSource<std::string>{
            .ready = [] { return true; },
            .fetch = [] { return make_snapshot(std::vector<std::string>{
                              "alpha", "beta", "gamma"}); },
        },
        substr_filter,
    };

    p.move(100);                  // walk far past the end
    CHECK(p.index() == 2);
    REQUIRE(p.selected() != nullptr);
    CHECK(*p.selected() == "gamma");

    p.move(-100);
    CHECK(p.index() == 0);

    // Narrowing the list must pull the cursor back in range, not leave it
    // pointing at a row that no longer exists.
    p.move(2);
    CHECK(p.index() == 2);
    p.type(U'b');                 // filters down to just "beta"
    CHECK(p.filtered().size() == 1);
    CHECK(p.index() == 0);
    REQUIRE(p.selected() != nullptr);
    CHECK(*p.selected() == "beta");
}

TEST_CASE("FilteredPicker: typing resets the cursor to the top") {
    FilteredPicker<std::string> p{
        SnapshotSource<std::string>{
            .ready = [] { return true; },
            .fetch = [] { return make_snapshot(std::vector<std::string>{
                              "aa", "ab", "ac"}); },
        },
        substr_filter,
    };
    p.move(2);
    CHECK(p.index() == 2);
    p.type(U'a');
    CHECK(p.index() == 0);        // re-filtering makes the old row meaningless
}

TEST_CASE("FilteredPicker: backspace reports whether anything was erased") {
    // The `@` and Ctrl+K pickers use this to mean "backspace on an empty
    // query closes me". Returning it keeps the test and the mutation atomic
    // instead of two statements that can drift apart.
    FilteredPicker<std::string> p{
        SnapshotSource<std::string>{
            .ready = [] { return true; },
            .fetch = [] { return make_snapshot(std::vector<std::string>{"x"}); },
        },
        substr_filter,
    };

    CHECK(!p.backspace());        // empty query → "nothing to erase" → close
    p.type(U'x');
    CHECK(p.query() == "x");
    CHECK(p.backspace());         // erased
    CHECK(p.query().empty());
    CHECK(!p.backspace());        // empty again
}

TEST_CASE("FilteredPicker: backspace pops a whole UTF-8 code point") {
    // Popping one byte off a multi-byte sequence leaves an invalid string
    // that the filter then scans.
    FilteredPicker<std::string> p{
        SnapshotSource<std::string>{
            .ready = [] { return true; },
            .fetch = [] { return make_snapshot(std::vector<std::string>{"x"}); },
        },
        substr_filter,
    };
    p.type(U'é');                 // 2 bytes
    CHECK(p.query().size() == 2);
    CHECK(p.backspace());
    CHECK(p.query().empty());     // not one dangling continuation byte
}

TEST_CASE("FilteredPicker: the filter runs once per (query, snapshot)") {
    // The memo. Reducer and view both read per keystroke; without this the
    // scan ran two or three times for one key.
    int calls = 0;
    FilteredPicker<std::string> p{
        SnapshotSource<std::string>{
            .ready = [] { return true; },
            .fetch = [] { return make_snapshot(std::vector<std::string>{
                              "alpha", "beta"}); },
        },
        [&](const std::vector<std::string>& v, std::string_view q) {
            ++calls;
            return substr_filter(v, q);
        },
    };

    p.filtered();
    p.filtered();
    p.filtered();
    CHECK(calls == 1);            // three reads, one pass

    p.type(U'a');
    p.filtered();
    p.filtered();
    CHECK(calls == 2);            // query changed once → exactly one more pass
}

TEST_CASE("FilteredPicker: selected() folds the empty and out-of-range cases") {
    FilteredPicker<std::string> p{
        SnapshotSource<std::string>{
            .ready = [] { return true; },
            .fetch = [] { return make_snapshot(std::vector<std::string>{"alpha"}); },
        },
        substr_filter,
    };
    REQUIRE(p.selected() != nullptr);
    p.type(U'z');                 // matches nothing
    CHECK(p.filtered().empty());
    CHECK(p.selected() == nullptr);   // the only failure mode, and it's typed
}
