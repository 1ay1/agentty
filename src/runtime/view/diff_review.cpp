#include "agentty/runtime/view/diff_review.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <maya/widget/markdown/highlight.hpp>

#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/hints.hpp"

namespace agentty::ui {

using namespace maya;
using namespace maya::dsl;
namespace syntax = maya::syntax;

namespace {

// ── Language from a file path's extension ────────────────────────────────
[[nodiscard]] syntax::Lang lang_of(std::string_view path) {
    auto dot = path.rfind('.');
    if (dot == std::string_view::npos) return syntax::Lang::Generic;
    return syntax::lang_from_tag(path.substr(dot + 1));
}

// ── Status glyphs ────────────────────────────────────────────────────────
[[nodiscard]] const char* status_dot(Hunk::Status s) {
    switch (s) {
        case Hunk::Status::Accepted: return "\xe2\x97\x8f";  // ● filled
        case Hunk::Status::Rejected: return "\xe2\x9c\x97";  // ✗
        case Hunk::Status::Pending:  return "\xe2\x97\x8b";  // ○ hollow
    }
    return " ";
}
[[nodiscard]] Color status_color(Hunk::Status s) {
    switch (s) {
        case Hunk::Status::Accepted: return success;
        case Hunk::Status::Rejected: return danger;
        case Hunk::Status::Pending:  return warn;
    }
    return muted;
}

// A parsed diff line: its sign, the code text (sign stripped), and the new-side
// line number (0 = none, e.g. a removed line).
struct DiffLine { char sign; std::string code; int lineno; };

[[nodiscard]] std::vector<DiffLine> parse_hunk(std::string_view patch) {
    std::vector<DiffLine> out;
    int newno = 0;
    std::size_t i = 0;
    while (i < patch.size()) {
        std::size_t nl = patch.find('\n', i);
        std::string_view line = patch.substr(i, (nl == std::string_view::npos ? patch.size() : nl) - i);
        i = (nl == std::string_view::npos) ? patch.size() : nl + 1;
        if (line.empty()) continue;
        if (line.starts_with("@@")) {
            // Seed the new-side counter from "+start".
            auto p = line.find('+');
            if (p != std::string_view::npos) {
                newno = 0;
                for (std::size_t k = p + 1; k < line.size() && line[k] >= '0' && line[k] <= '9'; ++k)
                    newno = newno * 10 + (line[k] - '0');
            }
            continue;   // the @@ header itself isn't rendered as a code line
        }
        char sign = line[0];
        std::string code{line.substr(1)};
        int ln = 0;
        if (sign == '+' || sign == ' ') ln = newno++;
        out.push_back({sign, std::move(code), ln});
    }
    return out;
}

// Syntax-highlight one code line into styled runs, tinted for its diff sign.
// Added/removed lines keep syntax colours but carry a faint add/remove wash via
// the gutter + sign; context lines are dimmed so the eye tracks the changes.
[[nodiscard]] Element diff_code_line(const DiffLine& dl, syntax::Lang lang,
                                     int gutter_w) {
    const bool add = dl.sign == '+', del = dl.sign == '-';
    // Gutter: line number (new side) or blank for removed lines.
    std::string gut = dl.lineno > 0 ? std::to_string(dl.lineno) : "";
    gut.insert(gut.begin(), static_cast<std::size_t>(std::max(0, gutter_w - (int)gut.size())), ' ');
    Color sign_c = add ? success : del ? danger : muted;
    std::string sign_s = add ? "+ " : del ? "- " : "  ";

    std::vector<Element> parts;
    parts.push_back(text(gut + " ", fg_dim(muted)));
    parts.push_back(text(sign_s, fg_of(sign_c)));

    // Syntax-highlight the code. Removed lines render dimmer (they're going
    // away); context dimmer still; added lines at full strength.
    auto spans = syntax::highlight(dl.code, lang);
    const auto& theme = syntax::themes::terminal;
    std::size_t pos = 0;
    auto emit = [&](std::size_t from, std::size_t to, syntax::Capture cap) {
        if (to <= from || from >= dl.code.size()) return;
        to = std::min(to, dl.code.size());
        Style st = theme.style_for(cap);
        if (del) st = st.with_dim();          // removed → faded
        else if (dl.sign == ' ') st = st.with_dim();  // context → faded
        parts.push_back(text(dl.code.substr(from, to - from), st));
    };
    for (const auto& sp : spans) {
        if (sp.start > pos) emit(pos, sp.start, syntax::Capture::None);
        emit(sp.start, sp.start + sp.len, sp.cap);
        pos = sp.start + sp.len;
    }
    if (pos < dl.code.size()) emit(pos, dl.code.size(), syntax::Capture::None);
    if (dl.code.empty()) parts.push_back(text(std::string{}));

    return h(std::move(parts)).build();
}

} // namespace

Element diff_review(const Model& m) {
    auto rule = [](Color c) {
        return component([c](int w, int) -> Element {
            std::string s; s.reserve(static_cast<std::size_t>(std::max(0, w)) * 3);
            for (int i = 0; i < w; ++i) s += "\xe2\x94\x80";   // ─
            return text(std::move(s), fg_dim(c));
        });
    };
    auto* cursor = pick::opened(m.ui.diff_review);
    if (!cursor || m.d.pending_changes.empty()) return nothing();
    const int fidx = std::min<int>(cursor->file_index,
                                   static_cast<int>(m.d.pending_changes.size()) - 1);
    const auto& fc = m.d.pending_changes[static_cast<std::size_t>(fidx)];

    // ── Overall progress across every file's hunks ──
    int total_hunks = 0, decided = 0, accepted = 0;
    for (const auto& f : m.d.pending_changes)
        for (const auto& hk : f.hunks) {
            ++total_hunks;
            if (hk.status != Hunk::Status::Pending) ++decided;
            if (hk.status == Hunk::Status::Accepted) ++accepted;
        }
    const bool all_done = (decided == total_hunks && total_hunks > 0);

    std::vector<Element> rows;

    // ── File rail: every file, its net status + diffstat, current one lit ──
    // Shows the WHOLE changeset at a glance (SOTA: you never lose the forest).
    {
        std::vector<Element> rail;
        rail.push_back(text("  "));
        for (int i = 0; i < static_cast<int>(m.d.pending_changes.size()); ++i) {
            const auto& f = m.d.pending_changes[static_cast<std::size_t>(i)];
            // Per-file roll-up status: all-accepted / any-rejected / pending.
            bool anyrej = false, anypend = false;
            for (const auto& hk : f.hunks) {
                if (hk.status == Hunk::Status::Rejected) anyrej = true;
                if (hk.status == Hunk::Status::Pending)  anypend = true;
            }
            Hunk::Status roll = anypend ? Hunk::Status::Pending
                              : anyrej  ? Hunk::Status::Rejected
                                        : Hunk::Status::Accepted;
            const bool cur = (i == fidx);
            // basename only — the rail is a compact index, not full paths.
            std::string name = f.path;
            if (auto sl = name.rfind('/'); sl != std::string::npos) name = name.substr(sl + 1);
            if (i > 0) rail.push_back(text("   "));
            rail.push_back(text(std::string(status_dot(roll)) + " ",
                                fg_of(status_color(roll))));
            rail.push_back(text(name, cur ? fg_bold(fg) : fg_of(muted)));
            rail.push_back(text(std::format(" +{}", f.added), fg_dim(success)));
            rail.push_back(text(std::format(" -{}", f.removed), fg_dim(danger)));
        }
        rows.push_back(h(std::move(rail)).build());
    }

    // ── Progress bar + counts ──
    {
        constexpr int kBarW = 18;
        int filled = total_hunks == 0 ? 0
                   : (decided * kBarW + total_hunks / 2) / total_hunks;
        std::string bar;
        for (int i = 0; i < kBarW; ++i) bar += (i < filled) ? "\xe2\x96\x88" : "\xe2\x96\x91";
        rows.push_back(h(
            text("  "),
            text(bar, all_done ? fg_of(success) : fg_of(accent)),
            text("  "),
            text(std::format("{}/{} reviewed", decided, total_hunks), fg_dim(muted)),
            spacer(),
            text("  "),
            all_done ? text(std::string("\xe2\x9c\x93 all reviewed \xe2\x80\x94 Esc applies"),
                            fg_bold(success))
                     : text(std::format("{} to accept", accepted), fg_dim(muted)),
            text("  ")
        ).build());
    }
    rows.push_back(rule(muted));

    // ── The current file's hunks, syntax-highlighted ──
    const syntax::Lang lang = lang_of(fc.path);
    // Gutter width from the largest new-side line number in this file.
    int max_ln = 1;
    for (const auto& hk : fc.hunks)
        max_ln = std::max(max_ln, hk.new_start + hk.new_len);
    const int gut_w = std::max(2, static_cast<int>(std::to_string(max_ln).size()));

    for (int hi = 0; hi < static_cast<int>(fc.hunks.size()); ++hi) {
        const auto& hk = fc.hunks[static_cast<std::size_t>(hi)];
        const bool sel = hi == cursor->hunk_index;

        // Hunk divider: "▸ hunk N/M  ●" — the cursor caret + a status dot, in
        // the status hue. Selected hunk's caret is bright; others muted.
        rows.push_back(h(
            text("  "),
            text(sel ? "\xe2\x96\xb8 " : "  ", sel ? fg_bold(accent) : fg_of(muted)),
            text(std::format("hunk {}/{}  ", hi + 1, fc.hunks.size()),
                 sel ? fg_bold(fg) : fg_dim(muted)),
            text(status_dot(hk.status), fg_of(status_color(hk.status))),
            text(" "),
            text(hk.status == Hunk::Status::Accepted ? "accepted"
               : hk.status == Hunk::Status::Rejected ? "rejected" : "pending",
                 fg_of(status_color(hk.status)))
        ).build());

        for (const auto& dl : parse_hunk(hk.patch)) {
            Element line = diff_code_line(dl, lang, gut_w) | padding(0, 0, 0, 4);
            if (hk.status != Hunk::Status::Pending && !sel) line = std::move(line) | dim();
            rows.push_back(line.build());
        }
        if (hi + 1 < static_cast<int>(fc.hunks.size())) rows.push_back(text(""));
    }
    rows.push_back(rule(muted));

    // ── Footer: phone-friendly keys, destructive actions tinted ──
    rows.push_back(key_hints({
        {"j/k", "hunk", 6},
        {"h/l", "file", 5, m.d.pending_changes.size() > 1 ? fg : muted},
        {"Y", "accept", 7, success},
        {"N", "reject", 7, danger},
        {"A", "all",    4, success},
        {"X", "none",   3, danger},
        {"Esc", all_done ? "apply" : "close", 8, all_done ? success : fg},
    }));

    auto content = (v(std::move(rows)) | padding(1, 2));
    return (v(content.build())
            | border(BorderStyle::Round)
            | bcolor(all_done ? success : muted)
            | btext(" Review Changes ", BorderTextPos::Top, BorderTextAlign::Center)
            ).build();
}

} // namespace agentty::ui
