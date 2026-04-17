#include "moha/view/diff_review.hpp"

#include <algorithm>
#include <format>
#include <vector>

#include <maya/widget/diff_view.hpp>

#include "moha/view/palette.hpp"

namespace moha::ui {

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
    if (!m.diff_review.open || m.pending_changes.empty()) return text("");
    const auto idx = std::min<int>(m.diff_review.file_index,
                                   static_cast<int>(m.pending_changes.size()) - 1);
    const auto& fc = m.pending_changes[idx];

    std::vector<Element> rows;
    rows.push_back(h(
        text("File ", fg_of(muted)),
        text(std::format("{}/{}", m.diff_review.file_index + 1, m.pending_changes.size()),
             fg_of(fg)),
        text("  "),
        text(fc.path, fg_bold(fg)),
        spacer(),
        text(std::format("+{} -{}", fc.added, fc.removed), fg_of(success))
    ).build());
    rows.push_back(sep);

    int hi = 0;
    for (const auto& h_ : fc.hunks) {
        bool sel = hi == m.diff_review.hunk_index;
        rows.push_back(h(
            sel ? text("\u203A ", fg_bold(accent)) : text("  "),
            text(std::format("hunk @@ -{} +{}", h_.old_start, h_.new_start),
                 fg_of(muted)),
            text("  "),
            text(hunk_status_tag(h_.status), fg_of(hunk_status_color(h_.status)))
        ).build());
        DiffView dv(fc.path, h_.patch);
        rows.push_back((v(dv.build()) | padding(0, 0, 0, 2)).build());
        ++hi;
    }
    rows.push_back(sep);
    rows.push_back(text("\u2191\u2193 hunk  \u2190\u2192 file  Y accept  N reject  A all  X none  Esc close",
                        fg_dim(muted)));
    auto content = (v(std::move(rows)) | padding(1, 2));
    return (v(content.build())
            | border(BorderStyle::Round) | bcolor(muted)
            | btext(" Review Changes ")).build();
}

} // namespace moha::ui
