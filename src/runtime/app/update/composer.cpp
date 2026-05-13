// composer_update — reducer for `msg::ComposerMsg`. The composer is purely
// local UI state (text, cursor, expanded flag, queued items, attachments,
// undo/redo stacks, history index); arms here don't reach into network /
// streaming / tools. ComposerEnter / ComposerSubmit route through
// detail::submit_message which handles the broader "kick a new turn" flow.

#include "agentty/runtime/app/update.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <maya/core/overload.hpp>

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/io/clipboard.hpp"
#include "agentty/runtime/command_palette.hpp"
#include "agentty/runtime/composer_attachment.hpp"
#include "agentty/runtime/mention_palette.hpp"
#include "agentty/runtime/symbol_palette.hpp"
#include "agentty/workspace/files.hpp"
#include "agentty/workspace/symbols.hpp"
#include "agentty/runtime/view/helpers.hpp"

namespace agentty::app::detail {

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kUndoDepth = 64;

// Snapshot the current composer payload into the undo stack and clear
// the redo stack. attachments are append-only at the runtime level
// (backspace over a chip removes the placeholder bytes from `text`
// but leaves the Attachment object in the vector — orphans are GC'd
// on composer clear), so storing only the vector's *size* at snapshot
// time is enough to restore on undo.
void push_undo(ComposerState& cs) {
    if (cs.undo_stack.size() >= kUndoDepth) {
        // Drop the oldest snapshot. erase from begin is O(N) on a
        // vector but N == 64 here so the cost is negligible compared
        // to the alternative of swapping in a deque.
        cs.undo_stack.erase(cs.undo_stack.begin());
    }
    ComposerState::Snapshot s;
    s.text   = cs.text;
    s.cursor = cs.cursor;
    s.attachments = cs.attachments;
    cs.undo_stack.push_back(std::move(s));
    cs.redo_stack.clear();
}

// History walking is "current draft" until the user mutates; any text
// edit after a walk treats the walked text as their new draft. Reset
// history_idx + drop the saved draft so the next ↑ snapshots the new
// state cleanly.
void reset_history(ComposerState& cs) {
    cs.history_idx = -1;
    cs.draft_save.reset();
}

void begin_edit(ComposerState& cs) {
    push_undo(cs);
    reset_history(cs);
}

// Word-boundary cursor walks. Boundaries are runs of whitespace; chip
// placeholders count as a single navigation unit (delegated to
// ui::chip_prev / chip_next). Mirrors the `vim`/`bash` Ctrl+W idea:
// skip whitespace, then skip word characters.
bool is_word_char(unsigned char c) noexcept {
    return std::isalnum(c) || c == '_';
}

int word_left(std::string_view s, int pos) noexcept {
    if (pos <= 0) return 0;
    // Step over a chip if the cursor sits at its right edge.
    int chip = ui::chip_prev(s, pos);
    if (chip != pos - 1
        || (pos > 0 && static_cast<unsigned char>(s[pos - 1]) == 0x01))
        return chip;
    int p = pos;
    // Skip trailing whitespace.
    while (p > 0 && std::isspace(static_cast<unsigned char>(s[p - 1]))) --p;
    // Skip a run of word chars.
    while (p > 0 && is_word_char(static_cast<unsigned char>(s[p - 1]))) --p;
    // If we didn't move past anything word-like, skip one punctuation
    // char so the cursor still advances.
    if (p == pos && p > 0) --p;
    return p;
}

int word_right(std::string_view s, int pos) noexcept {
    int n = static_cast<int>(s.size());
    if (pos >= n) return n;
    int chip = ui::chip_next(s, pos);
    if (chip != pos + 1
        || (pos < n && static_cast<unsigned char>(s[pos]) == 0x01))
        return chip;
    int p = pos;
    while (p < n && is_word_char(static_cast<unsigned char>(s[p]))) ++p;
    while (p < n && std::isspace(static_cast<unsigned char>(s[p]))) ++p;
    if (p == pos && p < n) ++p;
    return p;
}

// Image-paste detection. Terminal bracketed-paste delivers UTF-8 text
// only — to attach an image the user drops the file's *path* (drag-
// onto-terminal, "copy as path", whatever). We accept it iff the
// payload is a single trimmed line, names a regular file under the
// workspace's filesystem, and starts with one of the recognised
// image magic-byte prefixes.
const char* detect_image_media_type(std::string_view bytes) noexcept {
    auto u = [&](std::size_t i){ return static_cast<unsigned char>(bytes[i]); };
    // PNG: 89 50 4E 47 0D 0A 1A 0A
    if (bytes.size() >= 8 && u(0) == 0x89 && u(1) == 0x50 && u(2) == 0x4E
        && u(3) == 0x47 && u(4) == 0x0D && u(5) == 0x0A && u(6) == 0x1A
        && u(7) == 0x0A) return "image/png";
    // JPEG: FF D8 FF
    if (bytes.size() >= 3 && u(0) == 0xFF && u(1) == 0xD8 && u(2) == 0xFF)
        return "image/jpeg";
    // GIF87a / GIF89a
    if (bytes.size() >= 6
        && bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F'
        && bytes[3] == '8' && (bytes[4] == '7' || bytes[4] == '9')
        && bytes[5] == 'a') return "image/gif";
    // WEBP: "RIFF" .... "WEBP"
    if (bytes.size() >= 12
        && bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F' && bytes[3] == 'F'
        && bytes[8] == 'W' && bytes[9] == 'E' && bytes[10] == 'B' && bytes[11] == 'P')
        return "image/webp";
    return nullptr;
}

// Returns (path, media_type) if the paste looks like a single-line
// path to a recognised image file. Empty path on no match.
struct ImagePasteResult {
    std::string  path;
    const char*  media_type = nullptr;
    std::string  body;       // raw image bytes
};

// Normalise a pasted path candidate:
//   – expand a leading `~/` to $HOME (file managers / shells emit this)
//   – unescape `\ ` / `\(` / `\)` / `\'` etc. (drag-drop on macOS and
//     several Linux file managers backslash-escape every shell-special
//     character so the path can be re-pasted into a shell verbatim)
//   – drop CR if a CRLF terminal slipped one through
std::string normalize_path_candidate(std::string_view in) {
    // Trim trailing CR.
    while (!in.empty() && (in.back() == '\r' || in.back() == ' ')) in.remove_suffix(1);
    std::string s;
    s.reserve(in.size());
    if (in.size() >= 2 && in[0] == '~' && in[1] == '/') {
        if (const char* home = std::getenv("HOME"); home && *home) {
            s.append(home);
            in.remove_prefix(1);  // keep the '/'
        }
    }
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\\' && i + 1 < in.size()) {
            // Shell-style escape. Drop the backslash, keep the next char.
            s.push_back(in[i + 1]);
            ++i;
            continue;
        }
        s.push_back(in[i]);
    }
    return s;
}

