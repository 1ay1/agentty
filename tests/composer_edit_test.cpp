// composer_edit_test — reducer-level coverage for the composer's
// word-wise delete keys (Ctrl+W / Alt+D), added alongside the existing
// kill-line + word-motion family. Drives detail::composer_update
// directly (no maya, no I/O) and asserts the buffer + cursor land where
// readline's unix-word-rubout / kill-word would put them, including the
// chip-aware boundary that removes a whole attachment token in one
// stroke.

#include <cstdio>
#include <string>

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

namespace {
int failures = 0, total = 0;

void check(const char* name, bool cond, const std::string& got = {}) {
    ++total;
    if (cond) std::printf("  ok   %s\n", name);
    else { ++failures; std::printf("  FAIL %s — got: [%s]\n", name, got.c_str()); }
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

int main() {
    std::printf("composer_edit_test — Ctrl+W / Alt+D word delete\n\n");

    // ── Ctrl+W: delete word backward ──────────────────────────────────
    {
        // cursor at end → drop the last word ("baz") + its leading space.
        auto m = step(with_text("foo bar baz", 11), ComposerDeleteWordBack{});
        check("ctrl-w drops trailing word", m.ui.composer.text == "foo bar ",
              m.ui.composer.text);
        check("ctrl-w cursor follows", m.ui.composer.cursor == 8,
              std::to_string(m.ui.composer.cursor));
    }
    {
        // cursor mid-buffer (after "bar ") → delete "bar " back to "foo ".
        auto m = step(with_text("foo bar baz", 8), ComposerDeleteWordBack{});
        check("ctrl-w mid-buffer", m.ui.composer.text == "foo baz",
              m.ui.composer.text);
        check("ctrl-w mid cursor", m.ui.composer.cursor == 4,
              std::to_string(m.ui.composer.cursor));
    }
    {
        // cursor at 0 → no-op, no crash.
        auto m = step(with_text("foo", 0), ComposerDeleteWordBack{});
        check("ctrl-w at start no-op", m.ui.composer.text == "foo" &&
              m.ui.composer.cursor == 0, m.ui.composer.text);
    }
    {
        // empty buffer → no-op.
        auto m = step(with_text("", 0), ComposerDeleteWordBack{});
        check("ctrl-w empty no-op", m.ui.composer.text.empty() &&
              m.ui.composer.cursor == 0, m.ui.composer.text);
    }

    // ── Alt+D: delete word forward ────────────────────────────────────
    {
        // cursor at 0 → drop the first word ("foo") + the gap after it.
        auto m = step(with_text("foo bar baz", 0), ComposerDeleteWordForward{});
        check("alt-d drops leading word", m.ui.composer.text == "bar baz",
              m.ui.composer.text);
        check("alt-d cursor stays", m.ui.composer.cursor == 0,
              std::to_string(m.ui.composer.cursor));
    }
    {
        // cursor at end → no-op.
        auto m = step(with_text("foo bar", 7), ComposerDeleteWordForward{});
        check("alt-d at end no-op", m.ui.composer.text == "foo bar" &&
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
        check("ctrl-w removes whole chip token", chip_gone, m.ui.composer.text);
    }

    // ── Undo restores ctrl-w. ─────────────────────────────────────────
    {
        auto m = step(with_text("foo bar baz", 11), ComposerDeleteWordBack{});
        m = step(std::move(m), ComposerUndo{});
        check("undo restores ctrl-w", m.ui.composer.text == "foo bar baz",
              m.ui.composer.text);
    }

    // ── Undo coalescing: a run of typed chars is ONE undo unit; the
    //    first whitespace starts a fresh unit so undo rewinds word by
    //    word, not char by char. ───────────────────────────────────────
    {
        Model m;  // start empty
        for (char c : std::string("hello"))
            m = step(std::move(m), ComposerCharInput{static_cast<char32_t>(c)});
        check("typed run present", m.ui.composer.text == "hello",
              m.ui.composer.text);
        // One undo should wipe the WHOLE coalesced word, back to empty.
        m = step(std::move(m), ComposerUndo{});
        check("one undo rewinds the whole typed word",
              m.ui.composer.text.empty(), m.ui.composer.text);
    }
    {
        // "foo bar": space breaks the run, so undo peels "bar", leaving
        // "foo " (the space snapshot).
        Model m;
        for (char c : std::string("foo bar"))
            m = step(std::move(m), ComposerCharInput{static_cast<char32_t>(c)});
        m = step(std::move(m), ComposerUndo{});
        check("undo peels one word across a space boundary",
              m.ui.composer.text == "foo ", m.ui.composer.text);
    }

    // ── Word motion consumes a RUN of punctuation as one unit. ─────────
    {
        // cursor after "))))" → word-left jumps the whole run at once.
        auto m = step(with_text("a))))", 5), ComposerCursorWordLeft{});
        check("word-left eats punctuation run", m.ui.composer.cursor == 1,
              std::to_string(m.ui.composer.cursor));
    }
    {
        // cursor before "((((" → word-right jumps the whole run.
        auto m = step(with_text("((((a", 0), ComposerCursorWordRight{});
        check("word-right eats punctuation run", m.ui.composer.cursor == 4,
              std::to_string(m.ui.composer.cursor));
    }

    // ── Queue-peek reset: editing while peeking a queued slot must drop
    //    the peek so submit doesn't delete the wrong queue entry. ───────
    {
        Model m;
        m.ui.composer.queued.push_back({"queued msg", {}});
        m.ui.composer.queue_peek_idx = 0;      // pretend we Alt+↑'d
        m.ui.composer.text = "queued msg";
        m.ui.composer.cursor = 10;
        m = step(std::move(m), ComposerCharInput{U'!'});
        check("typing while peeking drops queue_peek_idx",
              m.ui.composer.queue_peek_idx == -1,
              std::to_string(m.ui.composer.queue_peek_idx));
    }

    std::printf("\n%d/%d checks passed\n", total - failures, total);
    return failures == 0 ? 0 : 1;
}
