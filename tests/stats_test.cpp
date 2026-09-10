// stats_test — the fold, the epoch guard, and the tab table.
//
// The panel's whole value is that its numbers are TRUE. A stats view that
// can drift from what actually happened is worse than no view, because the
// user cannot tell which digits to trust. These tests pin the properties
// that make the numbers trustworthy:
//
//   • shares sum to 1 over their stated denominator (no rounding leak)
//   • the denominator is stated and consistent
//   • turns that did not run are not counted at all
//   • Smart-Mode-off turns are their own bucket, never folded into
//     Strategic — the split that makes delegated_share computable
//   • unmeasured turns are excluded from averages, not counted as zero
//   • the incremental fold agrees with a full rebuild, ALWAYS — including
//     across the three history-mutating operations
//
// Purity is why all of this is testable without a Model, a wire or a clock:
// the fold is a function of the transcript and nothing else.

#include "agtest.hpp"

#include "agentty/domain/stats/facts.hpp"
#include "agentty/domain/stats/tabs.hpp"
#include "agentty/domain/stats/unit.hpp"

#include <string>
#include <vector>

using agentty::Message;
using agentty::ModelId;
using agentty::Role;
using agentty::Thread;
using agentty::smart::ModelRole;
namespace st = agentty::stats;

namespace {

// An assistant turn that RAN: provenance stamped, as launch_stream does.
Message served(const char* model, ModelRole role) {
    Message m;
    m.role = Role::Assistant;
    m.served_model = ModelId{model};
    m.served_role = role;
    return m;
}

// An assistant turn that ran with Smart Mode OFF: a model but no role.
Message unrouted(const char* model) {
    Message m;
    m.role = Role::Assistant;
    m.served_model = ModelId{model};
    return m;
}

Message user_turn() {
    Message m;
    m.role = Role::User;
    m.text = "do the thing";
    return m;
}

// The in-flight placeholder: an assistant message that never dispatched.
Message placeholder() {
    Message m;
    m.role = Role::Assistant;
    return m;
}

Message with_usage(std::uint32_t in, std::uint32_t out,
                   std::uint32_t cache_r = 0, std::uint32_t cache_w = 0) {
    Message m = unrouted("m1");
    Message::Telemetry t;
    t.input_tokens  = in;
    t.output_tokens = out;
    t.cache_read     = cache_r;
    t.cache_creation = cache_w;
    t.ttft_ms   = 100;
    t.stream_ms = 900;
    m.telemetry = t;
    return m;
}

Thread thread_of(std::vector<Message> msgs) {
    Thread t;
    t.id = agentty::ThreadId{"t1"};
    t.messages = std::move(msgs);
    return t;
}

// A full rebuild, for the differential test below.
st::Facts rebuild(const Thread& t) {
    st::Projection p;
    return p.refresh(t);
}

[[nodiscard]] std::uint64_t count_of(const st::Tally& tally,
                                     std::string_view label) {
    for (const auto& [k, v] : tally.rows())
        if (k == label) return v;
    return 0;
}

}  // namespace

// ── The fold ─────────────────────────────────────────────────────────────

TEST_CASE("stats: an empty transcript has no turns and does not divide") {
    const auto f = rebuild(thread_of({}));
    CHECK(f.session.assistant_turns == 0);
    CHECK(f.smart.routed == 0);
    CHECK(f.smart.unrouted == 0);
    CHECK(f.models.by_model.empty());
}

TEST_CASE("stats: a turn that never ran is not counted") {
    // The in-flight placeholder is in the transcript from the moment the
    // user submits. Counting it would make every share wrong for the whole
    // duration of the turn the user is watching.
    const auto f = rebuild(thread_of({user_turn(), placeholder()}));
    CHECK(f.models.by_model.empty());
    CHECK(f.smart.routed == 0);
    // It IS an assistant turn, and the Session tab says so.
    CHECK(f.session.assistant_turns == 1);
    CHECK(f.session.user_turns == 1);
}

