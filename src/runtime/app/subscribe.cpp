#include "agentty/runtime/app/subscribe.hpp"

#include <chrono>
#include <optional>
#include <variant>

#include <maya/terminal/ansi.hpp>

#include "agentty/runtime/login.hpp"
#include "agentty/runtime/picker.hpp"

namespace agentty::app {

using maya::Sub;
using maya::KeyEvent;
using maya::CharKey;
using maya::SpecialKey;
namespace pick = agentty::ui::pick;

namespace {

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

std::optional<Msg> on_model_picker(const KeyEvent& ev) {
    if (std::holds_alternative<SpecialKey>(ev.key)) {
        auto sk = std::get<SpecialKey>(ev.key);
        switch (sk) {
            case SpecialKey::Escape: return CloseModelPicker{};
            case SpecialKey::Enter:  return ModelPickerSelect{};
            case SpecialKey::Up:     return ModelPickerMove{-1};
            case SpecialKey::Down:   return ModelPickerMove{+1};
            default: break;
        }
    }
    if (auto* ck = std::get_if<CharKey>(&ev.key))
        if (ck->codepoint == 'f' || ck->codepoint == 'F')
            return ModelPickerToggleFavorite{};
    return std::nullopt;
}

std::optional<Msg> on_thread_list(const KeyEvent& ev) {
    if (std::holds_alternative<SpecialKey>(ev.key)) {
        auto sk = std::get<SpecialKey>(ev.key);
        switch (sk) {
            case SpecialKey::Escape: return CloseThreadList{};
            case SpecialKey::Enter:  return ThreadListSelect{};
            case SpecialKey::Up:     return ThreadListMove{-1};
            case SpecialKey::Down:   return ThreadListMove{+1};
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
    if (ev.mods.ctrl) {
        if (auto* ck = std::get_if<CharKey>(&ev.key)) {
            switch (static_cast<char>(ck->codepoint)) {
                case 'c': case 'C': return Quit{};
                case '/':           return OpenModelPicker{};
                case 'j': case 'J': return OpenThreadList{};
                case 'k': case 'K': return OpenCommandPalette{};
                case 'l': case 'L': return RedrawScreen{};
                case 'r': case 'R': return OpenDiffReview{};
                case 'n': case 'N': return NewThread{};
                case 't': case 'T': return OpenTodoModal{};
                case 'e': case 'E': return ComposerToggleExpand{};
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
            case SpecialKey::Escape:    return Quit{};
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
    const bool in_models  = pick::is_open(m.ui.model_picker);
    const bool in_threads = pick::is_open(m.ui.thread_list);
    const bool in_diff    = pick::is_open(m.ui.diff_review);
    const bool in_todo    = pick::is_open(m.ui.todo.open);
    const bool in_login   = ui::login::is_open(m.ui.login);
    const bool streaming  = m.s.active()
                         && !m.s.is_awaiting_permission();
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
            if (in_models)  return on_model_picker(ev);
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
    if (m.s.active()) {
        static const auto tick_period = maya::ansi::env_supports_synchronized_output()
            ? std::chrono::milliseconds(33)
            : std::chrono::milliseconds(100);
        auto tick = Sub<Msg>::every(tick_period, Tick{});
        return Sub<Msg>::batch(std::move(key_sub), std::move(paste_sub), std::move(tick));
    }
    return Sub<Msg>::batch(std::move(key_sub), std::move(paste_sub));
}

} // namespace agentty::app
