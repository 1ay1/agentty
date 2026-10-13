// codeblock_update — reducer for the Ctrl+G code-block picker (the "run
// what the AI just suggested" flow). Open scans the newest assistant
// message for fenced blocks; Select suspends the TUI and runs the chosen
// block INTERACTIVELY on the real terminal — sudo password prompts work,
// output streams live — while a tee captures every output byte;
// RunFinished opens the Result card where the user decides what happens
// to the captured copy: attach to the composer as an Output chip, copy
// it clean, or discard.
//
// Deliberate scope decisions:
//   • Interactive-first: the run happens under maya's Cmd::suspend — the
//     TUI tears down to a cooked tty, the child inherits the REAL
//     terminal for stdin (sudo reads the password from /dev/tty, which
//     stays the real tty regardless of our stdout pipe), and stdout+
//     stderr flow through a tee pipe: every byte hits the user's screen
//     live AND lands in the capture buffer. Ctrl+C uses classic
//     system() semantics — the parent ignores SIGINT/SIGQUIT for the
//     duration, the child takes the default action and dies; agentty
//     resumes cleanly.
//   • The captured output only reaches the COMPOSER when the user
//     presses `a` on the Result card — it lands as an Output attachment
//     (chip), not a conversation message: the same collapse/expand
//     contract as a big paste. The user annotates and submits when (and
//     if) they want the model to see it.
//   • Only shell-ish blocks are runnable (see is_shell_language). For a
//     python/js block Select shows a toast; Edit / Copy still work.
//   • Opening is gated on an idle session: mid-stream the message list
//     is in flux and the "latest assistant reply" is still growing.
//   • Windows: no fork/tcsetattr — falls back to the non-interactive
//     captured runner (same one the bash tool uses). sudo isn't a
//     Windows concept anyway; the honest degradation.

#include "agentty/runtime/app/update/internal.hpp"

#include <algorithm>
#include <utility>


#include <maya/core/overload.hpp>
#include <maya/style/theme.hpp>

#include "agentty/runtime/panel/code_blocks.hpp"
#include "agentty/io/clipboard.hpp"
#include "agentty/runtime/app/cmd_code_block_run.hpp"

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

using maya::overload;
namespace cbp = agentty::code_blocks;

namespace {

// Newest assistant message that yields at least one fenced block.
// Document order within the message is preserved (block 1 = topmost),
// matching how the user visually indexes the reply on screen.
[[nodiscard]] std::vector<CodeBlock> latest_assistant_blocks(const Model& m) {
    for (auto it = m.d.current.messages.rbegin();
         it != m.d.current.messages.rend(); ++it) {
        if (it->role != Role::Assistant) continue;
        if (it->text.empty()) continue;
        auto blocks = cbp::extract_code_blocks(it->text);
        if (!blocks.empty()) return blocks;
        // Keep walking: an assistant turn with prose but no fences
        // shouldn't mask an earlier reply that HAS runnable blocks
        // (common shape: reply N has the commands, reply N+1 is a
        // short "let me know how it goes" follow-up).
    }
    return {};
}

// Blocks available RIGHT NOW from a still-streaming reply. The live text
// lands in `streaming_text` (settled prose moves to `text` only at turn end),
// so mid-turn we read whichever the live assistant message carries and use
// the CLOSED-only extractor: a block whose ``` fence hasn't arrived yet is
// still being typed and must not be offered (running a half-line is unsafe).
// `saw_open` reports that a block is mid-fence, so the caller can say "still
// streaming" instead of "no blocks".
[[nodiscard]] std::vector<CodeBlock>
streaming_reply_closed_blocks(const Model& m, bool& saw_open) {
    saw_open = false;
    for (auto it = m.d.current.messages.rbegin();
         it != m.d.current.messages.rend(); ++it) {
        if (it->role != Role::Assistant) continue;
        // Prefer the live streaming buffer; fall back to settled text (the
        // finalize may have already moved it while the phase lag clears).
        std::string_view live = !it->streaming_text.empty()
                                    ? std::string_view{it->streaming_text}
                                    : std::string_view{it->text};
        if (live.empty()) continue;
        bool open_here = false;
        auto blocks = cbp::extract_closed_code_blocks(live, &open_here);
        saw_open = saw_open || open_here;
        if (!blocks.empty()) return blocks;
    }
    return {};
}
// The block RUNNER (fork/exec on the real tty, or the Windows subprocess
// fallback) is an effect and lives in cmd_code_block_run.cpp. This file only
// decides which block runs and returns cmd::run_code_block(...).

} // namespace