TEST_CASE("stats: routed and unrouted are separate buckets") {
    // THE property. A Smart-Mode-off turn has no role; counting it as
    // Strategic would overstate the flagship's share, and dropping it
    // would understate the denominator. It gets its own bucket.
    const auto f = rebuild(thread_of({
        served("sonnet", ModelRole::Strategic),
        served("haiku",  ModelRole::Utility),
        unrouted("gpt-5"),
    }));
    CHECK(f.smart.routed == 2);
    CHECK(f.smart.unrouted == 1);
    CHECK(f.smart.routed + f.smart.unrouted == f.session.assistant_turns);
    // The Models tab counts ALL THREE — it is descriptive, not a verdict.
    CHECK(f.models.by_model.total() == 3);
}

TEST_CASE("stats: delegated counts routed turns below Strategic") {
    const auto f = rebuild(thread_of({
        served("sonnet", ModelRole::Strategic),
        served("sonnet", ModelRole::Strategic),
        served("glm",    ModelRole::Implementation),
        served("haiku",  ModelRole::Utility),
        unrouted("gpt-5"),          // must NOT move the numerator or denominator
    }));
    CHECK(f.smart.routed == 4);
    CHECK(f.smart.delegated == 2);
}

TEST_CASE("stats: role shares sum to one over routed turns") {
    // No rounding leak: the parts are counts, and counts sum exactly.
    const auto f = rebuild(thread_of({
        served("a", ModelRole::Strategic),
        served("b", ModelRole::Implementation),
        served("c", ModelRole::Utility),
        unrouted("d"),
    }));
    CHECK(f.smart.by_role.total() == f.smart.routed);
}

TEST_CASE("stats: an all-Strategic session delegates nothing") {
    const auto f = rebuild(thread_of({
        served("sonnet", ModelRole::Strategic),
        served("sonnet", ModelRole::Strategic),
    }));
    CHECK(f.smart.routed == 2);
    CHECK(f.smart.delegated == 0);
}

TEST_CASE("stats: smart routing cards are bookkeeping, not turns") {
    // A routing card is a synthetic assistant message. Counting it would
    // inflate every denominator on the panel.
    Message card;
    card.role = Role::Assistant;
    card.smart_routing = true;
    const auto f = rebuild(thread_of({user_turn(), card,
                                      served("a", ModelRole::Strategic)}));
    CHECK(f.session.assistant_turns == 1);
    CHECK(f.smart.routed == 1);
}

TEST_CASE("stats: unmeasured turns stay out of the averages") {
    // A turn without telemetry is "not measured", which is different from
    // "measured as zero". Averaging fake zeroes in would understate every
    // per-turn figure on a thread with pre-telemetry history.
    const auto f = rebuild(thread_of({
        with_usage(1000, 500),
        unrouted("old"),            // no telemetry: predates the field
    }));
    CHECK(f.session.assistant_turns == 2);
    CHECK(f.session.measured_turns == 1);
    CHECK(f.tokens.output == 500);
}

TEST_CASE("stats: the cache split accounts for every prefix token") {
    // input_tokens EXCLUDES the cache fields on the wire, so the three
    // buckets partition the prefix with no arithmetic and none invented.
    const auto f = rebuild(thread_of({with_usage(300, 100, 600, 100)}));
    CHECK(f.cache.hits == 600);
    CHECK(f.cache.writes == 100);
    CHECK(f.cache.misses == 300);
    CHECK(f.context.peak_input == 1000);
    CHECK(f.cache.turns_with_cache == 1);
}

// ── The incremental fold ─────────────────────────────────────────────────

TEST_CASE("stats: incremental refresh equals a full rebuild") {
    // The core claim. If these ever disagree the panel shows numbers that
    // depend on WHEN you opened it.
    st::Projection p;
    Thread t = thread_of({});
    for (int i = 0; i < 12; ++i) {
        t.messages.push_back(user_turn());
        t.messages.push_back(i % 3 == 0 ? served("sonnet", ModelRole::Strategic)
                                        : with_usage(100, 50, 20));
        const auto& inc = p.refresh(t);
        const auto  full = rebuild(t);
        CHECK(inc.session.assistant_turns == full.session.assistant_turns);
        CHECK(inc.session.user_turns == full.session.user_turns);
        CHECK(inc.tokens.output == full.tokens.output);
        CHECK(inc.smart.routed == full.smart.routed);
        CHECK(inc.cache.hits == full.cache.hits);
    }
}