ImagePasteResult sniff_image_paste(std::string_view text) {
    ImagePasteResult r;
    // Trim leading / trailing whitespace (incl. CR).
    std::size_t a = 0, b = text.size();
    while (a < b && std::isspace(static_cast<unsigned char>(text[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(text[b - 1]))) --b;
    auto trimmed = text.substr(a, b - a);
    if (trimmed.empty()) return r;
    // Must be a single line — embedded newlines hint at pasted prose
    // rather than a path.
    if (trimmed.find('\n') != std::string_view::npos) return r;
    if (trimmed.size() > 4096) return r;  // sane upper bound

    // Strip surrounding quotes (drag-and-drop on macOS / GNOME quotes
    // paths automatically).
    if (trimmed.size() >= 2
        && (trimmed.front() == '\'' || trimmed.front() == '"')
        && trimmed.back() == trimmed.front()) {
        trimmed = trimmed.substr(1, trimmed.size() - 2);
    }
    // file:// URIs land here on some desktops. Accept either two or
    // three leading slashes (file:// vs file:///).
    constexpr std::string_view kFileUri = "file://";
    if (trimmed.size() > kFileUri.size()
        && trimmed.substr(0, kFileUri.size()) == kFileUri) {
        trimmed.remove_prefix(kFileUri.size());
        // Some emitters use file:/// for absolute paths. Collapse the
        // extra slash so the result starts with exactly one '/'.
        if (!trimmed.empty() && trimmed.front() == '/') {
            // already absolute, fine
        }
    }

    auto candidate = normalize_path_candidate(trimmed);
    if (candidate.empty()) return r;

    fs::path p{candidate};
    std::error_code ec;
    if (!fs::is_regular_file(p, ec) || ec) return r;
    // Sniff first 16 bytes for a magic prefix.
    std::ifstream in(p, std::ios::binary);
    if (!in) return r;
    char buf[16]{};
    in.read(buf, sizeof(buf));
    auto got = static_cast<std::size_t>(in.gcount());
    auto* mt = detect_image_media_type(std::string_view{buf, got});
    if (!mt) return r;
    // Slurp full bytes. 8 MiB cap — Anthropic's per-image limit is
    // 5 MB and base64 expansion adds ~33 %, so anything bigger than
    // ~6 MB on disk would fail server-side anyway.
    auto sz = fs::file_size(p, ec);
    if (ec || sz > 8 * 1024 * 1024) return r;
    in.clear();
    in.seekg(0);
    std::string body(static_cast<std::size_t>(sz), '\0');
    in.read(body.data(), static_cast<std::streamsize>(sz));
    if (in.gcount() != static_cast<std::streamsize>(sz)) return r;

    r.path       = p.string();
    r.media_type = mt;
    r.body       = std::move(body);
    return r;
}

// Build the reverse-chronological list of user-message text refs in
// the active thread. Used by ↑/↓ history walking. We deliberately
// don't deduplicate (a user who typed "y" three times to retry
// expects all three to be visitable — the editing context for each
// was different even if the text was the same).
std::vector<const std::string*> previous_user_texts(const Model& m) {
    std::vector<const std::string*> out;
    out.reserve(m.d.current.messages.size() / 2);
    for (auto it = m.d.current.messages.rbegin();
         it != m.d.current.messages.rend(); ++it) {
        if (it->role == Role::User && !it->text.empty())
            out.push_back(&it->text);
    }
    return out;
}

void apply_history_entry(ComposerState& cs, const std::string& text) {
    cs.text   = text;
    cs.cursor = static_cast<int>(text.size());
    cs.attachments.clear();
    if (text.find('\n') != std::string::npos) cs.expanded = true;
}

} // namespace

using maya::overload;

Step composer_update(Model m, msg::ComposerMsg cm) {
    return std::visit(overload{
        [&](ComposerCharInput e) -> Step {
            // '/' on a fully-empty composer opens the command palette
            // instead of being typed as text — Claude Code / Cursor /
            // every chat-shell muscle memory. The empty-buffer guard
            // (text + attachments + cursor) keeps mid-prose slashes
            // (URLs, regexes, formula divides) from hijacking input.
            // Once the palette is open, subscribe routes subsequent
            // keystrokes to on_command_palette so the slash itself is
            // not consumed twice; the open-palette starts with an
            // empty query.
            if (e.ch == U'/'
                && m.ui.composer.text.empty()
                && m.ui.composer.attachments.empty()
                && m.ui.composer.cursor == 0) {
                m.ui.command_palette = palette::Open{};
                return done(std::move(m));
            }
            // '@' opens the file mention picker. Unlike '/' this is
            // permitted mid-prose ("ping @alice tomorrow" is a fine
            // English sentence), so we don't require an empty buffer
            // — we only require the previous character to be a word
            // boundary (start-of-string or whitespace) so URLs / emails
            // / e.g. "alice@example.com" don't trigger.
            auto at_word_boundary = [&]{
                if (m.ui.composer.cursor == 0) return true;
                char prev = m.ui.composer.text[
                    static_cast<std::size_t>(m.ui.composer.cursor) - 1];
                return prev == ' ' || prev == '\t' || prev == '\n';
            };
            if (e.ch == U'@' && at_word_boundary()) {
                mention::Open o;
                o.files = list_workspace_files();
                m.ui.mention_palette = std::move(o);
                return done(std::move(m));
            }
            // '#' opens the symbol picker — mirrors '@'. The first
            // open walks the workspace and is therefore noticeably
            // slower than '@' on a cold cache; subsequent opens are
            // instant.
            if (e.ch == U'#' && at_word_boundary()) {
                symbol_palette::Open o;
                o.entries = list_workspace_symbols();
                m.ui.symbol_palette = std::move(o);
                return done(std::move(m));
            }
            begin_edit(m.ui.composer);
            auto utf8 = ui::utf8_encode(e.ch);
            m.ui.composer.text.insert(m.ui.composer.cursor, utf8);
            m.ui.composer.cursor += static_cast<int>(utf8.size());
            return done(std::move(m));
        },
        [&](ComposerBackspace) -> Step {
            if (m.ui.composer.cursor > 0 && !m.ui.composer.text.empty()) {
                begin_edit(m.ui.composer);
                // chip_prev jumps over a whole placeholder if the
                // cursor is at the right edge of one — backspace then
                // erases the entire chip token in a single keystroke,
                // which is the user's mental model: "delete the
                // attachment, not the closing sentinel byte." The
                // attachment object stays in the vector by index
                // (renumbering would break other placeholders pointing
                // at later indices); orphans get GC'd when the
                // composer next clears.
                int p = ui::chip_prev(m.ui.composer.text, m.ui.composer.cursor);
                m.ui.composer.text.erase(p, m.ui.composer.cursor - p);
                m.ui.composer.cursor = p;
            }
            return done(std::move(m));
        },
        [&](ComposerEnter)  { return submit_message(std::move(m)); },
        [&](ComposerSubmit) { return submit_message(std::move(m)); },
        [&](ComposerNewline) -> Step {
            begin_edit(m.ui.composer);
            m.ui.composer.text.insert(m.ui.composer.cursor, "\n");
            m.ui.composer.cursor += 1;
            m.ui.composer.expanded = true;
            return done(std::move(m));
        },
        [&](ComposerToggleExpand) -> Step {
            m.ui.composer.expanded = !m.ui.composer.expanded;
            return done(std::move(m));
        },
        [&](ComposerCursorLeft) -> Step {
            m.ui.composer.cursor = ui::chip_prev(m.ui.composer.text, m.ui.composer.cursor);
            return done(std::move(m));
        },
        [&](ComposerCursorRight) -> Step {
            m.ui.composer.cursor = ui::chip_next(m.ui.composer.text, m.ui.composer.cursor);
            return done(std::move(m));
        },
        [&](ComposerCursorHome) -> Step {
            m.ui.composer.cursor = 0;
            return done(std::move(m));
        },
        [&](ComposerCursorEnd) -> Step {
            m.ui.composer.cursor = static_cast<int>(m.ui.composer.text.size());
            return done(std::move(m));
        },
        [&](ComposerCursorWordLeft) -> Step {
            m.ui.composer.cursor = word_left(m.ui.composer.text, m.ui.composer.cursor);
            return done(std::move(m));
        },
        [&](ComposerCursorWordRight) -> Step {
            m.ui.composer.cursor = word_right(m.ui.composer.text, m.ui.composer.cursor);
            return done(std::move(m));
        },
        [&](ComposerKillToEndOfLine) -> Step {
            const auto& s = m.ui.composer.text;
            int n = static_cast<int>(s.size());
            int p = m.ui.composer.cursor;
            if (p >= n) return done(std::move(m));
            int q = p;
            while (q < n && s[q] != '\n') ++q;
            // Standard readline: Ctrl+K on a line of text deletes to
            // EOL exclusive of '\n'; on an empty line it deletes the
            // newline itself (collapses the empty line away).
            if (q == p && q < n && s[q] == '\n') ++q;
            begin_edit(m.ui.composer);
            m.ui.composer.text.erase(p, q - p);
            return done(std::move(m));
        },
        [&](ComposerKillToBeginningOfLine) -> Step {
            const auto& s = m.ui.composer.text;
            int p = m.ui.composer.cursor;
            if (p <= 0) return done(std::move(m));
            int q = p;
            while (q > 0 && s[q - 1] != '\n') --q;
            if (q == p) return done(std::move(m));
            begin_edit(m.ui.composer);
            m.ui.composer.text.erase(q, p - q);
            m.ui.composer.cursor = q;
            return done(std::move(m));
        },
        [&](ComposerUndo) -> Step {
            if (m.ui.composer.undo_stack.empty()) return done(std::move(m));
            ComposerState::Snapshot cur;
            cur.text   = std::move(m.ui.composer.text);
            cur.cursor = m.ui.composer.cursor;
            cur.attachments = std::move(m.ui.composer.attachments);
            auto prev = std::move(m.ui.composer.undo_stack.back());
            m.ui.composer.undo_stack.pop_back();
            if (m.ui.composer.redo_stack.size() >= kUndoDepth)
                m.ui.composer.redo_stack.erase(m.ui.composer.redo_stack.begin());
            m.ui.composer.redo_stack.push_back(std::move(cur));
            m.ui.composer.text        = std::move(prev.text);
            m.ui.composer.cursor      = prev.cursor;
            m.ui.composer.attachments = std::move(prev.attachments);
            reset_history(m.ui.composer);
            return done(std::move(m));
        },
        [&](ComposerRedo) -> Step {
            if (m.ui.composer.redo_stack.empty()) return done(std::move(m));
            ComposerState::Snapshot cur;
            cur.text   = std::move(m.ui.composer.text);
            cur.cursor = m.ui.composer.cursor;
            cur.attachments = std::move(m.ui.composer.attachments);
            auto next = std::move(m.ui.composer.redo_stack.back());
            m.ui.composer.redo_stack.pop_back();
            if (m.ui.composer.undo_stack.size() >= kUndoDepth)
                m.ui.composer.undo_stack.erase(m.ui.composer.undo_stack.begin());
            m.ui.composer.undo_stack.push_back(std::move(cur));
            m.ui.composer.text        = std::move(next.text);
            m.ui.composer.cursor      = next.cursor;
            m.ui.composer.attachments = std::move(next.attachments);
            reset_history(m.ui.composer);
            return done(std::move(m));
        },
        [&](ComposerHistoryPrev) -> Step {
            // First ↑ from the live draft — snapshot whatever the user
            // had typed so ↓ all the way back restores it.
            auto texts = previous_user_texts(m);
            if (texts.empty()) return done(std::move(m));
            int next_idx = m.ui.composer.history_idx + 1;
            if (next_idx >= static_cast<int>(texts.size()))
                next_idx = static_cast<int>(texts.size()) - 1;
            if (m.ui.composer.history_idx < 0) {
                m.ui.composer.draft_save = m.ui.composer.text;
            }
            m.ui.composer.history_idx = next_idx;
            apply_history_entry(m.ui.composer, *texts[
                static_cast<std::size_t>(next_idx)]);
            // History walk does NOT push undo: ↑↓ alone are
            // non-destructive (they leave draft_save intact). Once
            // the user edits, begin_edit fires reset_history and
            // the walked text becomes the new live draft.
            return done(std::move(m));
        },
        [&](ComposerHistoryNext) -> Step {
            if (m.ui.composer.history_idx < 0) return done(std::move(m));
            auto texts = previous_user_texts(m);
            int next_idx = m.ui.composer.history_idx - 1;
            if (next_idx < 0) {
                // Walked all the way back to the live draft.
                m.ui.composer.history_idx = -1;
                m.ui.composer.text = m.ui.composer.draft_save.value_or(std::string{});
                m.ui.composer.cursor = static_cast<int>(m.ui.composer.text.size());
                m.ui.composer.draft_save.reset();
                return done(std::move(m));
            }
            m.ui.composer.history_idx = next_idx;
            if (next_idx < static_cast<int>(texts.size())) {
                apply_history_entry(m.ui.composer,
                                    *texts[static_cast<std::size_t>(next_idx)]);
            }
            return done(std::move(m));
        },
        [&](ComposerImagePasteFromClipboard) -> Step {
            // Bracketed paste delivers UTF-8 text only; for an image-
            // on-clipboard the only working path on a vanilla terminal
            // is to ASK the OS clipboard for its image content via a
            // platform tool. read_clipboard_image() shells out (see
            // clipboard_image.cpp for the per-OS commands) and returns
            // the raw bytes + sniffed media-type. Sync because the
            // helpers all exit immediately on a no-image clipboard;
            // worst-case is ~200 ms when osascript / PowerShell is on
            // the path.
            std::string err;
            auto img = read_clipboard_image(&err);
            if (!img) {
                auto cmd = set_status_toast(m,
                    err.empty() ? std::string{"no image on clipboard"}
                                : std::move(err),
                    std::chrono::seconds{6});
                return {std::move(m), std::move(cmd)};
            }
            begin_edit(m.ui.composer);
            Attachment att;
            att.kind       = Attachment::Kind::Image;
            att.path       = "<clipboard>";
            att.media_type = std::move(img->media_type);
            att.byte_count = img->bytes.size();
            att.body       = std::move(img->bytes);
            std::size_t idx = m.ui.composer.attachments.size();
            m.ui.composer.attachments.push_back(std::move(att));
            auto placeholder = attachment::make_placeholder(idx);
            m.ui.composer.text.insert(m.ui.composer.cursor, placeholder);
            m.ui.composer.cursor += static_cast<int>(placeholder.size());
            m.ui.composer.expanded = true;
            return done(std::move(m));
        },
        [&](ComposerPaste& e) -> Step {
            // Bracketed-paste of raw image bytes — vanishingly rare
            // (most terminals scrub binary out of paste), but Kitty
            // and Wezterm with `--allow-passthrough`-style options
            // will hand us the bytes verbatim. Detect by magic prefix
            // and ingest as Image. Tested before path-detection so a
            // PNG that happens to start with bytes that could parse
            // as a path takes the right branch.
            if (auto* mt = detect_image_media_type(e.text); mt != nullptr) {
                begin_edit(m.ui.composer);
                Attachment att;
                att.kind       = Attachment::Kind::Image;
                att.path       = "<paste>";
                att.media_type = mt;
                att.byte_count = e.text.size();
                att.body       = std::move(e.text);
                std::size_t idx = m.ui.composer.attachments.size();
                m.ui.composer.attachments.push_back(std::move(att));
                auto placeholder = attachment::make_placeholder(idx);
                m.ui.composer.text.insert(m.ui.composer.cursor, placeholder);
                m.ui.composer.cursor += static_cast<int>(placeholder.size());
                m.ui.composer.expanded = true;
                return done(std::move(m));
            }

            // Image-path paste: a single-line path naming a real image
            // file becomes an Image attachment. Try this first so a
            // path under 800 bytes doesn't get inlined as plain text.
            // Sniff the first 16 bytes for a known magic prefix; only
            // ingest if it actually looks like an image.
            if (auto img = sniff_image_paste(e.text); !img.path.empty()) {
                begin_edit(m.ui.composer);
                Attachment att;
                att.kind       = Attachment::Kind::Image;
                att.path       = std::move(img.path);
                att.media_type = img.media_type;
                att.byte_count = img.body.size();
                att.body       = std::move(img.body);
                std::size_t idx = m.ui.composer.attachments.size();
                m.ui.composer.attachments.push_back(std::move(att));
                auto placeholder = attachment::make_placeholder(idx);
                m.ui.composer.text.insert(m.ui.composer.cursor, placeholder);
                m.ui.composer.cursor += static_cast<int>(placeholder.size());
                m.ui.composer.expanded = true;
                return done(std::move(m));
            }

            // Empty paste (clipboard manager hiccup, terminal dropped
            // a binary clipboard) — nothing to do.
            if (e.text.empty()) return done(std::move(m));

            // Normalize line endings: some terminals (and ssh tty cooked
            // mode) translate \n → \r in bracketed paste so the bytes
            // look like the user pressed Enter at every line. Maya's
            // word_wrap splits on \n only, so leaving \r in the body
            // collapses the whole paste into one logical line that
            // wraps based on width — visible as a giant single-row
            // user message with stray rendering, *not* the line-by-line
            // render the user expects. Canvas::write_text skips \r as
            // a control char so the cells don't get the carriage-return
            // byte either; without this normalization the paste both
            // looks wrong AND throws the parent layout off (the layout
            // sees one wrapped line, but the cached/measured height
            // mismatch leaves visible gaps between the user turn and
            // the assistant turn). Strip stray \r and convert \r-only
            // / \r\n to \n; pure-\n input is unchanged.
            {
                std::string norm;
                norm.reserve(e.text.size());
                for (std::size_t i = 0; i < e.text.size(); ++i) {
                    char c = e.text[i];
                    if (c == '\r') {
                        norm.push_back('\n');
                        if (i + 1 < e.text.size() && e.text[i + 1] == '\n')
                            ++i;
                    } else {
                        norm.push_back(c);
                    }
                }
                e.text = std::move(norm);
            }

            // Always-chip: every paste, regardless of size, becomes
            // an Attachment + inline placeholder. Single-line pastes
            // get a preview caption ("Pasted: hello world") so they
            // stay legible; multi-line pastes get the lines/bytes
            // summary. Inline-as-text was a UX regression for any
            // paste with structure — the previous threshold-based
            // collapse mixed two presentations and made the composer
            // height twitch with paste size.
            const std::size_t lines =
                std::ranges::count(e.text, '\n')
                + (e.text.back() == '\n' ? 0 : 1);

            begin_edit(m.ui.composer);
            Attachment att;
            att.kind       = Attachment::Kind::Paste;
            att.line_count = lines;
            att.byte_count = e.text.size();
            att.body       = std::move(e.text);
            std::size_t idx = m.ui.composer.attachments.size();
            m.ui.composer.attachments.push_back(std::move(att));
            auto placeholder = attachment::make_placeholder(idx);
            m.ui.composer.text.insert(m.ui.composer.cursor, placeholder);
            m.ui.composer.cursor += static_cast<int>(placeholder.size());
            if (lines > 1) m.ui.composer.expanded = true;
            return done(std::move(m));
        },
        [&](ComposerRecallQueued) -> Step {
            // No-op when there's nothing to recall — the caller (the
            // Up-arrow keymap) only emits this when the queue is
            // non-empty, but the predicate is racy across frames so
            // be defensive.
            if (m.ui.composer.queued.empty()) return done(std::move(m));

            // Drain the queue into the composer, joined by '\n', and
            // append any pre-existing composer text after another
            // '\n'. Mirrors Claude Code's `Lc_` (binary offset
            // 76303220): a single ↑ press drains the WHOLE editable
            // queue into one composer load — no per-item cycling.
            // Multi-line queued items keep their newlines so a paste
            // that became a queued message survives the recall
            // round-trip.
            std::string recalled;
            for (std::size_t i = 0; i < m.ui.composer.queued.size(); ++i) {
                if (i > 0) recalled.push_back('\n');
                recalled += m.ui.composer.queued[i];
            }
            // Cursor lands at the boundary between recalled text and
            // the user's pre-existing composer input — exactly where
            // they'd want to start editing or appending. (Claude
            // Code's seam is `O.join("\n").length + 1 + _`; we use
            // the same idea: end-of-recalled + 1 if there's anything
            // after, else end-of-recalled.)
            int boundary = static_cast<int>(recalled.size());
            if (!m.ui.composer.text.empty()) {
                recalled.push_back('\n');
                ++boundary;
                recalled += m.ui.composer.text;
            }
            begin_edit(m.ui.composer);
            m.ui.composer.text   = std::move(recalled);
            m.ui.composer.cursor = boundary;
            // Multi-line content → flip expanded so the composer's
            // `expanded` cap (16 rows) takes effect, not the 8-row
            // unexpanded cap. Same trigger as ComposerPaste.
            if (m.ui.composer.text.find('\n') != std::string::npos)
                m.ui.composer.expanded = true;
            // Destructive recall: queued items now live ONLY in the
            // composer buffer. Re-submit re-queues at the tail (fresh
            // tail position). Clearing the composer drops them. Same
            // trade-off as Claude Code — keeps the data model simple
            // (no "soft-deleted, recallable" intermediate state).
            m.ui.composer.queued.clear();
            return done(std::move(m));
        },
    }, cm);
}

} // namespace agentty::app::detail
