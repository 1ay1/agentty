// compaction_wire_test — the compaction WIRE SUBSTITUTION and its
// interaction with fork provenance (cmd_factory: wire_messages_for /
// estimate_wire_tokens).
//
// compaction_threshold_test covers WHEN compaction fires; this covers WHAT
// the wire looks like afterwards, and that fork + compaction compose:
//
//   1. No compaction → wire is the raw transcript, unchanged.
//   2. A CompactionRecord replaces [0, up_to_index) with ONE synthetic User
//      summary message; the tail [up_to_index, end) ships raw.
//   3. The wire ALWAYS starts with a User (Anthropic requirement) — the
//      synthetic summary is a User.
//   4. estimate_wire_tokens counts the substituted (smaller) view, not the
//      raw transcript — otherwise auto-compaction would re-fire forever.
//   5. A malformed record (up_to_index 0 or > size) degrades to the raw
//      transcript instead of corrupting the wire.
//   6. FORK + COMPACTION compose: a forked thread's fork_note (a real User
//      message at head) ships on the wire uncompacted; and when a fork is
//      later compacted, the summary subsumes the fork_note prefix and the
//      wire still starts with a User.

#include "agtest.hpp"

#include "agentty/runtime/app/cmd_factory.hpp"
#include "agentty/domain/conversation.hpp"

#include <string>

using namespace agentty;


static Message umsg(std::string t) {
    Message m; m.role = Role::User; m.text = std::move(t); return m;
}
static Message amsg(std::string t) {
    Message m; m.role = Role::Assistant; m.text = std::move(t); return m;
}

TEST_CASE("compaction wire") {
    // ── 1: no compaction → raw transcript verbatim ──
    {
        Thread t;
        t.messages = { umsg("q1"), amsg("a1"), umsg("q2"), amsg("a2") };
        auto wire = app::cmd::wire_messages_for(t);
        check(wire.size() == 4, "no compaction: all messages ship");
        check(wire[0].text == "q1" && wire[3].text == "a2",
              "no compaction: verbatim order preserved");
    }

    // ── 2+3: a CompactionRecord substitutes the prefix ──
    {
        Thread t;
        t.messages = { umsg("q1"), amsg("a1"), umsg("q2"), amsg("a2"),
                       umsg("q3"), amsg("a3") };
        Thread::CompactionRecord rec;
        rec.up_to_index = 4;                 // summarize [0,4): q1..a2
        rec.summary     = "user asked q1/q2; assistant answered a1/a2";
        t.compactions.push_back(rec);

        auto wire = app::cmd::wire_messages_for(t);
        // 1 summary + tail (q3, a3) = 3
        check(wire.size() == 3, "compaction: prefix collapses to summary + tail");
        check(wire.front().role == Role::User,
              "compaction: wire starts with a User (Anthropic invariant)");
        check(wire.front().is_compact_summary,
              "compaction: head is flagged is_compact_summary");
        check(wire.front().text.find("q1/q2") != std::string::npos,
              "compaction: summary text carried into the head message");
        check(wire[1].text == "q3" && wire[2].text == "a3",
              "compaction: raw tail after the boundary preserved");
    }

    // ── 4: estimate_wire_tokens prices the substituted view ──
    {
        Thread t;
        // A big prefix that a compaction shrinks dramatically.
        for (int i = 0; i < 40; ++i)
            t.messages.push_back(i % 2 ? amsg(std::string(4000, 'a'))
                                       : umsg(std::string(4000, 'q')));
        const int raw = app::cmd::estimate_wire_tokens(t);

        Thread::CompactionRecord rec;
        rec.up_to_index = 38;                // collapse almost everything
        rec.summary     = "short recap";
        t.compactions.push_back(rec);
        const int compacted = app::cmd::estimate_wire_tokens(t);

        check(compacted < raw / 2,
              "estimate: compacted wire is much cheaper than raw "
              "(raw=" + std::to_string(raw) + " compacted="
              + std::to_string(compacted) + ")");
    }

    // ── 5: malformed record degrades to raw ──
    {
        Thread t;
        t.messages = { umsg("q1"), amsg("a1") };
        Thread::CompactionRecord bad;
        bad.up_to_index = 99;                // > size → malformed
        bad.summary = "ignored";
        t.compactions.push_back(bad);
        auto wire = app::cmd::wire_messages_for(t);
        check(wire.size() == 2 && wire[0].text == "q1",
              "malformed record (index > size): raw transcript sent");

        Thread t2;
        t2.messages = { umsg("q1"), amsg("a1") };
        Thread::CompactionRecord zero;
        zero.up_to_index = 0;                // 0 → nothing to summarize
        zero.summary = "ignored";
        t2.compactions.push_back(zero);
        auto wire2 = app::cmd::wire_messages_for(t2);
        check(wire2.size() == 2 && wire2[0].text == "q1",
              "malformed record (index 0): raw transcript sent");
    }

    // ── 6a: fork_note ships uncompacted on the wire ──
    {
        Thread t;
        Message note;
        note.role = Role::User;
        note.fork_note = true;
        note.fork_transcript = "/tmp/parent.transcript.md";
        note.text = "This conversation is a fork; transcript at /tmp/parent…";
        t.messages.push_back(std::move(note));
        t.messages.push_back(umsg("first real question"));

        auto wire = app::cmd::wire_messages_for(t);
        check(wire.size() == 2, "fork: note + prompt both ship (no compaction)");
        check(wire.front().role == Role::User && wire.front().fork_note,
              "fork: the fork_note is the wire head and a User (provider-proof)");
        check(wire[1].text == "first real question",
              "fork: the real prompt follows the note");
    }

    // ── 6b: compacting a fork subsumes the fork_note; wire still User-first ──
    {
        Thread t;
        Message note;
        note.role = Role::User;
        note.fork_note = true;
        note.text = "fork pointer";
        t.messages.push_back(std::move(note));
        t.messages.push_back(umsg("q1"));
        t.messages.push_back(amsg("a1"));
        t.messages.push_back(umsg("q2"));
        t.messages.push_back(amsg("a2"));

        Thread::CompactionRecord rec;
        rec.up_to_index = 3;                 // subsume note + q1 + a1
        rec.summary = "forked thread; user asked q1, got a1";
        t.compactions.push_back(rec);

        auto wire = app::cmd::wire_messages_for(t);
        check(wire.front().role == Role::User,
              "fork+compaction: wire still starts with a User");
        check(wire.front().is_compact_summary,
              "fork+compaction: head is the summary (subsumed the fork_note)");
        check(!wire.front().fork_note,
              "fork+compaction: the summary head is NOT flagged fork_note");
        check(wire.back().text == "a2",
              "fork+compaction: raw tail after the boundary preserved");
    }
}

