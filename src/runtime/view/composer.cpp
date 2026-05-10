#include "agentty/runtime/view/composer.hpp"

#include <algorithm>
#include <cstdio>

#include "agentty/runtime/composer_attachment.hpp"
#include "agentty/runtime/view/helpers.hpp"
#include "agentty/runtime/view/palette.hpp"

namespace agentty::ui {

namespace {

// Map agentty runtime state → widget State enum. Pure data translation;
// the widget owns all visual decisions (border color, prompt boldness,
// placeholder text, height pin).
maya::Composer::State composer_state(const Model& m) {
    if (m.s.is_awaiting_permission()) return maya::Composer::State::AwaitingPermission;
    if (m.s.is_executing_tool())      return maya::Composer::State::ExecutingTool;
    if (m.s.is_streaming())           return maya::Composer::State::Streaming;
    return maya::Composer::State::Idle;
}

// Walk the composer text, replacing each placeholder token with a
// human-readable chip caption ("[Pasted text · 412 lines · 14 KB]" /
// "[@src/auth/login.cpp]"). Translate the agentty-space cursor into
// view-space simultaneously: any agentty cursor position is — by the
// chip-aware navigation in update/composer.cpp — always at a chip
// boundary, never inside a token, so the mapping is well-defined.
struct DisplayText {
    std::string text;
    int         cursor = 0;
};
DisplayText render_chips(std::string_view src, int agentty_cursor,
                         const std::vector<Attachment>& attachments) {
    DisplayText out;
    out.text.reserve(src.size());
    int i = 0;
    int n = static_cast<int>(src.size());
    int cur = std::clamp(agentty_cursor, 0, n);
    while (i < n) {
        if (i == cur) out.cursor = static_cast<int>(out.text.size());
        if (static_cast<unsigned char>(src[i]) == 0x01) {
            auto len = attachment::placeholder_len_at(
                src, static_cast<std::size_t>(i));
            if (len > 0) {
                auto idx = attachment::placeholder_index(
                    src, static_cast<std::size_t>(i));
                std::string chip = "[?]";
                if (idx < attachments.size()) {
                    chip = "[" + attachment::chip_label(attachments[idx]) + "]";
                }
                out.text.append(chip);
                i += static_cast<int>(len);
                continue;
            }
        }
        out.text.push_back(src[i++]);
    }
    if (cur >= n) out.cursor = static_cast<int>(out.text.size());
    return out;
}

// Visually clip lines that are absurdly wide. The maya Composer
// word-wraps at terminal width, so an 800-byte single-line paste
// (URL, base64 blob, hash) produces ~10 wrapped rows and dominates
// the composer's vertical space. We replace the BODY of any line
// over `kVisibleLineWidth` bytes with `head … tail (N chars)` —
// purely visual, the underlying buffer (which is what gets sent to
// the model) is untouched. The cursor's host line is NEVER clipped
// so the user can edit inside what they pasted.
//
// Discipline: this runs *after* render_chips so the byte counts here
// already reflect chip captions, not raw placeholders. We work in
// bytes, not display columns — close enough for ASCII-heavy pastes
// (URLs / hashes) and we don't owe perfect grapheme accounting for a
// purely cosmetic clip.
constexpr int kVisibleLineWidth = 160;
constexpr int kHeadKeep         = 80;
constexpr int kTailKeep         = 40;

DisplayText clip_long_lines(DisplayText in) {
    DisplayText out;
    out.text.reserve(in.text.size());
    int n   = static_cast<int>(in.text.size());
    int cur = in.cursor;
    int i   = 0;
    while (i < n) {
        int j = i;
        while (j < n && in.text[j] != '\n') ++j;
        int line_len = j - i;
        bool cursor_on_line = cur >= i && cur <= j;
        if (line_len > kVisibleLineWidth && !cursor_on_line) {
            out.text.append(in.text, i, kHeadKeep);
            out.text.append(" \xe2\x80\xa6 ");                  // " … "
            out.text.append(in.text, j - kTailKeep, kTailKeep);
            // Always-on suffix so the user knows there's more under
            // the visual clip ("(412 chars)" / "(1.2 KB)").
            char suf[32];
            if (line_len < 1024)
                std::snprintf(suf, sizeof(suf), " (%d chars)", line_len);
            else
                std::snprintf(suf, sizeof(suf), " (%.1f KB)",
                              static_cast<double>(line_len) / 1024.0);
            out.text.append(suf);
        } else {
            // Cursor is on this line OR the line is short enough to
            // render verbatim. If the cursor is on a clipped-eligible
            // line we surface it as-is so editing keeps working — the
            // composer's word-wrap will still chunk it, but at least
            // the bytes the user is poking at are visible.
            if (cursor_on_line && cur > i) {
                // Translate the cursor offset into the OUT buffer.
                // out has gained no extra bytes for this line, so the
                // delta is just (cur - i) added to current out size.
                int new_cursor = static_cast<int>(out.text.size()) + (cur - i);
                out.cursor = new_cursor;
            }
            out.text.append(in.text, i, line_len);
        }
        if (j < n) {
            out.text.push_back('\n');
            // Cursor exactly on the trailing newline of a clipped line
            // — placement onto the post-clip representation is well-
            // defined as end-of-clipped-line; cursor_on_line caught it
            // above for unclipped path. For the clipped path we deal
            // with it here: if the cursor sits at j (the '\n') and the
            // line was clipped, drop it on the newline we just emitted.
            if (cur == j && line_len > kVisibleLineWidth)
                out.cursor = static_cast<int>(out.text.size()) - 1;
        }
        i = j + 1;
        if (j == n) break;  // no trailing newline; we're done
    }
    // Cursor at end-of-buffer.
    if (cur >= n) out.cursor = static_cast<int>(out.text.size());
    return out;
}

} // namespace

maya::Composer::Config composer_config(const Model& m) {
    maya::Composer::Config cfg;
    auto disp = clip_long_lines(render_chips(m.ui.composer.text,
                                             m.ui.composer.cursor,
                                             m.ui.composer.attachments));
    cfg.text            = std::move(disp.text);
    cfg.cursor          = disp.cursor;
    cfg.state           = composer_state(m);
    cfg.active_color    = phase_color(m.s.phase);
    cfg.text_color      = fg;
    cfg.accent_color    = accent;
    cfg.warn_color      = warn;
    cfg.highlight_color = highlight;
    cfg.queued          = m.ui.composer.queued.size();
    cfg.profile         = {.label = std::string{profile_label(m.d.profile)},
                           .color = profile_color(m.d.profile)};
    cfg.expanded        = m.ui.composer.expanded;
    return cfg;
}

} // namespace agentty::ui
