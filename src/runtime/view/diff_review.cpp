// Diff-review pane — the multi-file hunk review overlay (Ctrl+R).
//
// Layout (all heights clamped so the pane NEVER exceeds the terminal
// viewport — same stranding discipline as pickers.cpp: an overlay taller
// than the base box grows the frame and strands rows in native
// scrollback on close):
//
//   ┌─ Review Changes ── 4/9 decided ─┐
//   │ ~ src/foo.cpp   +12 -7   file 2/3│   header: kind icon + path + stats
//   │ ── files ───────────────────────│
//   │   + src/new.cpp        +45 -0 ✓ │   file strip (status per file)
//   │ ▸ ~ src/foo.cpp        +12 -7 ◐ │
//   │ ─────────────────────────────── │
//   │   @@ -3,4 +3,5      ✓ accepted  │   prior hunks: header line only
//   │ ▸ @@ -42,8 +43,9    · pending   │   focused hunk: header + body
//   │   │ 42  ctx line                │
//   │   │     -old line               │
//   │   │ 43  +new line               │
//   │   … 12 more lines (PgDn)        │
//   │ ─────────────────────────────── │
//   │ ↑↓ hunk  ←→ file  y/n decide …  │   footer hints
//   └─────────────────────────────────┘
//
// Interaction model (reducer: update/diff.cpp): y/n decide + auto-advance
// to the next pending hunk anywhere in the set; u undoes; f/d decide the
// whole file; Enter finishes (pending⇒accepted, materialise, close);
// Esc parks (decided files apply, undecided stay for later).

#include "agentty/runtime/view/diff_review.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <maya/platform/io.hpp>

#include "agentty/runtime/view/palette.hpp"

