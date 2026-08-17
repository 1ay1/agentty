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
