// composer_edit_test — reducer-level coverage for the composer's
// word-wise delete keys (Ctrl+W / Alt+D), added alongside the existing
// kill-line + word-motion family. Drives detail::composer_update
// directly (no maya, no I/O) and asserts the buffer + cursor land where
// readline's unix-word-rubout / kill-word would put them, including the
// chip-aware boundary that removes a whole attachment token in one
// stroke.

#include <string>

#include "agtest.hpp"

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/composer_attachment.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"

using agentty::Model;
using agentty::app::detail::composer_update;
namespace msg = agentty::msg;
// The composer message structs live in the agentty:: namespace; the
// msg::ComposerMsg variant aliases them. Pull the ones we drive in.
using agentty::ComposerDeleteWordBack;
using agentty::ComposerDeleteWordForward;
using agentty::ComposerUndo;
using agentty::ComposerCharInput;
using agentty::ComposerCursorWordLeft;
using agentty::ComposerCursorWordRight;
using agentty::ComposerKillToEndOfLine;

namespace {

void check_edit(const char* name, bool cond, const std::string& got = {}) {
    CHECK_MESSAGE(cond, name << " — got: [" << got << "]");
}

// Run one ComposerMsg through the reducer, return the resulting model.
Model step(Model m, msg::ComposerMsg cm) {
    return composer_update(std::move(m), std::move(cm)).first;
}

Model with_text(std::string text, int cursor) {
    Model m;
    m.ui.composer.text   = std::move(text);
    m.ui.composer.cursor = cursor;
    return m;
}
} // namespace