namespace agentty::ui {

using namespace maya;
using namespace maya::dsl;

namespace {

// ── Status vocabulary ────────────────────────────────────────────────

const char* hunk_status_word(Hunk::Status s) {
    switch (s) {
        case Hunk::Status::Accepted: return "\u2713 accepted";
        case Hunk::Status::Rejected: return "\u2717 rejected";
        case Hunk::Status::Pending:  return "\u00b7 pending";
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

// File kind icon: + created, - deleted, ~ modified.
struct FileKind { const char* icon; maya::Color color; };
FileKind file_kind(const FileChange& fc) {
    if (fc.original_contents.empty()) return {"+", success};
    if (fc.new_contents.empty())      return {"-", danger};
    return {"~", info};
}

// Per-file decision summary: all accepted ✓ / all rejected ✗ / mixed
// decided ◆ / partially decided ◐ / untouched · .
struct FileSummary { const char* glyph; maya::Color color; };
FileSummary file_summary(const FileChange& fc) {
    int acc = 0, rej = 0, pen = 0;
    for (const auto& h : fc.hunks) {
        switch (h.status) {
            case Hunk::Status::Accepted: ++acc; break;
            case Hunk::Status::Rejected: ++rej; break;
            case Hunk::Status::Pending:  ++pen; break;
        }
    }
    if (pen == 0 && rej == 0 && acc > 0) return {"\u2713", success};
    if (pen == 0 && acc == 0 && rej > 0) return {"\u2717", danger};
    if (pen == 0)                        return {"\u25c6", info};      // mixed, decided
    if (acc + rej > 0)                   return {"\u25d0", warn};      // partial
    return {"\u00b7", muted};
}

// ── Viewport budget ──────────────────────────────────────────────────
//
// Same resolution strategy as pickers.cpp::picker_viewport_h: real ioctl
// wins; LINES env only when there is no tty (tests, pipes). The pane's
// TOTAL height (rows incl. border) must fit under the terminal height
// minus a safety margin, and under the base box cap (~24 rows, see
// view.cpp phase 2) so the overlay never extends the painted frame.
int pane_budget_rows() {
    const auto sz = maya::platform::query_terminal_size(
        maya::platform::stdout_handle());
    int term_rows = sz.height.value;
    if (!maya::platform::is_tty(maya::platform::stdout_handle())) {
        if (const char* e = std::getenv("LINES"))
            if (const int n = std::atoi(e); n > 0) term_rows = n;
    }
    if (term_rows <= 0) term_rows = 24;
    // -3: bottom inset (2, view.cpp compose_overlay) + 1 breathing row.
    return std::clamp(term_rows - 3, 10, 21);
}

// ── Diff body (windowed) ─────────────────────────────────────────────
//
// Hand-rolled instead of maya::DiffView because the focused hunk needs a
// WINDOW over its patch (body_scroll + fixed height) — DiffView always
// renders the whole patch, which would blow the height budget on a
// 300-line hunk. Line numbers follow the DiffView convention: new-file
// numbers on context/+ lines, blank gutter on - lines.
struct PatchLine { char tag; std::string_view body; int new_no; };

std::vector<PatchLine> parse_patch(const Hunk& hk) {
    std::vector<PatchLine> out;
    int new_no = hk.new_start;
    std::string_view sv = hk.patch;
    while (!sv.empty()) {
        auto nl = sv.find('\n');
        auto line = (nl == std::string_view::npos) ? sv : sv.substr(0, nl);
        sv = (nl == std::string_view::npos) ? std::string_view{}
                                            : sv.substr(nl + 1);
        if (line.empty()) continue;
        const char tag = line[0];
        const auto body = line.substr(1);
        if (tag == '-') {
            out.push_back({tag, body, 0});
        } else {
            out.push_back({tag, body, new_no});
            ++new_no;
        }
    }
    return out;
}

Element render_patch_line(const PatchLine& pl) {
    // Gutter: "│ 123 " — rail + 4-col new-file line number (blank for -).
    std::string gutter;
    if (pl.new_no > 0) {
        auto num = std::to_string(pl.new_no);
        while (num.size() < 4) num.insert(num.begin(), ' ');
        gutter = num;
    } else {
        gutter = "    ";
    }
    Style body_style;
    std::string lead;
    switch (pl.tag) {
        case '+': body_style = fg_of(success); lead = "+"; break;
        case '-': body_style = fg_of(danger);  lead = "-"; break;
        default:  body_style = fg_dim(text_secondary); lead = " "; break;
    }
    return h(
        text("  \u2502 ", fg_dim(muted)),
        text(std::move(gutter) + " ", fg_dim(muted)),
        (text(lead + std::string(pl.body), body_style) | clip)
    ).build();
}

} // namespace

Element diff_review(const Model& m) {
    auto* cursor = pick::opened(m.ui.diff_review);
    if (!cursor || m.d.pending_changes.empty()) return nothing();

    const int n_files = static_cast<int>(m.d.pending_changes.size());
    const int fi = std::clamp(cursor->file_index, 0, n_files - 1);
    const auto& fc = m.d.pending_changes[static_cast<std::size_t>(fi)];
    const int n_hunks = static_cast<int>(fc.hunks.size());
    const int hi = n_hunks > 0 ? std::clamp(cursor->hunk_index, 0, n_hunks - 1)
                               : 0;

    // Global review progress for the title.
    int total = 0, decided = 0;
    for (const auto& c : m.d.pending_changes)
        for (const auto& h_ : c.hunks) {
            ++total;
            if (h_.status != Hunk::Status::Pending) ++decided;
        }

    // ── Height budget ──────────────────────────────────────────────
    // Fixed chrome: border(2) + header(1) + hunk-strip sep(1) +
    // footer sep(1) + footer(1) = 6. File strip (only when 2+ files):
    // sep(1) + min(n_files,4) rows. Everything left goes to hunks.
    const int budget = pane_budget_rows();
    const bool show_files = n_files > 1;
    const int file_rows = show_files ? std::min(n_files, 4) : 0;
    const int chrome = 6 + (show_files ? file_rows + 1 : 0);
    const int hunk_area = std::max(4, budget - chrome);

    std::vector<Element> rows;

    // ── Header: kind + path + stats + file position ────────────────
    const auto kind = file_kind(fc);
    rows.push_back(h(
        text(std::string(kind.icon) + " ", fg_bold(kind.color)),
        (text(fc.path, fg_bold(code_path)) | clip),
        text("  "),
        text(std::format("+{}", fc.added), fg_of(success)),
        text(" "),
        text(std::format("-{}", fc.removed), fg_of(danger)),
        spacer(),
        text(std::format("file {}/{}", fi + 1, n_files), fg_dim(muted))
    ).build());

    // ── File strip (2+ files): windowed around the focused file ────
    if (show_files) {
        rows.push_back(sep);
        int first = std::clamp(fi - file_rows / 2, 0, n_files - file_rows);
        for (int i = first; i < first + file_rows; ++i) {
            const auto& f = m.d.pending_changes[static_cast<std::size_t>(i)];
            const bool sel = i == fi;
            const auto k = file_kind(f);
            const auto sum = file_summary(f);
            rows.push_back(h(
                sel ? text("\u25b8 ", fg_bold(accent)) : text("  "),
                text(std::string(k.icon) + " ", fg_of(k.color)),
                (text(f.path, sel ? fg_bold(fg) : fg_of(muted)) | clip),
                spacer(),
                text(std::format("+{} ", f.added), fg_dim(success)),
                text(std::format("-{} ", f.removed), fg_dim(danger)),
                text(sum.glyph, fg_of(sum.color))
            ).build());
        }
    }
    rows.push_back(sep);

    // ── Hunk area ──────────────────────────────────────────────────
    if (n_hunks == 0) {
        rows.push_back(text("  (no hunks)", fg_italic(muted)));
    } else {
        // Every hunk gets a 1-row header; the focused hunk additionally
        // shows a body window. Budget: headers first, body gets the rest.
        const int max_headers = std::max(1, std::min(n_hunks, hunk_area / 2));
        int first_h = std::clamp(hi - max_headers / 2, 0,
                                 std::max(0, n_hunks - max_headers));
        const int shown_headers =
            std::min(max_headers, n_hunks - first_h);
        const bool more_above = first_h > 0;
        const bool more_below = first_h + shown_headers < n_hunks;
        int body_h = hunk_area - shown_headers
                   - (more_above ? 1 : 0) - (more_below ? 1 : 0);

        if (more_above)
            rows.push_back(text(
                std::format("  \u2026 {} more hunk{} above", first_h,
                            first_h == 1 ? "" : "s"),
                fg_dim(muted)));

        for (int i = first_h; i < first_h + shown_headers; ++i) {
            const auto& hk = fc.hunks[static_cast<std::size_t>(i)];
            const bool sel = i == hi;
            rows.push_back(h(
                sel ? text("\u25b8 ", fg_bold(accent)) : text("  "),
                text(std::format("@@ -{},{} +{},{}", hk.old_start, hk.old_len,
                                 hk.new_start, hk.new_len),
                     sel ? fg_of(fg) : fg_dim(muted)),
                text("  "),
                text(hunk_status_word(hk.status),
                     fg_of(hunk_status_color(hk.status))),
                spacer(),
                text(sel ? std::format("hunk {}/{}", i + 1, n_hunks)
                         : std::string{},
                     fg_dim(muted))
            ).build());

            if (!sel || body_h <= 1) continue;

            // Focused hunk body — windowed by body_scroll.
            const auto lines = parse_patch(hk);
            const int n_lines = static_cast<int>(lines.size());
            const int max_scroll = std::max(0, n_lines - (body_h - 1));
            const int off = std::clamp(cursor->body_scroll, 0, max_scroll);
            const bool clipped_below = off + body_h - 1 < n_lines;
            const int visible = std::min(n_lines - off,
                                         body_h - (clipped_below ? 1 : 0));
            if (off > 0) {
                // Replace the first visible row with an above-marker
                // only when scrolled; keeps the window height stable.
                rows.push_back(text(
                    std::format("  \u2502 \u2026 {} line{} above", off,
                                off == 1 ? "" : "s"),
                    fg_dim(muted)));
            }
            const int body_start = off + (off > 0 ? 1 : 0);
            for (int l = body_start; l < off + visible; ++l)
                rows.push_back(render_patch_line(
                    lines[static_cast<std::size_t>(l)]));
            if (clipped_below) {
                const int rest = n_lines - (off + visible);
                rows.push_back(text(
                    std::format("  \u2502 \u2026 {} more line{}  PgDn",
                                rest, rest == 1 ? "" : "s"),
                    fg_dim(muted)));
            }
        }

        if (more_below) {
            const int rest = n_hunks - (first_h + shown_headers);
            rows.push_back(text(
                std::format("  \u2026 {} more hunk{} below", rest,
                            rest == 1 ? "" : "s"),
                fg_dim(muted)));
        }
    }

    // ── Footer: key hints ─────────────────────────────────────────
    rows.push_back(sep);
    rows.push_back(h(
        text("↑↓", fg_of(fg)), text(" hunk  ", fg_dim(muted)),
        text("←→", fg_of(fg)), text(" file  ", fg_dim(muted)),
        text("y", fg_of(success)), text("/", fg_dim(muted)),
        text("n", fg_of(danger)), text(" decide  ", fg_dim(muted)),
        text("u", fg_of(fg)), text(" undo  ", fg_dim(muted)),
        text("f", fg_of(success)), text("/", fg_dim(muted)),
        text("d", fg_of(danger)), text(" file  ", fg_dim(muted)),
        text("a", fg_of(success)), text("/", fg_dim(muted)),
        text("x", fg_of(danger)), text(" all  ", fg_dim(muted)),
        text("⏎", fg_of(fg)), text(" finish  ", fg_dim(muted)),
        text("Esc", fg_of(fg)), text(" later", fg_dim(muted))
    ).build());

    // Title carries the global progress so it's visible at a glance.
    const std::string title = total > 0
        ? std::format(" Review Changes \u00b7 {}/{} decided ", decided, total)
        : " Review Changes ";
    const auto content = v(std::move(rows)) | padding(0, 1, 0, 1);
    return (v(content.build())
            | border(BorderStyle::Round)
            | bcolor(decided == total && total > 0 ? success : accent)
            | btext(title, BorderTextPos::Top, BorderTextAlign::Center)
            ).build();
}

} // namespace agentty::ui
