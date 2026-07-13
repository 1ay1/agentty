#include "agentty/runtime/app/subscribe.hpp"

#include <chrono>
#include <cstdlib>
#include <optional>
#include <variant>

#include <maya/terminal/ansi.hpp>

#include "agentty/runtime/login.hpp"
#include "agentty/runtime/picker.hpp"
#include "agentty/runtime/app/update/internal.hpp"

namespace agentty::app {

using maya::Sub;
using maya::KeyEvent;
using maya::CharKey;
using maya::SpecialKey;
namespace pick = agentty::ui::pick;

namespace {

// True when agentty is driven over an SSH session. Detected once from
// the env the SSH daemon exports into the remote shell. Over SSH the
// wire (not local CPU) is the render bottleneck: each streaming frame
// emits several KB of ANSI diff, and at 30 fps that saturates a
// high-latency / low-bandwidth link, producing the mid-turn lag and
// the end-of-turn catch-up repaint. We slow the streaming tick when
// remote (see the cadence block below).
bool running_over_ssh() {
    static const bool remote = [] {
        // Escape hatch: a fast LAN SSH hop doesn't need throttling.
        if (const char* off = std::getenv("AGENTTY_NO_SSH_THROTTLE");
            off && off[0] && off[0] != '0')
            return false;
        return std::getenv("SSH_CONNECTION") != nullptr
            || std::getenv("SSH_TTY") != nullptr
            || std::getenv("SSH_CLIENT") != nullptr;
    }();
    return remote;
}

} // namespace

// The single definition of the streaming Tick cadence. See the
// declaration in subscribe.hpp for the full rationale. Computed once
// per session — the inputs are immutable — and shared verbatim by both
// subscribe() (the timer interval) and Program::visual_hash() (the
// phase-locked animation bucket).
std::chrono::milliseconds streaming_tick_period() noexcept {
    static const std::chrono::milliseconds period = [] {
        auto base = maya::ansi::env_supports_synchronized_output()
            ? std::chrono::milliseconds(33)
            : std::chrono::milliseconds(100);
        if (running_over_ssh())
            return std::max(base, std::chrono::milliseconds(80));
        return base;
    }();
    return period;
}

namespace {

// True when the last message still carries in-flight wire bytes
// (streaming_text / pending_stream not yet drained into the settled
// body). The Tick subscription gates the reveal-fx animation clock and
// the pending_stream→streaming_text drip; gating it ONLY on
// m.s.active() leaves a one-tick gap at the very START of a stream:
// the first 1-2 StreamTextDelta msgs paint via their own render (fps=0),
// but the animation loop + drip don't engage until the Tick timer the
// reducer just armed actually fires — a visible split-second hang after
// the first word or two. agent_session never sees this because it
// subscribes to Tick UNCONDITIONALLY (the clock is always running).
// Rather than run an always-on tick when idle (wasteful at fps=0), we
// extend the tick window to cover any frame where live bytes exist, so
// the clock is already ticking the instant the first delta lands and
// the reveal animation is continuous from byte one — same end result
// as the reference, with zero idle cost.
bool tail_has_live_bytes(const Model& m) noexcept {
    if (m.d.current.messages.empty()) return false;
    const auto& back = m.d.current.messages.back();
    return !back.streaming_text.empty() || !back.pending_stream.empty();
}

// ── Per-modal key handlers — return std::nullopt to fall through ──────────

std::optional<Msg> on_permission(const KeyEvent& ev) {
    if (auto* ck = std::get_if<CharKey>(&ev.key)) {
        switch (ck->codepoint) {
            case 'y': case 'Y': return PermissionApprove{};
            case 'n': case 'N': return PermissionReject{};
            case 'a': case 'A': return PermissionApproveAlways{};
        }
    }
    if (std::holds_alternative<SpecialKey>(ev.key)
        && std::get<SpecialKey>(ev.key) == SpecialKey::Escape)
        return PermissionReject{};
    return std::nullopt;
}

std::optional<Msg> on_command_palette(const KeyEvent& ev) {
    if (std::holds_alternative<SpecialKey>(ev.key)) {
        auto sk = std::get<SpecialKey>(ev.key);
        switch (sk) {
            case SpecialKey::Escape:    return CloseCommandPalette{};
            case SpecialKey::Enter:     return CommandPaletteSelect{};
            case SpecialKey::Up:        return CommandPaletteMove{-1};
            case SpecialKey::Down:      return CommandPaletteMove{+1};
            case SpecialKey::Backspace: return CommandPaletteBackspace{};
            default: break;
        }
    }
    if (auto* ck = std::get_if<CharKey>(&ev.key))
        return CommandPaletteInput{ck->codepoint};
    return std::nullopt;
}

std::optional<Msg> on_mention_palette(const KeyEvent& ev) {
    if (std::holds_alternative<SpecialKey>(ev.key)) {
        auto sk = std::get<SpecialKey>(ev.key);
        switch (sk) {
            case SpecialKey::Escape:    return CloseMentionPalette{};
            case SpecialKey::Enter:     return MentionPaletteSelect{};
            case SpecialKey::Up:        return MentionPaletteMove{-1};
            case SpecialKey::Down:      return MentionPaletteMove{+1};
            case SpecialKey::Backspace: return MentionPaletteBackspace{};
            default: break;
        }
    }
    if (auto* ck = std::get_if<CharKey>(&ev.key))
        return MentionPaletteInput{ck->codepoint};
    return std::nullopt;
}

std::optional<Msg> on_symbol_palette(const KeyEvent& ev) {
    if (std::holds_alternative<SpecialKey>(ev.key)) {
        auto sk = std::get<SpecialKey>(ev.key);
        switch (sk) {
            case SpecialKey::Escape:    return CloseSymbolPalette{};
            case SpecialKey::Enter:     return SymbolPaletteSelect{};
            case SpecialKey::Up:        return SymbolPaletteMove{-1};
            case SpecialKey::Down:      return SymbolPaletteMove{+1};
            case SpecialKey::Backspace: return SymbolPaletteBackspace{};
            default: break;
        }
    }
    if (auto* ck = std::get_if<CharKey>(&ev.key))
        return SymbolPaletteInput{ck->codepoint};
    return std::nullopt;
}

// Ctrl+G code-block picker. Enter runs the cursor row; a bare digit
// runs that row directly (1-based, matching the ①②③ row labels — the
// zero-navigation fast path: Ctrl+G, 2, done). `e` stages into the
// composer for editing, `y` copies clean.
std::optional<Msg> on_code_block_picker(const KeyEvent& ev) {
    if (std::holds_alternative<SpecialKey>(ev.key)) {
        switch (std::get<SpecialKey>(ev.key)) {
            case SpecialKey::Escape: return CloseCodeBlockPicker{};
            case SpecialKey::Enter:  return CodeBlockPickerSelect{};
            case SpecialKey::Up:     return CodeBlockPickerMove{-1};
            case SpecialKey::Down:   return CodeBlockPickerMove{+1};
            default: break;
        }
    }
    if (auto* ck = std::get_if<CharKey>(&ev.key)) {
        char32_t c = ck->codepoint;
        if (c >= U'1' && c <= U'9')
            return CodeBlockPickerSelect{static_cast<int>(c - U'1')};
        switch (c) {
            case U'e': case U'E': return CodeBlockPickerEdit{};
            case U'y': case U'Y': return CodeBlockPickerCopy{};
            case U'q': case U'Q': return CloseCodeBlockPicker{};
            default: break;
        }
    }
    return std::nullopt;
}

// Post-run result card: a = attach to composer, y = copy, Esc/q/Enter
// dismiss. Enter deliberately DISCARDS rather than attaches — the
// default action must be the safe one (no surprise composer content);
// attaching is the explicit `a`.
std::optional<Msg> on_code_block_result(const KeyEvent& ev) {
    if (std::holds_alternative<SpecialKey>(ev.key)) {
        switch (std::get<SpecialKey>(ev.key)) {
            case SpecialKey::Escape:
            case SpecialKey::Enter:    return CodeBlockResultDiscard{};
            // Scroll the capture. The result card is read-only (no
            // selection cursor), so Move deltas translate straight to
            // viewport scroll rows in the reducer.
            case SpecialKey::Up:       return CodeBlockPickerMove{-1};
            case SpecialKey::Down:     return CodeBlockPickerMove{+1};
            case SpecialKey::PageUp:   return CodeBlockPickerMove{-10};
            case SpecialKey::PageDown: return CodeBlockPickerMove{+10};
            default: break;
        }
    }
    if (auto* ck = std::get_if<CharKey>(&ev.key)) {
        switch (ck->codepoint) {
            case U'a': case U'A': return CodeBlockResultAttach{};
            case U'y': case U'Y': return CodeBlockResultCopy{};
            case U'q': case U'Q': case U'd': case U'D':
                return CodeBlockResultDiscard{};
            default: break;
        }
    }
    return std::nullopt;
}

std::optional<Msg> on_model_picker(const KeyEvent& ev) {
    if (std::holds_alternative<SpecialKey>(ev.key)) {
        auto sk = std::get<SpecialKey>(ev.key);
        switch (sk) {
            case SpecialKey::Escape:   return CloseModelPicker{};
            case SpecialKey::Enter:    return ModelPickerSelect{};
            case SpecialKey::Up:       return ModelPickerMove{-1};
            case SpecialKey::Down:     return ModelPickerMove{+1};
            // Backspace edits the live search query rather than paging.
            case SpecialKey::Backspace: return ModelPickerFilterBackspace{};
            case SpecialKey::Home:     return ModelPickerJump{ModelPickerJump::Where::Home};
            case SpecialKey::End:      return ModelPickerJump{ModelPickerJump::Where::End};
            case SpecialKey::PageUp:   return ModelPickerJump{ModelPickerJump::Where::PageUp};
            case SpecialKey::PageDown: return ModelPickerJump{ModelPickerJump::Where::PageDown};
            // ←/→ cycle the reasoning-effort tier for the highlighted model.
            case SpecialKey::Left:     return ModelPickerCycleEffort{-1};
            case SpecialKey::Right:    return ModelPickerCycleEffort{+1};
            default: break;
        }
    }
    if (auto* ck = std::get_if<CharKey>(&ev.key)) {
        char32_t c = ck->codepoint;
        // Ctrl+F toggles the highlighted model as a favourite — moved off
        // the bare `f` key now that plain letters feed the search box.
        if (ev.mods.ctrl) {
            if (c >= 0x01 && c <= 0x1A) c = U'a' + (c - 1);
            if (c == U'f') return ModelPickerToggleFavorite{};
            return std::nullopt;
        }
        // Any other printable codepoint types into the filter query.
        if (c >= 0x20) return ModelPickerFilterInput{c};
    }
    return std::nullopt;
}

std::optional<Msg> on_provider_picker(const KeyEvent& ev) {
    if (std::holds_alternative<SpecialKey>(ev.key)) {
        auto sk = std::get<SpecialKey>(ev.key);
        switch (sk) {
            case SpecialKey::Escape:   return CloseProviderPicker{};
            case SpecialKey::Enter:    return ProviderPickerSelect{};
            case SpecialKey::Up:       return ProviderPickerMove{-1};
            case SpecialKey::Down:     return ProviderPickerMove{+1};
            case SpecialKey::Home:     return ProviderPickerJump{ProviderPickerJump::Where::Home};
            case SpecialKey::End:      return ProviderPickerJump{ProviderPickerJump::Where::End};
            case SpecialKey::PageUp:   return ProviderPickerJump{ProviderPickerJump::Where::PageUp};
            case SpecialKey::PageDown: return ProviderPickerJump{ProviderPickerJump::Where::PageDown};
            default: break;
        }
    }
    return std::nullopt;
}

std::optional<Msg> on_thread_list(const KeyEvent& ev) {
    if (std::holds_alternative<SpecialKey>(ev.key)) {
        auto sk = std::get<SpecialKey>(ev.key);
        switch (sk) {
            case SpecialKey::Escape:   return CloseThreadList{};
            case SpecialKey::Enter:    return ThreadListSelect{};
            case SpecialKey::Up:       return ThreadListMove{-1};
            case SpecialKey::Down:     return ThreadListMove{+1};
            case SpecialKey::Home:     return ThreadListJump{ThreadListJump::Where::Home};
            case SpecialKey::End:      return ThreadListJump{ThreadListJump::Where::End};
            case SpecialKey::PageUp:   return ThreadListJump{ThreadListJump::Where::PageUp};
            case SpecialKey::PageDown: return ThreadListJump{ThreadListJump::Where::PageDown};
            default: break;
        }
    }
    if (auto* ck = std::get_if<CharKey>(&ev.key))
        if (ck->codepoint == 'n' || ck->codepoint == 'N') return NewThread{};
    return std::nullopt;
}

std::optional<Msg> on_diff_review(const KeyEvent& ev) {
    if (std::holds_alternative<SpecialKey>(ev.key)) {
        auto sk = std::get<SpecialKey>(ev.key);
        switch (sk) {
            case SpecialKey::Escape: return CloseDiffReview{};
            case SpecialKey::Up:     return DiffReviewMove{-1};
            case SpecialKey::Down:   return DiffReviewMove{+1};
            case SpecialKey::Left:   return DiffReviewPrevFile{};
            case SpecialKey::Right:  return DiffReviewNextFile{};
            default: break;
        }
    }
    if (auto* ck = std::get_if<CharKey>(&ev.key)) {
        switch (ck->codepoint) {
            case 'y': case 'Y': return AcceptHunk{};
            case 'n': case 'N': return RejectHunk{};
            case 'a': case 'A': return AcceptAllChanges{};
            case 'x': case 'X': return RejectAllChanges{};
        }
    }
    return std::nullopt;
}

std::optional<Msg> on_todo_modal(const KeyEvent& ev) {
    if (std::holds_alternative<SpecialKey>(ev.key)
        && std::get<SpecialKey>(ev.key) == SpecialKey::Escape)
        return CloseTodoModal{};
    return std::nullopt;
}

// Login modal — dispatches based on which sub-state we're in.
// Picking accepts only '1'/'2' (and Esc to close);
// OAuthCode + ApiKeyInput consume free-text input + cursor keys + Enter;
// OAuthExchanging consumes only Esc (cancel back to Picking is fine);
// Failed accepts any key to return to Picking.
std::optional<Msg> on_login(const ui::login::State& state, const KeyEvent& ev) {
    using namespace agentty::ui::login;

    // Esc always closes — gives the user an out from any sub-state.
    if (std::holds_alternative<SpecialKey>(ev.key)
        && std::get<SpecialKey>(ev.key) == SpecialKey::Escape)
        return CloseLogin{};

    if (std::holds_alternative<Picking>(state)
        || std::holds_alternative<Failed>(state)) {
        if (auto* ck = std::get_if<CharKey>(&ev.key))
            return LoginPickMethod{ck->codepoint};
        return std::nullopt;
    }

    if (std::holds_alternative<OAuthExchanging>(state)) {
        // Awaiting HTTP completion. No keys accepted besides Esc (above).
        return NoOp{};
    }

    // ── OAuthCode-only chrome shortcuts ─────────────────────────────
    // Bare letter `c` / `o` would collide with text input on the code
    // field, so we gate these on a modifier OR on the field being
    // empty (the typical state when the user has just landed in this
    // screen and wants to copy the URL before pasting the code).
    if (auto* oc = std::get_if<OAuthCode>(&state)) {
        if (auto* ck = std::get_if<CharKey>(&ev.key)) {
            char32_t c = ck->codepoint;
            // Normalize Ctrl-letter codes (terminals deliver Ctrl+X as
            // either raw 0x01..0x1A or the lowercase letter + ctrl mod
            // depending on whether KKP / modifyOtherKeys is enabled).
            if (ev.mods.ctrl && c >= 0x01 && c <= 0x1A)
                c = U'a' + (c - 1);
            const bool empty_code = oc->code_input.empty();
            // Ctrl+C is reserved for Quit at the global layer, so we
            // use Ctrl+Y as the no-collision "yank URL" binding. Plain
            // `c` / `o` also work when the code input is empty, so the
            // typical happy path (land here, hit `c`, paste in
            // browser) needs zero modifiers.
            const bool is_y = (c == U'y' || c == U'Y');
            const bool is_c = (c == U'c' || c == U'C');
            const bool is_o = (c == U'o' || c == U'O');
            if (ev.mods.ctrl && is_y) return LoginCopyAuthUrl{};
            if (ev.mods.ctrl && is_o) return LoginOpenBrowserAgain{};
            if (empty_code && is_c)   return LoginCopyAuthUrl{};
            if (empty_code && is_o)   return LoginOpenBrowserAgain{};
        }
    }

    // OAuthCode or ApiKeyInput — both accept free-text input.
    if (std::holds_alternative<SpecialKey>(ev.key)) {
        switch (std::get<SpecialKey>(ev.key)) {
            case SpecialKey::Enter:     return LoginSubmit{};
            case SpecialKey::Backspace: return LoginBackspace{};
            case SpecialKey::Left:      return LoginCursorLeft{};
            case SpecialKey::Right:     return LoginCursorRight{};
            default: return std::nullopt;
        }
    }
    if (auto* ck = std::get_if<CharKey>(&ev.key))
        if (ck->codepoint >= 0x20) return LoginCharInput{ck->codepoint};
    return std::nullopt;
}

std::optional<Msg> on_global(const KeyEvent& ev) {
    // Ctrl-J on legacy terminals (iSH, plain xterm, tmux without KKP /
    // modifyOtherKeys) arrives as the bare LF byte 0x0A. maya's input
    // parser folds BOTH \r (0x0D) and \n (0x0A) into SpecialKey::Enter,
    // so by the time we see it the key looks identical to Return — and
    // every other Ctrl shortcut works because they arrive as distinct
    // control bytes, but Ctrl-J gets swallowed into Enter. The only
    // surviving discriminator is raw_sequence, which preserves the
    // original byte: Return sends \r, Ctrl-J sends \n. Recover the
    // OpenThreadList binding from that.  (Terminals that map Return to
    // LF can't distinguish the two; there Ctrl-J is simply unavailable,
    // which is unavoidable for this keystroke.)
    if (std::holds_alternative<SpecialKey>(ev.key)
        && std::get<SpecialKey>(ev.key) == SpecialKey::Enter
        && !ev.mods.shift && !ev.mods.alt && !ev.mods.ctrl
        && ev.raw_sequence == "\n")
        return OpenThreadList{};
    // Alt+←/→ — quick-cycle through threads without the picker. In the
    // deck order the ^J list shows (newest first): ← = newer, → = older.
    // Checked in on_global (before on_composer) so it never collides
    // with plain/Ctrl arrow cursor movement in the composer.
    // (Terminals without a real Alt key — iPhone terminals — emulate Alt
    // as an Esc prefix. That only works because Esc is INERT on the main
    // screen: a stray Escape keystroke must never quit the app, or the
    // Esc-then-arrow sequence would kill agentty mid-chord. Quit is
    // Ctrl+C, deliberately the only app-exit key.)
    if (ev.mods.alt && !ev.mods.ctrl
        && std::holds_alternative<SpecialKey>(ev.key)) {
        switch (std::get<SpecialKey>(ev.key)) {
            case SpecialKey::Left:  return ThreadCycle{-1};
            case SpecialKey::Right: return ThreadCycle{+1};
            default: break;
        }
    }
    if (auto* ck = std::get_if<CharKey>(&ev.key)) {
        char32_t c = ck->codepoint;
        // Legacy terminals (iSH, plain xterm, tmux without KKP /
        // modifyOtherKeys) deliver Ctrl-<letter> as the raw control
        // byte 0x01..0x1A with NO ctrl modifier flag set — e.g. Ctrl-K
        // arrives as 0x0B. Normalise that to the lower-case letter with
        // ctrl implied so the shortcuts fire regardless of keyboard-
        // protocol support. (Ctrl-J / 0x0A is handled above since maya
        // turns it into Enter before we ever see it as a CharKey.)
        const bool raw_ctrl = (c >= 0x01 && c <= 0x1A);
        if (raw_ctrl) c = U'a' + (c - 1);
        // 0x09 (Ctrl-I / Tab) and 0x0D (Ctrl-M / Enter) are excluded:
        // those are unconditionally Tab / Enter on legacy terminals and
        // hijacking them would break tab-completion and submit.
        const bool ctrl = ev.mods.ctrl || (raw_ctrl && c != U'i' && c != U'm');
        if (ctrl) {
            switch (c) {
                case U'c': case U'C': return Quit{};
                case U'/':           return OpenModelPicker{};
                case U'j': case U'J': return OpenThreadList{};
                case U'k': case U'K': return OpenCommandPalette{};
                case U'p': case U'P': return OpenProviderPicker{};
                case U'l': case U'L': return RedrawScreen{};
                case U'r': case U'R': return OpenDiffReview{};
                case U'n': case U'N': return NewThread{};
                case U't': case U'T': return OpenTodoModal{};
                case U'e': case U'E': return ComposerToggleExpand{};
                case U'g': case U'G': return OpenCodeBlockPicker{};
                default: break;
            }
        }
    }
    if (ev.mods.shift && std::holds_alternative<SpecialKey>(ev.key)) {
        auto sk = std::get<SpecialKey>(ev.key);
        if (sk == SpecialKey::Tab || sk == SpecialKey::BackTab)
            return CycleProfile{};
    }
    return std::nullopt;
}

// Composer state needed for keymap decisions, captured into the
// subscription so the lambda doesn't have to retain a Model copy
// (Model is move-only since the view cache moved on-board, and a
// reference would dangle past the subscribe() call).
struct ComposerKeyState {
    bool text_empty;
    bool has_queued;
    bool in_history;     // walking over prior user messages — ↓ has meaning
    bool has_history;    // any prior user turns exist at all — ↑ has meaning
    bool in_queue_peek;  // editing a queued item in place (Alt+↑/↓ cycle)
};

std::optional<Msg> on_composer(ComposerKeyState s, const KeyEvent& ev) {
    if (std::holds_alternative<SpecialKey>(ev.key)) {
        auto sk = std::get<SpecialKey>(ev.key);
        switch (sk) {
            case SpecialKey::Enter:
                // Newline on Shift+Enter (chat-app muscle memory) OR
                // Alt+Enter (universal fallback for terminals that don't
                // speak KKP / modifyOtherKeys). Maya enables both
                // protocols on entry, but if the user's terminal ignores
                // both, Shift+Enter arrives as plain Enter and the only
                // way to insert a newline is Alt+Enter — the legacy
                // binding that EVERY terminal delivers as `\x1b\r`.
                // Plain Enter still submits.
                return (ev.mods.shift || ev.mods.alt)
                       ? Msg{ComposerNewline{}}
                       : Msg{ComposerEnter{}};
            case SpecialKey::Backspace:
                // Alt+Backspace on an empty composer with nothing peeked
                // — "undo queue": drop the most recently queued message.
                // Useful when you fire-and-forget into the queue and
                // immediately regret it while the agent's still busy.
                if (ev.mods.alt && s.text_empty && s.has_queued && !s.in_queue_peek)
                    return Msg{ComposerQueuePopLast{}};
                return ComposerBackspace{};
            case SpecialKey::Left:
                // Ctrl+Left = jump-by-word. Mirrors readline / every
                // text editor. Plain Left is per-character (chip-aware).
                if (ev.mods.ctrl) return ComposerCursorWordLeft{};
                return ComposerCursorLeft{};
            case SpecialKey::Right:
                if (ev.mods.ctrl) return ComposerCursorWordRight{};
                return ComposerCursorRight{};
            case SpecialKey::Home:      return ComposerCursorHome{};
            case SpecialKey::End:       return ComposerCursorEnd{};
            case SpecialKey::Up:
                // Alt+↑ — per-item queue editor. Takes precedence over
                // every other ↑ binding so it works mid-edit (e.g. you
                // typed half a thought, realized you want to fix the
                // queued one first — Alt+↑ still gets you there
                // without having to clear the composer first).
                if (ev.mods.alt && s.has_queued)
                    return Msg{ComposerQueuePeekPrev{}};
                // ↑ priorities, in order:
                //   1. queue non-empty AND composer empty → recall queue
                //      (Claude Code's "Press up to edit queued
                //      messages" affordance, binary offsets 84602515 /
                //      76303220).
                //   2. already mid-history-walk → step further into the
                //      past.
                //   3. composer empty AND there's at least one prior
                //      user turn → start history walk.
                // Anything else (multi-line text editing, etc.) falls
                // through so ↑ stays available for cursor moves later.
                if (s.text_empty && s.has_queued)
                    return Msg{ComposerRecallQueued{}};
                if (s.in_history) return ComposerHistoryPrev{};
                if (s.text_empty && s.has_history) return ComposerHistoryPrev{};
                return std::nullopt;
            case SpecialKey::Down:
                // Alt+↓ — walk back OUT of the per-item queue peek
                // toward the live draft. Only meaningful while peeking;
                // outside that, falls through.
                if (ev.mods.alt && s.in_queue_peek)
                    return Msg{ComposerQueuePeekNext{}};
                // ↓ only has meaning while walking history — it walks
                // back toward the live draft. Outside the walk it
                // falls through.
                if (s.in_history) return ComposerHistoryNext{};
                return std::nullopt;
            case SpecialKey::Escape:
                // Deliberately INERT on the main screen — quit is Ctrl+C
                // only. Two reasons: (1) Esc is the most-mashed key in a
                // terminal (vim muscle memory, "dismiss whatever this
                // is") and instant app-exit on it loses sessions; (2)
                // iPhone terminals (iSH, Termius, a-Shell) have no Alt
                // key and emulate Alt+←/→ as Esc-then-arrow — the Esc
                // half of that chord must not be a live Quit binding.
                // Esc still cancels a streaming turn (handled in the
                // dispatch above) and closes every modal (each modal
                // handler owns its own Esc).
                return std::nullopt;
            default: return std::nullopt;
        }
    }
    if (auto* ck = std::get_if<CharKey>(&ev.key)) {
        // Ctrl-prefixed letter keys: editor-style controls. Tested
        // before the printable-text branch so a Ctrl+Z arriving as
        // CharKey{0x1A} or CharKey{'z'}+ctrl is captured either way.
        if (ev.mods.ctrl && !ev.mods.alt) {
            char32_t c = ck->codepoint;
            // Some terminals deliver ASCII Ctrl-X as 0x01..0x1A; others
            // (KKP / modifyOtherKeys) keep it as the lower-case letter
            // with mods.ctrl=true. Normalise.
            if (c >= 0x01 && c <= 0x1A) c = U'a' + (c - 1);
            switch (c) {
                case U'k': return ComposerKillToEndOfLine{};
                case U'u': return ComposerKillToBeginningOfLine{};
                case U'w':
                    // Ctrl+W — delete word backward (readline
                    // unix-word-rubout). The universal "oops, drop
                    // that word" key across every shell and editor.
                    return ComposerDeleteWordBack{};
                case U'z':
                    // Ctrl+Shift+Z is the alternate Redo binding (no
                    // Ctrl+Y on macOS muscle-memory). Plain Ctrl+Z is
                    // Undo.
                    return ev.mods.shift ? Msg{ComposerRedo{}}
                                         : Msg{ComposerUndo{}};
                case U'y': return ComposerRedo{};
                case U'v':
                    // Ctrl+V → image paste from clipboard. On Linux/macOS
                    // this reaches us as raw 0x16 because the terminal
                    // emulator forwards Ctrl-letter codes. On Windows
                    // Terminal Ctrl+V is bound to the terminal's own
                    // "paste" action by default, so this keystroke is
                    // swallowed before it ever hits agentty — that's
                    // why we also accept Alt+V below and detect empty
                    // bracketed-paste in update/composer.cpp.
                    return ComposerImagePasteFromClipboard{};
                default: break;
            }
        }
        // Alt+V → image paste from clipboard, alternate trigger that
        // every terminal (Windows Terminal included) passes through
        // to the application. Same Msg as Ctrl+V; the reducer arm
        // doesn't care which key fired it.
        if (ev.mods.alt && !ev.mods.ctrl) {
            char32_t c = ck->codepoint;
            // Alt+V arrives as ESC v → CharKey{'v'} + mods.alt. Some
            // terminals upcase the codepoint when Shift is also held;
            // both V and v should fire.
            if (c == U'v' || c == U'V')
                return ComposerImagePasteFromClipboard{};
            // Alt+D — delete word forward (readline kill-word).
            // Symmetric to Ctrl+W; arrives as ESC d → CharKey{'d'}+alt.
            if (c == U'd' || c == U'D')
                return ComposerDeleteWordForward{};
        }
        if (ck->codepoint >= 0x20) return ComposerCharInput{ck->codepoint};
    }
    return std::nullopt;
}

} // namespace

Sub<Msg> subscribe(const Model& m) {
    const bool in_perm    = m.d.pending_permission.has_value();
    const bool in_cmd     = is_open(m.ui.command_palette);
    const bool in_mention = mention_is_open(m.ui.mention_palette);
    const bool in_symbol  = symbol_palette_is_open(m.ui.symbol_palette);
    const bool in_blocks  = code_block_picker_is_open(m.ui.code_blocks);
    const bool in_blockres = code_block_result_is_open(m.ui.code_blocks);
    const bool in_models  = pick::is_open(m.ui.model_picker);
    const bool in_providers = pick::is_open(m.ui.provider_picker);
    const bool in_threads = pick::is_open(m.ui.thread_list);
    const bool in_diff    = pick::is_open(m.ui.diff_review);
    const bool in_todo    = pick::is_open(m.ui.todo.open);
    const bool in_login   = ui::login::is_open(m.ui.login);
    const bool streaming  = m.s.active()
                         && !m.s.is_awaiting_permission();
    // Ctrl+←/→ thread-cycle gate: the agent turn must be fully idle.
    // Distinct from `streaming` (which carves out awaiting-permission
    // so Esc keeps meaning "cancel") — switching threads mid-turn is
    // never allowed, permission prompt or not.
    const bool turn_active = m.s.active();
    bool has_history = false;
    for (const auto& msg : m.d.current.messages)
        if (msg.role == Role::User && !msg.text.empty()) { has_history = true; break; }
    const ComposerKeyState composer_state{
        m.ui.composer.text.empty(),
        !m.ui.composer.queued.empty(),
        m.ui.composer.history_idx >= 0,
        has_history,
        m.ui.composer.queue_peek_idx >= 0,
    };

    auto key_sub = Sub<Msg>::on_key(
        [=, login_state = m.ui.login](const KeyEvent& ev) -> std::optional<Msg> {
            // Login modal owns the whole keyboard — auth is the gating
            // step, no other UI is reachable until the user finishes
            // (or Escs out, which is allowed but leaves agentty unauth'd).
            if (in_login)   return on_login(login_state, ev);
            if (in_perm)    return on_permission(ev);
            if (in_cmd)     return on_command_palette(ev);
            if (in_mention) return on_mention_palette(ev);
            if (in_symbol)  return on_symbol_palette(ev);
            if (in_blocks)  return on_code_block_picker(ev);
            if (in_blockres) return on_code_block_result(ev);
            if (in_models)  return on_model_picker(ev);
            if (in_providers) return on_provider_picker(ev);
            if (in_threads) return on_thread_list(ev);
            if (in_diff)    return on_diff_review(ev);
            if (in_todo)    if (auto r = on_todo_modal(ev)) return r;
            // Esc during a live stream cancels the request rather than
            // quitting the app. Modals above swallow Esc themselves, so this
            // only fires from the bare composer view.
            if (streaming
                && std::holds_alternative<SpecialKey>(ev.key)
                && std::get<SpecialKey>(ev.key) == SpecialKey::Escape)
                return CancelStream{};
            // Ctrl+←/→ — quick-cycle threads, same deck order as Alt+←/→
            // (← = newer, → = older). Only when the composer is EMPTY:
            // with text in the box Ctrl+arrows stay jump-by-word
            // (readline muscle memory, handled in on_composer). And only
            // while no agent turn is running — mid-turn the key falls
            // through to the composer so it can't yank the thread out
            // from under a live stream.
            if (!turn_active && composer_state.text_empty
                && ev.mods.ctrl && !ev.mods.alt
                && std::holds_alternative<SpecialKey>(ev.key)) {
                switch (std::get<SpecialKey>(ev.key)) {
                    case SpecialKey::Left:  return ThreadCycle{-1};
                    case SpecialKey::Right: return ThreadCycle{+1};
                    default: break;
                }
            }
            if (auto msg = on_global(ev)) return msg;
            return on_composer(composer_state, ev);
        });

    auto paste_sub = Sub<Msg>::on_paste([in_login](std::string s) -> Msg {
        // Pastes go to the login modal's text fields when it's open
        // (users will paste OAuth codes / API keys); otherwise they're
        // composer pastes.
        if (in_login) return LoginPaste{std::move(s)};
        return ComposerPaste{std::move(s)};
    });

    // Only subscribe to Tick while the spinner is visible. With fps=0 the
    // maya loop is purely event-driven; an unconditional 16ms tick would
    // force a render 60× per second even when nothing is changing.
    //
    // Tick cadence is gated on the host terminal's support for DEC mode
    // 2026 (synchronized output). On terminals that buffer the frame
    // atomically, 33 ms (~30 fps) keeps the spinner smooth without
    // flicker. On terminals that paint bytes as they arrive (Apple
    // Terminal, plain xterm, tmux without sync passthrough), every
    // repaint is visibly progressive — so we drop to 100 ms (10 fps) to
    // cut the flicker frequency by 3× at the cost of a slightly choppier
    // spinner. The capability is heuristic-detected once at startup; see
    // maya::ansi::env_supports_synchronized_output().
    //
    // SSH override: when remote, the wire — not local paint — is the
    // bottleneck. Each streaming frame emits several KB of ANSI diff;
    // at 30 fps that's ~290 KB/s, which saturates a high-latency or
    // low-bandwidth link and shows up as mid-turn lag plus an
    // end-of-turn catch-up repaint (the kernel send buffer drains the
    // backlog after the stream stops). We clamp the streaming tick to
    // at least 80 ms (~12 fps) when remote — the SSH round-trip latency
    // already dominates perceived smoothness, so dropped frames aren't
    // noticeable, while the sustained byte rate falls proportionally.
    // We take the SLOWER of the local choice and the SSH floor so a
    // non-sync terminal (already 100 ms) is never sped up. The reveal
    // SPEED is unchanged — the pacer is bytes/second, not bytes/tick, so
    // prose fills at the same wall-clock rate, just in fewer, larger
    // frames.
    // The settle-freeze (meta.cpp Tick, the agent_session MessageStop
    // analog) fires on a Tick while m.s.is_idle() and pending_settle_
    // freeze is set. Gating the tick ONLY on active()/live-bytes drops
    // the clock the instant the reveal drains its last bytes into the
    // settled body — but the widget hasn't flipped live_ off yet, so
    // pending_settle_freeze is still set with no tick left to fire it.
    // The deferred freeze then strands until the next user keystroke and
    // diffs against a stale prev_cells = cache-miss re-emit = the
    // duplicated turn the user sees in scrollback. Keep ticking until the
    // freeze has actually fired (flag cleared) so the live-tail→frozen
    // handoff always lands on a fresh frame, exactly like agent_session's
    // always-on 30fps clock reconciles the collapse the same frame.
    //
    // Reveal-still-gliding term (was MISSING — the low-CPU post-stream
    // stall). maya's reveal_fx is a WALL-CLOCK typewriter: after the wire
    // drains its last bytes (m.s.active() false, tail_has_live_bytes
    // false) the cursor is often still gliding across the final
    // paragraph at ~90 cps, with SECONDS of animation left on a longer
    // turn. If the gate rests only on the four terms above, the Tick
    // stops the instant the bytes land and the reveal FREEZES mid-glide
    // — the widget sits at 1% CPU waiting for a clock that won't tick
    // until the next keystroke, which then snaps the remaining text into
    // view all at once. `!live_tail_reveal_settled(m)` is true exactly
    // while the reveal (is_live / reveal_in_progress / is_finalizing /
    // is_parsing) has NOT drained, so we keep waking the widget until the
    // typewriter reaches the live edge — the same reason build_live_tail
    // and the deferred settle-freeze consult this predicate.
    if (m.s.active() || tail_has_live_bytes(m) || m.ui.pending_settle_freeze
        || m.ui.settle_cooldown_ticks > 0
        || !detail::live_tail_reveal_settled(m)) {
        auto tick = Sub<Msg>::every(streaming_tick_period(), Tick{});
        return Sub<Msg>::batch(std::move(key_sub), std::move(paste_sub), std::move(tick));
    }
    return Sub<Msg>::batch(std::move(key_sub), std::move(paste_sub));
}

} // namespace agentty::app
