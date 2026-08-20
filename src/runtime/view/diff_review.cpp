#include "agentty/runtime/view/diff_review.hpp"

#include <algorithm>
#include <format>
#include <vector>

#include <maya/widget/diff_view.hpp>

#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/hints.hpp"

namespace agentty::ui {

using namespace maya;
using namespace maya::dsl;

namespace {
const char* hunk_status_tag(Hunk::Status s) {
    switch (s) {
        case Hunk::Status::Accepted: return "[\u2713 accepted]";
        case Hunk::Status::Rejected: return "[\u2717 rejected]";
        case Hunk::Status::Pending:  return "[ pending ]";
    }
    return "";
}

maya::Color hunk_status_color(Hunk::Status s) {
    switch (s) {
        case Hunk::Status::Accepted: return success;
        case Hunk::Status::Rejected: return danger;
        case Hunk::Status::Pending:  return warn;
    }
    return muted;
}
} // namespace

Element diff_review(const Model& m) {
    // A ONE-line horizontal rule. maya's dsl `sep` is a zero-height box bordered
    // on BOTH edges, so it paints as TWO lines — too heavy inside this pane.
    auto rule = [] {
        return component([](int w, int) -> Element {
            std::string s; s.reserve(static_cast<std::size_t>(std::max(0, w)) * 3);
            for (int i = 0; i < w; ++i) s += "\xe2\x94\x80";   // ─
            return text(std::move(s), fg_dim(muted));
        });
    };
    auto* cursor = pick::opened(m.ui.diff_review);
    if (!cursor || m.d.pending_changes.empty()) return nothing();
    const auto idx = std::min<int>(cursor->file_index,
                                   static_cast<int>(m.d.pending_changes.size()) - 1);
    const auto& fc = m.d.pending_changes[idx];

    // ── Overall progress across EVERY file's hunks ── so a big review shows
    // "you've decided 6 of 20", not just which file you're on.
    int total_hunks = 0, decided = 0, accepted = 0;
    for (const auto& f : m.d.pending_changes)
        for (const auto& hk : f.hunks) {
            ++total_hunks;
            if (hk.status != Hunk::Status::Pending) ++decided;
            if (hk.status == Hunk::Status::Accepted) ++accepted;
        }
    const bool all_done = (decided == total_hunks && total_hunks > 0);

    std::vector<Element> rows;

    // ── File header: path · +/- · file N/M · overall hunk progress ──
    rows.push_back(h(
        text(fc.path, fg_bold(fg)),
        spacer(),
        text(std::format("+{}", fc.added), fg_of(success)),
        text(" "),
        text(std::format("-{}", fc.removed), fg_of(danger)),
        text("   "),
        text(std::format("file {}/{}", cursor->file_index + 1,
                         m.d.pending_changes.size()), fg_dim(muted))
    ).build());
    // Progress bar row: decided / total hunks, with a tiny inline gauge.
    {
        constexpr int kBarW = 16;
        int filled = total_hunks == 0 ? 0
                   : (decided * kBarW + total_hunks / 2) / total_hunks;
        std::string bar;
        for (int i = 0; i < kBarW; ++i)
            bar += (i < filled) ? "\xe2\x96\x88" : "\xe2\x96\x91";  // █ / ░
        rows.push_back(h(
            text(bar, all_done ? fg_of(success) : fg_of(accent)),
            text("  "),
            text(std::format("{}/{} hunks reviewed", decided, total_hunks),
                 fg_dim(muted)),
            spacer(),
            all_done ? text(std::string("\xe2\x9c\x93 all reviewed \xe2\x80\x94 Esc applies"),
                            fg_bold(success))
                     : text(std::format("{} to accept", accepted), fg_dim(muted))
        ).build());
    }
    rows.push_back(rule());

    int hi = 0;
    for (const auto& h_ : fc.hunks) {
        const bool sel      = hi == cursor->hunk_index;
        const bool resolved = h_.status != Hunk::Status::Pending;
        // A decided hunk recedes (dim) so the eye lands on what's still
        // pending; the selected hunk always stays bright.
        auto hdr_style = sel ? fg_bold(accent)
                       : resolved ? fg_dim(muted)
                       : fg_of(muted);
        rows.push_back(h(
            sel ? text("\u203A ", fg_bold(accent)) : text("  "),
            text(std::format("@@ -{},{} +{},{}", h_.old_start, h_.old_len,
                             h_.new_start, h_.new_len), hdr_style),
            text("  "),
            text(hunk_status_tag(h_.status), fg_of(hunk_status_color(h_.status)))
        ).build());
        DiffView dv(fc.path, h_.patch);
        // Fade the diff body of a decided hunk so pending hunks dominate.
        Element body = (resolved && !sel)
            ? (v(dv.build()) | padding(0, 0, 0, 2) | dim()).build()
            : (v(dv.build()) | padding(0, 0, 0, 2)).build();
        rows.push_back(std::move(body));
        ++hi;
    }
    rows.push_back(rule());

    // ── Footer: responsive key hints, destructive actions in the danger hue
    // so "reject" / "none" never read like "accept". Uses the shared key_hints
    // strip (drops low-priority hints first when the pane is narrow).
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
