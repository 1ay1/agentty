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
    Color sign_c = add ? success : del ? danger : muted;

    // Build the WHOLE line as one string + per-byte-range StyledRuns, so it
    // renders as a SINGLE text element that clips its tail cleanly at the pane
    // width — an hstack of per-span text cells would instead squeeze every span
    // when the line overflows (mangled). This is how maya's DiffView stays
    // clean, applied with our syntax highlighting.
    std::string content;
    std::vector<StyledRun> runs;

    // Gutter (right-aligned line number) + sign marker.
    std::string gut = dl.lineno > 0 ? std::to_string(dl.lineno) : "";
    gut.insert(gut.begin(), static_cast<std::size_t>(std::max(0, gutter_w - (int)gut.size())), ' ');
    gut += " ";
    runs.push_back({content.size(), gut.size(), Style{}.with_fg(muted).with_dim()});
    content += gut;
    std::string sign_s = add ? "+ " : del ? "- " : "  ";
    runs.push_back({content.size(), sign_s.size(), Style{}.with_fg(sign_c)});
    content += sign_s;

    // Syntax-highlighted code. Removed + context lines render dimmed so the
    // eye tracks the additions.
    const std::size_t code_off = content.size();
    content += dl.code;
    auto spans = syntax::highlight(dl.code, lang);
    const auto& theme = syntax::themes::terminal;
    auto push = [&](std::size_t from, std::size_t to, syntax::Capture cap) {
        if (to <= from) return;
        Style st = theme.style_for(cap);
        if (del || dl.sign == ' ') st = st.with_dim();
        runs.push_back({code_off + from, to - from, st});
    };
    std::size_t pos = 0;
    for (const auto& sp : spans) {
        if (sp.start > pos) push(pos, sp.start, syntax::Capture::None);
        push(sp.start, std::min<std::size_t>(sp.start + sp.len, dl.code.size()),
             sp.cap);
        pos = sp.start + sp.len;
    }
    if (pos < dl.code.size()) push(pos, dl.code.size(), syntax::Capture::None);

    return Element{TextElement{.content = std::move(content),
                               .wrap = TextWrap::TruncateEnd,
                               .runs = std::move(runs)}};
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
    // Built as ONE styled TextElement (TruncateEnd) so with many files it clips
    // cleanly at the pane edge instead of squeezing every name.
    {
        std::string content = "  ";
        std::vector<StyledRun> runs;
        auto seg = [&](const std::string& s, Style st) {
            runs.push_back({content.size(), s.size(), st}); content += s;
        };
        for (int i = 0; i < static_cast<int>(m.d.pending_changes.size()); ++i) {
            const auto& f = m.d.pending_changes[static_cast<std::size_t>(i)];
            bool anyrej = false, anypend = false;
            for (const auto& hk : f.hunks) {
                if (hk.status == Hunk::Status::Rejected) anyrej = true;
                if (hk.status == Hunk::Status::Pending)  anypend = true;
            }
            Hunk::Status roll = anypend ? Hunk::Status::Pending
                              : anyrej  ? Hunk::Status::Rejected
                                        : Hunk::Status::Accepted;
            const bool cur = (i == fidx);
            std::string name = f.path;
            if (auto sl = name.rfind('/'); sl != std::string::npos) name = name.substr(sl + 1);
            if (i > 0) seg("   ", Style{});
            seg(std::string(status_dot(roll)) + " ", Style{}.with_fg(status_color(roll)));
            seg(name, cur ? Style{}.with_fg(fg).with_bold() : Style{}.with_fg(muted));
            seg(std::format(" +{}", f.added), Style{}.with_fg(success).with_dim());
            seg(std::format(" -{}", f.removed), Style{}.with_fg(danger).with_dim());
        }
        rows.push_back(Element{TextElement{.content = std::move(content),
                                           .wrap = TextWrap::TruncateEnd,
                                           .runs = std::move(runs)}});
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

        // Per-hunk diffstat for the collapsed summary.
        int hadd = 0, hrem = 0;
        for (const auto& dl : parse_hunk(hk.patch)) {
            if (dl.sign == '+') ++hadd; else if (dl.sign == '-') ++hrem;
        }

        // Hunk header: "▸ hunk N/M  ● status   +A −R". The caret marks the
        // focused hunk; collapsed hunks show only this one line so a huge
        // changeset stays navigable instead of a 200-row wall.
        rows.push_back(h(
            text("  "),
            text(sel ? "\xe2\x96\xb8 " : "  ", sel ? fg_bold(accent) : fg_of(muted)),
            text(std::format("hunk {}/{}  ", hi + 1, fc.hunks.size()),
                 sel ? fg_bold(fg) : fg_dim(muted)),
            text(status_dot(hk.status), fg_of(status_color(hk.status))),
            text(" "),
            text(hk.status == Hunk::Status::Accepted ? "accepted"
               : hk.status == Hunk::Status::Rejected ? "rejected" : "pending",
                 fg_of(status_color(hk.status))),
            text(std::format("   +{} ", hadd), fg_dim(success)),
            text(std::format("-{}", hrem), fg_dim(danger))
        ).build());

        // Only the FOCUSED hunk expands. Others stay collapsed to the header
        // above — the SOTA large-review model (GitHub/Zed): one hunk in view,
        // j/k walks the list.
        if (!sel) continue;

        // Cap a single huge hunk's height so it can't blow past the viewport;
        // show a "+N more lines" tail so nothing is silently hidden.
        constexpr int kMaxHunkRows = 24;
        auto lines = parse_hunk(hk.patch);
        int shown = 0;
        for (const auto& dl : lines) {
            if (shown >= kMaxHunkRows) break;
            rows.push_back((diff_code_line(dl, lang, gut_w) | padding(0, 0, 0, 4)).build());
            ++shown;
        }
        if (static_cast<int>(lines.size()) > kMaxHunkRows)
            rows.push_back(text(std::format(
                "      \xe2\x80\xa6 {} more lines in this hunk",
                static_cast<int>(lines.size()) - kMaxHunkRows), fg_dim(muted)));
        rows.push_back(text(""));
    }
    rows.push_back(rule(muted));

    // ── Footer: phone-friendly keys, destructive actions tinted ──
    rows.push_back(key_hints({
        {"j/k", "hunk", 6},
        {"h/l", "file", 5, m.d.pending_changes.size() > 1 ? fg : muted},
        {"Y", "accept", 7, success},
        {"N", "reject", 7, danger},
        {"^A", "all",  4, success},
        {"^X", "none", 3, danger},
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