TEST_CASE("stats: the live tail is re-folded, never sealed") {
    // The bug in the projection this replaces: a {message_count, thread_id}
    // stamp cannot see the LAST message mutating, so a counter under it
    // freezes exactly while the user is watching it move.
    st::Projection p;
    Thread t = thread_of({user_turn(), with_usage(100, 10)});
    CHECK(p.refresh(t).tokens.output == 10);

    // Same message count — the stream just landed more tokens on it.
    t.messages.back().telemetry->output_tokens = 90;
    CHECK(p.refresh(t).tokens.output == 90);
}

TEST_CASE("stats: rewind rebuilds instead of keeping a stale prefix") {
    st::Projection p;
    Thread t = thread_of({with_usage(100, 10), with_usage(100, 20),
                          with_usage(100, 30)});
    CHECK(p.refresh(t).tokens.output == 60);

    t.messages.pop_back();          // rewind to a checkpoint
    t.messages.pop_back();
    CHECK(p.refresh(t).tokens.output == 10);
}

TEST_CASE("stats: switching threads rebuilds") {
    st::Projection p;
    Thread a = thread_of({with_usage(100, 10)});
    CHECK(p.refresh(a).tokens.output == 10);

    Thread b = thread_of({with_usage(100, 77)});
    b.id = agentty::ThreadId{"t2"};
    CHECK(p.refresh(b).tokens.output == 77);
}

TEST_CASE("stats: compaction rebuilds") {
    // Compaction REWRITES a prefix. Patching that incrementally is how a
    // fold starts producing numbers that are wrong and believed.
    st::Projection p;
    Thread t = thread_of({with_usage(100, 10), with_usage(100, 20)});
    CHECK(p.refresh(t).tokens.output == 30);

    t.compactions.push_back({});
    t.messages = {with_usage(100, 5)};
    CHECK(p.refresh(t).tokens.output == 5);
    CHECK(p.refresh(t).context.compactions == 1);
}

// ── Tally and Hist ───────────────────────────────────────────────────────

TEST_CASE("stats: a tally ranks by count and keeps ties stable") {
    st::Tally t;
    t.add("first", 5);
    t.add("second", 5);
    t.add("third", 9);
    const auto r = t.ranked();
    REQUIRE(r.size() == 3u);
    CHECK(r[0].first == "third");
    // Equal counts keep INSERTION order, so rows do not swap places
    // between frames while a stream runs.
    CHECK(r[1].first == "first");
    CHECK(r[2].first == "second");
}

TEST_CASE("stats: a histogram reports quantiles without storing samples") {
    st::Hist h;
    for (int i = 0; i < 100; ++i) h.add(i < 95 ? 10u : 5000u);
    CHECK(h.count() == 100);
    CHECK(h.max() == 5000);
    // p50 sits in the fast bucket, p95 in the slow one — which is the
    // whole point: a mean of 259ms would describe neither population.
    CHECK(h.quantile(0.5) <= 16.0);
    CHECK(h.quantile(0.95) >= 1024.0);
}

TEST_CASE("stats: quantiles separate within one octave") {
    // Plain log2 buckets double at every step, so a p50 and a p95 that
    // fall anywhere in the same octave report the SAME number — measured
    // on a real thread as "Median 4.1s, p95 4.1s", which reads as a bug
    // and tells the user nothing. Sub-buckets are what pull them apart.
    st::Hist h;
    for (int i = 0; i < 90; ++i) h.add(4200);    // just above 4096
    for (int i = 0; i < 10; ++i) h.add(7900);    // still under 8192
    CHECK(h.quantile(0.5) < h.quantile(0.95));
}

TEST_CASE("stats: a quantile never exceeds the observed maximum") {
    // A bucket's upper EDGE is the honest reading — the histogram does
    // not know where inside the bucket a sample sat — but an edge past
    // the maximum prints a p95 larger than the slowest call, a number
    // that cannot be true and that discredits every figure beside it.
    st::Hist h;
    h.add(10);
    h.add(5200);
    for (double q : {0.5, 0.9, 0.95, 0.99, 1.0})
        CHECK(h.quantile(q) <= static_cast<double>(h.max()));
}

// ── The tab table ────────────────────────────────────────────────────────