Cmd codeblock_update(Model& m, msg::CodeBlockMsg cm) {
    return std::visit(overload{
        [&](OpenCodeBlocks) -> Cmd {
            // Mid-stream is allowed: the network stream runs on a background
            // worker (StreamDelta posts back to the UI thread), so suspending
            // the TUI to run a block does NOT pause the reply — deltas keep
            // arriving and land when you return. While streaming we offer the
            // blocks whose ``` fence has ALREADY closed in the live reply; a
            // block still being typed is withheld until its fence lands.
            std::vector<CodeBlock> blocks;
            if (m.s.active()) {
                bool saw_open = false;
                blocks = streaming_reply_closed_blocks(m, saw_open);
                if (blocks.empty()) {
                    auto cmd = set_status_toast(m,
                        saw_open
                          ? "a code block is still streaming \xe2\x80\x94 try again in a moment"
                          : "no complete code blocks yet");
                    return cmd;
                }
            } else {
                blocks = latest_assistant_blocks(m);
                if (blocks.empty()) {
                    auto cmd = set_status_toast(m,
                        "no code blocks in the last reply");
                    return cmd;
                }
            }
            m.ui.panel.descend(pn::CodeBlocks{{std::move(blocks), 0}});
            m.ui.code_blocks_scroll.y = 0;
            return Cmd::none();
        },
        [&](CloseCodeBlocks) -> Cmd {
            // Result overlay open → both alternatives die and we unwind to
            // the parent; the list alone → same. ascend() restores whatever
            // the picker was opened over (palette → ^K state intact).
            ascend(m);
            return Cmd::none();
        },
        [&](CodeBlocksMove& e) -> Cmd {
            if (auto* o = m.ui.panel.get<pn::CodeBlocks>()) {
                int sz = static_cast<int>(o->blocks.size());
                if (sz <= 0) return Cmd::none();
                o->index = std::clamp(o->index + e.delta, 0, sz - 1);
                return Cmd::none();
            }
            if (m.ui.panel.get<pn::CodeBlockResult>()) {
                // Read-only result card: Move deltas scroll the capture
                // viewport directly. max_y is paint-written-back by the
                // Picker widget; clamp against it (0 before first paint
                // — harmless, the writeback lands next frame).
                auto& sc = m.ui.code_blocks_scroll;
                sc.y = std::clamp(sc.y + e.delta, 0, std::max(0, sc.max_y));
            }
            return Cmd::none();
        },
        [&](CodeBlocksSelect& e) -> Cmd {
            auto* o = m.ui.panel.get<pn::CodeBlocks>();
            if (!o) return Cmd::none();
            const int idx = e.index.value_or(o->index);
            if (idx < 0 || idx >= static_cast<int>(o->blocks.size()))
                return Cmd::none();
            CodeBlock block = o->blocks[static_cast<std::size_t>(idx)];
            const cbp::BlockShell shell = cbp::shell_for_language(block.language);
            if (shell == cbp::BlockShell::None) {
                // Not runnable on this platform — nudge toward the actions
                // that DO make sense for this block. Picker stays open so
                // `e` / `y` are one keystroke away.
                const std::string tag = block.language.empty()
                    ? std::string{"this"} : "'" + block.language + "'";
                auto cmd = set_status_toast(m,
                    tag + " block isn't runnable here — "
                    "press e to edit or y to copy");
                return cmd;
            }
            m.ui.panel.close<pn::CodeBlocks>(); m.ui.panel.close<pn::CodeBlockResult>();
            return cmd::run_code_block(std::move(block.body), shell);
        },
        [&](CodeBlocksEdit) -> Cmd {
            auto* o = m.ui.panel.get<pn::CodeBlocks>();
            if (!o) return Cmd::none();
            const int idx = o->index;
            if (idx < 0 || idx >= static_cast<int>(o->blocks.size()))
                return Cmd::none();
            std::string body = o->blocks[static_cast<std::size_t>(idx)].body;
            m.ui.panel.close<pn::CodeBlocks>(); m.ui.panel.close<pn::CodeBlockResult>();
            // Splice at the cursor rather than replacing — same
            // convention as the @file / #symbol chips. The common case
            // is an empty composer, where this IS a replace.
            m.ui.composer.text.insert(
                static_cast<std::size_t>(m.ui.composer.cursor), body);
            m.ui.composer.cursor += static_cast<int>(body.size());
            return Cmd::none();
        },
        [&](CodeBlocksCopy) -> Cmd {
            auto* o = m.ui.panel.get<pn::CodeBlocks>();
            if (!o) return Cmd::none();
            const int idx = o->index;
            if (idx < 0 || idx >= static_cast<int>(o->blocks.size()))
                return Cmd::none();
            std::string body = o->blocks[static_cast<std::size_t>(idx)].body;
            m.ui.panel.close<pn::CodeBlocks>(); m.ui.panel.close<pn::CodeBlockResult>();
            // Write via native tooling (pbcopy/wl-copy/xclip) synchronously
            // AND emit the OSC 52 Cmd. OSC 52 is opt-in in Terminal.app /
            // iTerm2 so it silently no-ops there; the native write is what
            // actually lands the copy locally. OSC 52 still carries it across
            // an SSH/tmux hop to a remote clipboard.
            (void)write_clipboard_text(body);
            auto toast = set_status_toast(m, "copied clean block to clipboard");
            return Cmd::batch(
                        cmd::write_clipboard(std::move(body)),
                        std::move(toast));
        },
        [&](CodeBlockRunFinished& e) -> Cmd {
            // Don't auto-stage — open the RESULT card instead. The user
            // already watched the output live on the real terminal; this
            // is the decision beat: attach the captured copy to the
            // composer, copy it clean, or discard. The composer only
            // ever receives output the user explicitly asked for.
            // descend(), not assignment: the run finishes ASYNC — the user
            // may have another panel open by now, and the result card must
            // not destroy it. Esc from the card restores it verbatim.
            m.ui.panel.descend(pn::CodeBlockResult{{
                std::move(e.command), std::move(e.output),
                e.exit_code, e.timed_out}});
            m.ui.code_blocks_scroll.y = 0;
            return Cmd::none();
        },
        [&](CodeBlockResultAttach) -> Cmd {
            auto* r = m.ui.panel.get<pn::CodeBlockResult>();
            if (!r) return Cmd::none();
            // Fold the captured output into the composer as an Output
            // attachment — the SAME collapse-to-chip / expand-on-submit
            // machinery a big paste uses. However huge the log is, the
            // composer shows one pill ("Output: sudo mkfs… · 1240 lines
            // · 48 KB"); the whole body only materialises on the wire
            // when the user actually submits.
            std::string out = std::move(r->output);
            std::size_t lines = out.empty() ? 0 : 1;
            for (char c : out) if (c == '\n') ++lines;

            Attachment att;
            att.kind       = Attachment::Kind::Output;
            att.name       = std::move(r->command);   // chip caption
            att.line_count = lines;
            att.byte_count = out.size();
            att.body.set_bytes(std::move(out));
            std::size_t idx = m.ui.composer.attachments.size();
            m.ui.composer.attachments.push_back(std::move(att));
            auto placeholder = attachment::make_placeholder(idx);
            m.ui.composer.text.insert(
                static_cast<std::size_t>(m.ui.composer.cursor), placeholder);
            m.ui.composer.cursor += static_cast<int>(placeholder.size());
            m.ui.composer.expanded = true;
            m.ui.panel.close<pn::CodeBlocks>(); m.ui.panel.close<pn::CodeBlockResult>();
            auto toast = set_status_toast(m, "output attached to composer");
            return toast;
        },
        [&](CodeBlockResultCopy) -> Cmd {
            auto* r = m.ui.panel.get<pn::CodeBlockResult>();
            if (!r) return Cmd::none();
            std::string body = std::move(r->output);
            m.ui.panel.close<pn::CodeBlocks>(); m.ui.panel.close<pn::CodeBlockResult>();
            (void)write_clipboard_text(body);   // native pbcopy/wl-copy/xclip
            auto toast = set_status_toast(m, "output copied to clipboard");
            return Cmd::batch(
                        cmd::write_clipboard(std::move(body)),
                        std::move(toast));
        },
        [&](CodeBlockResultDiscard) -> Cmd {
            m.ui.panel.close<pn::CodeBlocks>(); m.ui.panel.close<pn::CodeBlockResult>();
            return Cmd::none();
        },
    }, cm);
}

} // namespace agentty::app::detail