TEST_CASE("compaction reclaim is measured") {
    // A compaction's whole job is to make the next request fit. Nothing
    // recorded whether it DID.
    //
    // The dangerous case is not a compaction that fails loudly — it is one
    // that reclaims almost nothing. It looks identical in the transcript:
    // a summary appears, the turn continues. Then the next turn is still
    // over threshold, compacts again, reclaims nothing again, and the
    // thread livelocks summarising itself while the user watches it do
    // nothing. That is only visible as a NUMBER, which is why GitHub's
    // runtime carries the same pre/post pair.
    //
    // Both sides must come from the SAME scorer or the difference is
    // noise, so this pins that the pair is measured with
    // cmd::estimate_wire_tokens — the compaction-aware one.
    Thread t;
    t.messages.push_back(umsg(std::string(20000, 'q')));   // a big prefix
    t.messages.push_back(amsg(std::string(20000, 'a')));
    t.messages.push_back(umsg("and now a short tail"));

    const int before = app::cmd::estimate_wire_tokens(t);

    Thread::CompactionRecord rec;
    rec.up_to_index   = 2;              // subsume the two big messages
    rec.summary       = "user asked a long thing; assistant answered";
    rec.tokens_before = before;
    t.compactions.push_back(rec);
    t.compactions.back().tokens_after = app::cmd::estimate_wire_tokens(t);

    const auto& r = t.compactions.back();

    // The measurement is real: a big prefix replaced by one short summary
    // has to shrink the wire substantially.
    check(r.tokens_after < r.tokens_before,
          "compaction reclaim: after is smaller than before");
    check(r.reclaimed().has_value(),
          "compaction reclaim: a measured record reports a reclaim");
    check(*r.reclaimed() > 1000,
          "compaction reclaim: subsuming ~40KB reclaims real tokens");

    // And it agrees with the estimator the auto-compaction trigger reads,
    // which is the only reason before/after can be compared at all.
    check(r.tokens_after == app::cmd::estimate_wire_tokens(t),
          "compaction reclaim: after matches the live wire estimate");
}

TEST_CASE("an unmeasured compaction reports unknown, not zero") {
    // Records written before the measurement existed reload with 0/0.
    // Zero reclaim and "we never measured" want OPPOSITE reactions — the
    // first means compaction is broken and the thread is about to
    // livelock, the second means we simply don't know. Conflating them
    // would have an old thread reporting a disaster that never happened.
    Thread t;
    t.messages.push_back(umsg("q"));
    t.messages.push_back(amsg("a"));

    Thread::CompactionRecord rec;
    rec.up_to_index = 1;
    rec.summary     = "old record, written before tokens were tracked";
    t.compactions.push_back(rec);

    check(!t.compactions.back().reclaimed().has_value(),
          "unmeasured compaction reports nullopt rather than 0");

    // A half-measured record is also unknown: one side alone cannot
    // produce a difference, and guessing the other would be inventing data.
    t.compactions.back().tokens_before = 5000;
    check(!t.compactions.back().reclaimed().has_value(),
          "half-measured compaction is still unknown");
}