TEST_CASE("composer_edit") {
    // ── Ctrl+W: delete word backward ──────────────────────────────────
    {
        // cursor at end → drop the last word ("baz") + its leading space.
        auto m = step(with_text("foo bar baz", 11), ComposerDeleteWordBack{});
        check_edit("ctrl-w drops trailing word", m.ui.composer.text == "foo bar ",
              m.ui.composer.text);
        check_edit("ctrl-w cursor follows", m.ui.composer.cursor == 8,
              std::to_string(m.ui.composer.cursor));
    }
    {
        // cursor mid-buffer (after "bar ") → delete "bar " back to "foo ".
        auto m = step(with_text("foo bar baz", 8), ComposerDeleteWordBack{});
        check_edit("ctrl-w mid-buffer", m.ui.composer.text == "foo baz",
              m.ui.composer.text);
        check_edit("ctrl-w mid cursor", m.ui.composer.cursor == 4,
              std::to_string(m.ui.composer.cursor));
    }
    {
        // cursor at 0 → no-op, no crash.
        auto m = step(with_text("foo", 0), ComposerDeleteWordBack{});
        check_edit("ctrl-w at start no-op", m.ui.composer.text == "foo" &&
              m.ui.composer.cursor == 0, m.ui.composer.text);
    }
    {
        // empty buffer → no-op.
        auto m = step(with_text("", 0), ComposerDeleteWordBack{});
        check_edit("ctrl-w empty no-op", m.ui.composer.text.empty() &&
              m.ui.composer.cursor == 0, m.ui.composer.text);
    }

    // ── Alt+D: delete word forward ────────────────────────────────────
    {
        // cursor at 0 → drop the first word ("foo") + the gap after it.
        auto m = step(with_text("foo bar baz", 0), ComposerDeleteWordForward{});
        check_edit("alt-d drops leading word", m.ui.composer.text == "bar baz",
              m.ui.composer.text);
        check_edit("alt-d cursor stays", m.ui.composer.cursor == 0,
              std::to_string(m.ui.composer.cursor));
    }
    {
        // cursor at end → no-op.
        auto m = step(with_text("foo bar", 7), ComposerDeleteWordForward{});
        check_edit("alt-d at end no-op", m.ui.composer.text == "foo bar" &&
              m.ui.composer.cursor == 7, m.ui.composer.text);
    }

    // ── Chip-aware: a Ctrl+W at the right edge of an attachment chip
    //    removes the whole placeholder token in one stroke. ────────────
    {
        Model m;
        // Build "see <CHIP>" where CHIP is a real placeholder for att[0].
        agentty::Attachment att;
        att.kind       = agentty::Attachment::Kind::Paste;
        att.line_count = 3;
        att.byte_count = 42;
        att.body       = "x\ny\nz";
        m.ui.composer.attachments.push_back(std::move(att));
        std::string ph = agentty::attachment::make_placeholder(0);
        m.ui.composer.text   = "see " + ph;
        m.ui.composer.cursor = static_cast<int>(m.ui.composer.text.size());

        m = step(std::move(m), ComposerDeleteWordBack{});
        // The chip token is gone; "see " (or "see") remains, chip removed.
        const bool chip_gone =
            m.ui.composer.text.find(static_cast<char>(0x01)) == std::string::npos;
        check_edit("ctrl-w removes whole chip token", chip_gone, m.ui.composer.text);
    }

    // ── Undo restores ctrl-w. ─────────────────────────────────────────
    {
        auto m = step(with_text("foo bar baz", 11), ComposerDeleteWordBack{});
        m = step(std::move(m), ComposerUndo{});
        check_edit("undo restores ctrl-w", m.ui.composer.text == "foo bar baz",
              m.ui.composer.text);
    }

    // ── Undo coalescing: a run of typed chars is ONE undo unit; the
    //    first whitespace starts a fresh unit so undo rewinds word by
    //    word, not char by char. ───────────────────────────────────────
    {
        Model m;  // start empty
        for (char c : std::string("hello"))
            m = step(std::move(m), ComposerCharInput{static_cast<char32_t>(c)});
        check_edit("typed run present", m.ui.composer.text == "hello",
              m.ui.composer.text);
        // One undo should wipe the WHOLE coalesced word, back to empty.
        m = step(std::move(m), ComposerUndo{});
        check_edit("one undo rewinds the whole typed word",
              m.ui.composer.text.empty(), m.ui.composer.text);
    }
    {
        // "foo bar": space breaks the run, so undo peels "bar", leaving
        // "foo " (the space snapshot).
        Model m;
        for (char c : std::string("foo bar"))
            m = step(std::move(m), ComposerCharInput{static_cast<char32_t>(c)});
        m = step(std::move(m), ComposerUndo{});
        check_edit("undo peels one word across a space boundary",
              m.ui.composer.text == "foo ", m.ui.composer.text);
    }

    // ── Word motion consumes a RUN of punctuation as one unit. ─────────
    {
        // cursor after "))))" → word-left jumps the whole run at once.
        auto m = step(with_text("a))))", 5), ComposerCursorWordLeft{});
        check_edit("word-left eats punctuation run", m.ui.composer.cursor == 1,
              std::to_string(m.ui.composer.cursor));
    }
    {
        // cursor before "((((" → word-right jumps the whole run.
        auto m = step(with_text("((((a", 0), ComposerCursorWordRight{});
        check_edit("word-right eats punctuation run", m.ui.composer.cursor == 4,
              std::to_string(m.ui.composer.cursor));
    }

    // ── Queue-peek reset: editing while peeking a queued slot must drop
    //    the peek so submit doesn't delete the wrong queue entry. ───────
    {
        Model m;
        m.ui.composer.queued.push_back({"queued msg", {}});
        m.ui.composer.browsing = agentty::ComposerState::QueuePeek{0};   // pretend we Alt+↑'d
        m.ui.composer.text = "queued msg";
        m.ui.composer.cursor = 10;
        m = step(std::move(m), ComposerCharInput{U'!'});
        check_edit("typing while peeking returns the composer to Live",
              m.ui.composer.is_live(),
              m.ui.composer.queue_peek_index()
                  ? "still peeking " + std::to_string(*m.ui.composer.queue_peek_index())
                  : "history");
    }

    // ── Kill-to-end-of-line (Alt+K — reachable again after the Ctrl+K
    //    global-palette clash). ───────────────────────────────────────
    {
        auto m = step(with_text("foo bar baz", 4), ComposerKillToEndOfLine{});
        check_edit("kill-to-end deletes from cursor to EOL",
              m.ui.composer.text == "foo ", m.ui.composer.text);
    }
    {
        // Multi-line: kill-to-end stops at the newline, keeping it.
        auto m = step(with_text("foo\nbar", 1), ComposerKillToEndOfLine{});
        check_edit("kill-to-end stops at newline", m.ui.composer.text == "f\nbar",
              m.ui.composer.text);
    }
    {
        // At EOL already — no-op.
        auto m = step(with_text("foo", 3), ComposerKillToEndOfLine{});
        check_edit("kill-to-end at EOL is a no-op", m.ui.composer.text == "foo",
              m.ui.composer.text);
    }

    // ── Loop mode (^B) ───────────────────────────────────────────────
    // ARMING submits immediately (that's the point of ^B: "send this, and
    // keep sending it"), which needs the deps() seam — out of scope for this
    // pure-editing test. What IS pinned here is every transition the reducer
    // owns without touching the network: the empty-payload refusal, and the
    // disarm that must fully clear the snapshot.
    {
        // An EMPTY composer cannot arm a loop — a loop with no payload would
        // never fire, so the state is refused rather than entered. (No submit
        // happens on this path, so no deps() are needed.)
        auto m = step(with_text("", 0), agentty::ComposerToggleLoop{});
        check_edit("^B on an empty composer does not arm",
                   !m.ui.composer.loop_armed, "armed with no payload");
        check_edit("empty arm leaves looping() false",
                   !m.ui.composer.looping(), "looping with no payload");
    }
    {
        // Disarm clears EVERYTHING. A stale snapshot left behind would make
        // a later re-arm silently repeat the wrong prompt, and a stale count
        // would render ⟳ ×N for a loop that never ran.
        Model m;
        m.ui.composer.text            = "live text";
        m.ui.composer.loop_armed      = true;
        m.ui.composer.loop_text       = "run the tests";
        m.ui.composer.loop_iterations = 3;
        check_edit("precondition: looping", m.ui.composer.looping(), "not looping");

        auto off = step(std::move(m), agentty::ComposerToggleLoop{});
        check_edit("^B disarms", !off.ui.composer.loop_armed, "still armed");
        check_edit("disarm clears the snapshot",
                   off.ui.composer.loop_text.empty(), off.ui.composer.loop_text);
        check_edit("disarm resets the counter",
                   off.ui.composer.loop_iterations == 0,
                   std::to_string(off.ui.composer.loop_iterations));
        check_edit("disarm leaves looping() false",
                   !off.ui.composer.looping(), "still looping");
        check_edit("disarm does not touch the live composer text",
                   off.ui.composer.text == "live text", off.ui.composer.text);
    }
}