TEST_CASE("stats: tab metadata is derived from one table") {
    // kTabs is the SSOT. If a tab existed without a title or a subtitle,
    // the drift would show up here rather than as a blank strip chip.
    CHECK(st::kTabCount == static_cast<int>(st::kTabs.size()));
    for (const auto& d : st::kTabs) {
        CHECK(!d.title.empty());
        CHECK(!d.subtitle.empty());
        CHECK(d.available != nullptr);
        CHECK(!d.sections.empty());
        CHECK(st::tab_title(d.id) == d.title);
        CHECK(st::tab_subtitle(d.id) == d.subtitle);
    }
}

TEST_CASE("stats: a tab with nothing to say is not shown") {
    // An empty state is a tab admitting it should not have been drawn.
    const auto empty = rebuild(thread_of({}));
    const auto vis = st::visible_tabs(empty);
    // Session is always available; Tools/Cache/Smart are not.
    CHECK(std::find(vis.begin(), vis.end(), st::Tab::Session) != vis.end());
    CHECK(std::find(vis.begin(), vis.end(), st::Tab::Tools) == vis.end());
    CHECK(std::find(vis.begin(), vis.end(), st::Tab::Cache) == vis.end());
    CHECK(std::find(vis.begin(), vis.end(), st::Tab::Smart) == vis.end());
}

TEST_CASE("stats: Smart appears only once something was routed") {
    const auto off = rebuild(thread_of({unrouted("gpt-5")}));
    CHECK(!st::tab_available(st::Tab::Smart, off));

    const auto on = rebuild(thread_of({served("a", ModelRole::Utility)}));
    CHECK(st::tab_available(st::Tab::Smart, on));
}

TEST_CASE("stats: stepping never lands on a hidden tab") {
    // Tab/Shift-Tab walks the VISIBLE set, so a keypress cannot select a
    // view the strip is not drawing.
    const auto f = rebuild(thread_of({served("a", ModelRole::Utility)}));
    const auto vis = st::visible_tabs(f);
    st::Tab cur = vis.front();
    for (int i = 0; i < 3 * st::kTabCount; ++i) {
        cur = st::tab_step(cur, +1, f);
        CHECK(st::tab_available(cur, f));
    }
    for (int i = 0; i < 3 * st::kTabCount; ++i) {
        cur = st::tab_step(cur, -1, f);
        CHECK(st::tab_available(cur, f));
    }
}

TEST_CASE("stats: every extractor is total on an empty Facts") {
    // An extractor that divides by a zero denominator, or indexes an empty
    // tally, crashes the panel on the ONE session guaranteed to open it:
    // a brand-new thread.
    const st::Facts empty;
    std::vector<st::Metric> out;
    for (const auto& d : st::kTabs)
        for (const auto& sec : d.sections) {
            out.clear();
            sec.extract(empty, out);
            for (const auto& mt : out) {
                // Whatever a section chooses to emit must be finite and
                // formattable — NaN reaches the screen as "nan".
                CHECK(mt.value == mt.value);
                CHECK(!st::format(mt.unit, mt.value).empty());
            }
        }
}

// ── format() ─────────────────────────────────────────────────────────────

TEST_CASE("stats: format spells each unit one way") {
    using st::Unit;
    CHECK(st::format(Unit::Count, 47) == "47");
    CHECK(st::format(Unit::Tokens, 1200) == "1.2k");
    CHECK(st::format(Unit::Tokens, 12000) == "12k");
    CHECK(st::format(Unit::Tokens, 1'300'000) == "1.3M");
    CHECK(st::format(Unit::Millis, 340) == "340ms");
    CHECK(st::format(Unit::Millis, 3200) == "3.2s");
    CHECK(st::format(Unit::Millis, 214'000) == "3m34s");
    CHECK(st::format(Unit::Ratio, 0.62) == "62%");
    CHECK(st::format(Unit::Rate, 1200) == "1.2k/s");
}

TEST_CASE("stats: a non-zero share never prints as zero") {
    // Same rule as the bar that is not allowed to round to empty: "almost
    // none" and "none" are different readings, and a panel that prints the
    // first as the second is lying in the direction that matters.
    CHECK(st::format(st::Unit::Ratio, 0.004) == "<1%");
    CHECK(st::format(st::Unit::Ratio, 0.0) == "0%");
}
