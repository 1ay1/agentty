#include "agentty/runtime/view/pickers.hpp"

#include <algorithm>
#include <vector>

#include <maya/widget/picker.hpp>
#include <maya/widget/plan_view.hpp>
#include <maya/platform/io.hpp>

#include "agentty/runtime/view/helpers.hpp"
#include "agentty/runtime/view/palette.hpp"
#include "agentty/provider/registry.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/workspace/files.hpp"
#include "agentty/workspace/symbols.hpp"

// Pure adapter: builds maya::Picker::Config values from Model state. The
// widget owns every chrome decision — border style, viewport clipping,
// scrollbar glyph + thumb math, keep-selection-in-view auto-scroll. agentty
// supplies only the row-level Elements (each picker formats its items
// differently — favourite stars, timestamps, parent-dir disambiguators)
// and the typed cursor index.
//
// Per-row truncation rides on `text(...) | clip` (TextWrap::TruncateEnd):
// maya measures the column it allocated to the row and returns a
// truncated-with-ellipsis single line if the natural content overflows.

namespace agentty::ui {

using namespace maya;
using namespace maya::dsl;

namespace {

// "src/runtime/foo.cpp" → ("foo.cpp", "src/runtime/").
// Returns ("foo.cpp", "") for a bare filename.
std::pair<std::string_view, std::string_view>
split_name_dir(std::string_view path) {
    auto slash = path.find_last_of('/');
    if (slash == std::string_view::npos) return {path, {}};
    return {path.substr(slash + 1), path.substr(0, slash + 1)};
}

// Compress a directory path to its IMMEDIATE parent only — that's
// the disambiguator the user actually scans for. Truncation of the
// segment itself is left to maya (`| clip`), so this just performs
// the semantic step ("/home/.../Best Of Kumar Sanu/" → "Kumar Sanu/").
std::string parent_segment(std::string_view dir) {
    if (dir.empty()) return {};
    auto inner = dir;
    if (inner.back() == '/') inner.remove_suffix(1);
    auto slash = inner.find_last_of('/');
    auto last = (slash == std::string_view::npos)
        ? inner : inner.substr(slash + 1);
    std::string out{last};
    out.push_back('/');
    return out;
}

// Viewport height (rows) for every picker's scrollable list. Single
// constant so all pickers share the same shape. Items beyond this
// reachable via the scrollbar; selection always stays visible via
// the widget's auto-scroll-to-selection logic.
constexpr int kViewportH = 14;

// Picker chrome around the scrollable list: top border + title row +
// a blank + the two-row footer (blank + hint line) + bottom border, plus
// the AppLayout outer padding. ~7 rows. The picker floats bottom-pinned
// in a zstack over the base (welcome / conversation); maya's stack layout
// extends the frame's content height to whichever layer is taller. If the
// picker's TOTAL height (list + chrome) exceeds the terminal viewport,
// opening it pushes the base's top rows (the welcome wordmark) above the
// viewport top, scrolling them into native terminal scrollback via the
// bottom-edge \r\n the inline renderer uses to grow. Closing the picker
// shrinks the frame again, but those scrolled-off rows are owned by the
// emulator and can't be reclaimed (only reset_inline's \x1b[3J could, and
// that wipes the user's shell history) — so EACH open/close cycle strands
// another copy of the wordmark above the welcome ("the wordmark gets
// longer with every picker").
//
// Fix: clamp the list viewport so the WHOLE picker fits inside the
// terminal viewport — then opening it never overflows, never scrolls, and
// nothing strands. On a tall terminal this is a no-op (kViewportH wins);
// on a short one the list shrinks (still scrollable to reach every item).
constexpr int kPickerChromeRows = 7;

[[nodiscard]] int picker_viewport_h() {
    const auto sz = maya::platform::query_terminal_size(
        maya::platform::stdout_handle());
    const int term_rows = sz.height.value > 0 ? sz.height.value : 40;
    // Leave the chrome plus a small breathing margin so the picker's top
    // border sits strictly below the viewport top with the base behind it.
    const int avail = term_rows - kPickerChromeRows - 1;
    // Floor of 4 list rows keeps the picker usable even on a tiny term
    // (it scrolls); ceiling is the shared kViewportH.
    return std::clamp(avail, 4, kViewportH);
}

// One key-binding hint in a footer strip: a key glyph + a short label,
// plus a priority that decides survival order when the picker is too
// narrow to show them all (higher = kept longer).
struct Hint {
    std::string key;
    std::string label;
    int         priority = 0;
};

// Responsive key-hint footer. Renders the hints on a SINGLE line as
// "key label   key label   …"; when the available width can't fit them
// all, the lowest-priority hints drop out (rightmost wins ties) and the
// survivors keep their original left-to-right order. Never wraps.
//
// The maya Picker already clips footers to one line, so this can't break
// the modal even at width 1 — but dropping whole hints degrades far more
// gracefully than truncating a binding mid-word. The component reads the
// width maya allocated to the footer row, so the drop set re-evaluates
// live as the terminal resizes.
[[nodiscard]] Element key_hints(std::vector<Hint> hints) {
    return component([hints = std::move(hints)](int w, int) -> Element {
        if (w <= 0 || hints.empty()) return nothing();
        constexpr int gap = 3;   // columns between adjacent hints
        auto pair_w = [](const Hint& hn) {
            return string_width(hn.key) + 1 + string_width(hn.label);
        };
        std::vector<bool> keep(hints.size(), true);
        auto total = [&] {
            int sum = 0, shown = 0;
            for (std::size_t i = 0; i < hints.size(); ++i)
                if (keep[i]) { sum += pair_w(hints[i]); ++shown; }
            if (shown > 1) sum += gap * (shown - 1);
            return sum;
        };
        // Greedily evict the lowest-priority kept hint until the strip
        // fits (or nothing is left).
        while (total() > w) {
            int victim = -1;
            for (std::size_t i = 0; i < hints.size(); ++i) {
                if (!keep[i]) continue;
                if (victim < 0 || hints[i].priority <= hints[static_cast<std::size_t>(victim)].priority)
                    victim = static_cast<int>(i);
            }
            if (victim < 0) break;
            keep[static_cast<std::size_t>(victim)] = false;
        }
        std::vector<Element> parts;
        bool first = true;
        for (std::size_t i = 0; i < hints.size(); ++i) {
            if (!keep[i]) continue;
            if (!first) parts.push_back(text(std::string(gap, ' ')));
            first = false;
            parts.push_back(text(hints[i].key + " ", fg_of(fg)));
            parts.push_back(text(hints[i].label, fg_dim(muted)));
        }
        if (parts.empty()) return nothing();
        return h(std::move(parts)).build();
    });
}

} // namespace

Element model_picker(const Model& m) {
    auto* picker = pick::opened(m.ui.model_picker);
    if (!picker) return nothing();

    Picker::Config cfg;
    cfg.title      = " Models ";
    cfg.accent     = accent;
    cfg.min_width  = 40;
    cfg.viewport_h = picker_viewport_h();
    cfg.scroll     = &m.ui.model_picker_scroll;
    cfg.selected   = picker->index;

    if (m.d.available_models.empty()) {
        cfg.items.push_back(text(
            m.s.models_loading
                ? "  Loading models…"
                : "  No models available — check the provider/key, then Esc.",
            fg_italic(muted)));
    } else {
        cfg.rows.reserve(m.d.available_models.size());
        int i = 0;
        for (const auto& mi : m.d.available_models) {
            const bool sel    = i == picker->index;
            const bool active = mi.id == m.d.model_id;
            const auto caps   = ModelCapabilities::from_id(mi.id.value);
            Picker::Config::Row row;
            row.leading        = mi.display_name;
            row.leading_style  = active ? fg_bold(fg) : fg_of(muted);
            // Trailing: favourite star, plus the reasoning-effort tier on the
            // highlighted row when the model supports it (←/→ cycles it).
            std::string trailing = mi.favorite ? "★" : "";
            if (sel && caps.supports_effort() && m.d.effort != Effort::None) {
                if (!trailing.empty()) trailing += "  ";
                trailing += "◇ " + std::string{effort_label(m.d.effort)};
            }
            row.trailing       = std::move(trailing);
            row.trailing_style = fg_of(warn);
            row.selected = sel;
            row.active   = active;
            cfg.rows.push_back(std::move(row));
            ++i;
        }
    }

    cfg.footer.push_back(text(""));
    // Reasoning-effort line: shown only when the highlighted model supports
    // effort. Names the current tier and the ←/→ binding that cycles it.
    if (!m.d.available_models.empty()) {
        const int hi = std::clamp(picker->index, 0,
            static_cast<int>(m.d.available_models.size()) - 1);
        const auto caps = ModelCapabilities::from_id(
            m.d.available_models[static_cast<std::size_t>(hi)].id.value);
        if (caps.supports_effort())
            cfg.footer.push_back(h(
                text("←→", fg_of(fg)),
                text(" reasoning effort: ", fg_dim(muted)),
                text(std::string{effort_label(m.d.effort)}, fg_bold(accent))
            ).build());
    }
    cfg.footer.push_back(key_hints({
        {"\xe2\x86\x91\xe2\x86\x93", "move", 5},        // ↑↓
        {"PgUp/PgDn", "page", 2},
        {"Enter", "select", 5},
        {"F", "favorite", 1},
        {"Esc", "close", 4},
    }));

    return Picker{std::move(cfg)}.build();
}

namespace {

// Resolve the currently-active provider id so the picker can mark the
// active row. Anthropic when kind==Anthropic, else the endpoint label.
[[nodiscard]] std::string active_provider_id() {
    const auto& sel = provider::active();
    if (sel.kind == provider::Kind::OpenAI) return sel.openai_endpoint.label;
    return std::string{provider::default_provider_id()};
}

// Per-row auth status: does this provider have credentials available?
// Local backends need none; Anthropic is assumed authed (creds resolved at
// startup / via login); OpenAI-family checks its env-var chain.

} // namespace

Element provider_picker(const Model& m) {
    auto* picker = pick::opened(m.ui.provider_picker);
    if (!picker) return nothing();

    Picker::Config cfg;
    cfg.title      = " Providers ";
    cfg.accent     = highlight;
    cfg.min_width  = 52;
    cfg.viewport_h = picker_viewport_h();
    cfg.scroll     = &m.ui.provider_picker_scroll;
    cfg.selected   = picker->index;

    const std::string active_id = active_provider_id();
    const auto presets = provider::providers();

    auto env_has = [](std::string_view name) -> bool {
        if (name.empty()) return false;
        const char* v = std::getenv(std::string{name}.c_str());
        return v && *v;
    };

    cfg.rows.reserve(presets.size());
    int i = 0;
    for (const auto& p : presets) {
        const bool active = (p.id == active_id);
        const bool sel    = (i == picker->index);

        // Auth status for the trailing column.
        std::string note;
        maya::Color note_color = muted;
        if (p.is_local || p.auth == provider::AuthStyle::None) {
            note = "● local";
            note_color = info;
        } else if (p.kind == provider::Kind::Anthropic) {
            note = "✓ login";
            note_color = success;
        } else {
            bool have = false;
            std::string_view via;
            for (auto env : p.auth_env) {
                if (env_has(env)) { have = true; via = env; break; }
            }
            if (have) { note = "✓ " + std::string{via}; note_color = success; }
            else {
                // Name the first (provider-specific) env var the user should set.
                std::string_view want = p.auth_env.front();
                note = want.empty() ? "⚠ no key"
                                    : "⚠ " + std::string{want};
                note_color = warn;
            }
        }

        Picker::Config::Row row;
        row.leading        = std::string{p.label} + "  "
                           + std::string{p.blurb};
        row.leading_style  = active ? fg_bold(fg) : fg_of(muted);
        row.trailing       = note;
        row.trailing_style = fg_of(note_color);
        row.selected = sel;
        row.active   = active;
        cfg.rows.push_back(std::move(row));
        ++i;
    }

    cfg.footer.push_back(text(""));
    cfg.footer.push_back(h(
        text("✓", fg_of(success)), text(" ready  ", fg_dim(muted)),
        text("⚠", fg_of(warn)),    text(" set the named key first  ", fg_dim(muted))
    ).build());
    cfg.footer.push_back(key_hints({
        {"\xe2\x86\x91\xe2\x86\x93", "move", 5},        // ↑↓
        {"Enter", "switch", 5},
        {"Esc", "close", 4},
    }));

    return Picker{std::move(cfg)}.build();
}

Element thread_list(const Model& m) {
    auto* picker = pick::opened(m.ui.thread_list);
    if (!picker) return nothing();

    Picker::Config cfg;
    cfg.title      = " Threads ";
    cfg.accent     = info;
    cfg.min_width  = 50;
    cfg.viewport_h = picker_viewport_h();
    cfg.scroll     = &m.ui.thread_list_scroll;
    cfg.selected   = picker->index;

    if (m.d.threads.empty()) {
        cfg.items.push_back(text(
            m.s.threads_loading ? "  Loading conversations…"
                                : "  No threads yet.",
            fg_italic(muted)));
    } else {
        cfg.rows.reserve(m.d.threads.size());
        int i = 0;
        for (const auto& t : m.d.threads) {
            Picker::Config::Row row;
            row.leading        = t.title.empty() ? "(untitled)" : t.title;
            row.leading_style  = fg_of(muted);
            row.trailing       = timestamp_full(t.updated_at);
            row.trailing_style = fg_dim(muted);
            row.selected = (i == picker->index);
            cfg.rows.push_back(std::move(row));
            ++i;
        }
    }

    cfg.footer.push_back(text(""));
    cfg.footer.push_back(key_hints({
        {"\xe2\x86\x91\xe2\x86\x93", "move", 5},        // ↑↓
        {"PgUp/PgDn", "page", 2},
        {"Enter", "open", 5},
        {"N", "new", 3},
        {"Esc", "close", 4},
    }));

    return Picker{std::move(cfg)}.build();
}

Element command_palette(const Model& m) {
    auto* o = opened(m.ui.command_palette);
    if (!o) return nothing();

    auto matches = filtered_commands(o->query);

    Picker::Config cfg;
    cfg.title      = " Command Palette ";
    cfg.accent     = highlight;
    cfg.min_width  = 50;
    cfg.viewport_h = picker_viewport_h();
    cfg.scroll     = &m.ui.command_palette_scroll;
    cfg.selected   = matches.empty() ? -1 : o->index;

    cfg.header.push_back(h(text("› ", fg_bold(highlight)),
        text(o->query.empty() ? "type to filter…" : o->query,
             o->query.empty() ? fg_italic(muted) : fg_of(fg))
    ).build());
    cfg.header.push_back(sep);

    if (matches.empty()) {
        cfg.items.push_back(text("  no matches", fg_italic(muted)));
    } else {
        cfg.rows.reserve(matches.size());
        for (int i = 0; i < static_cast<int>(matches.size()); ++i) {
            const auto& cmd = *matches[static_cast<std::size_t>(i)];
            Picker::Config::Row row;
            row.leading        = std::string{cmd.label};
            row.leading_style  = fg_of(muted);
            row.trailing       = std::string{cmd.description};
            row.trailing_style = fg_dim(muted);
            row.selected = (i == o->index);
            cfg.rows.push_back(std::move(row));
        }
    }

    return Picker{std::move(cfg)}.build();
}

Element mention_palette(const Model& m) {
    auto* o = mention_opened(m.ui.mention_palette);
    if (!o) return nothing();

    const auto& matches = mention_filtered(*o);

    Picker::Config cfg;
    cfg.title      = " Mention File ";
    cfg.accent     = info;
    cfg.min_width  = 50;
    cfg.viewport_h = picker_viewport_h();
    cfg.scroll     = &m.ui.mention_palette_scroll;
    cfg.selected   = matches.empty() ? -1 : o->index;

    cfg.header.push_back(h(text("@", fg_bold(info)),
        text(o->query.empty() ? " type to filter files…" : (" " + o->query),
             o->query.empty() ? fg_italic(muted) : fg_of(fg))
    ).build());
    cfg.header.push_back(sep);

    if (o->files.empty()) {
        cfg.items.push_back(text("  workspace empty (or no readable files)", fg_italic(muted)));
    } else if (matches.empty()) {
        cfg.items.push_back(text("  no matches", fg_italic(muted)));
    } else {
        cfg.rows.reserve(matches.size());
        for (int i = 0; i < static_cast<int>(matches.size()); ++i) {
            const auto& path = o->files[matches[static_cast<std::size_t>(i)]];
            auto [name, dir] = split_name_dir(path);
            Picker::Config::Row row;
            row.leading        = std::string{name};
            row.leading_style  = fg_of(fg);
            row.trailing       = parent_segment(dir);
            row.trailing_style = fg_dim(muted);
            row.selected = (i == o->index);
            cfg.rows.push_back(std::move(row));
        }
    }

    // Position indicator: still useful as a textual N/total anchor even
    // though the scrollbar shows the same thing visually.
    if (static_cast<int>(matches.size()) > kViewportH) {
        cfg.footer.push_back(text(
            "  " + std::to_string(o->index + 1) + "/"
                + std::to_string(matches.size()),
            fg_dim(muted)));
    }

    return Picker{std::move(cfg)}.build();
}

Element symbol_palette(const Model& m) {
    auto* o = symbol_palette_opened(m.ui.symbol_palette);
    if (!o) return nothing();

    const auto& matches = symbol_filtered(*o);

    Picker::Config cfg;
    cfg.title      = " Symbol ";
    cfg.accent     = highlight;
    cfg.min_width  = 60;
    cfg.viewport_h = picker_viewport_h();
    cfg.scroll     = &m.ui.symbol_palette_scroll;
    cfg.selected   = matches.empty() ? -1 : o->index;

    cfg.header.push_back(h(text("#", fg_bold(highlight)),
        text(o->query.empty() ? " type to filter symbols…" : (" " + o->query),
             o->query.empty() ? fg_italic(muted) : fg_of(fg))
    ).build());
    cfg.header.push_back(sep);

    if (o->entries.empty()) {
        cfg.items.push_back(text("  no symbols indexed", fg_italic(muted)));
    } else if (matches.empty()) {
        cfg.items.push_back(text("  no matches", fg_italic(muted)));
    } else {
        cfg.rows.reserve(matches.size());
        for (int i = 0; i < static_cast<int>(matches.size()); ++i) {
            const auto& sym = o->entries[matches[static_cast<std::size_t>(i)]];
            auto [fname, dir] = split_name_dir(sym.path);
            Picker::Config::Row row;
            // Combine symbol name + locus into the leading cell so a
            // long parent-dir trailing still has room to render; the
            // "name  file:line" pair is what the user is scanning.
            row.leading        = sym.name + "  " + std::string{fname}
                               + ":" + std::to_string(sym.line_number);
            row.leading_style  = fg_of(fg);
            row.trailing       = parent_segment(dir);
            row.trailing_style = fg_dim(muted);
            row.selected = (i == o->index);
            cfg.rows.push_back(std::move(row));
        }
    }

    if (static_cast<int>(matches.size()) > kViewportH) {
        cfg.footer.push_back(text(
            "  " + std::to_string(o->index + 1) + "/"
                + std::to_string(matches.size()),
            fg_dim(muted)));
    }

    return Picker{std::move(cfg)}.build();
}

Element todo_modal(const Model& m) {
    if (!pick::is_open(m.ui.todo.open)) return nothing();

    Picker::Config cfg;
    cfg.title      = " Plan ";
    cfg.accent     = info;
    cfg.min_width  = 45;
    cfg.viewport_h = picker_viewport_h();
    cfg.scroll     = &m.ui.todo_scroll;
    // No selection cursor in the todo modal — read-only. Pass -1
    // so the auto-scroll-to-selection is a no-op and the user's
    // manual scroll position is fully respected.
    cfg.selected   = -1;

    if (m.ui.todo.items.empty()) {
        cfg.items.push_back(text("  No tasks yet.", fg_italic(muted)));
        cfg.items.push_back(text("  The agent will create tasks as it works.", fg_dim(muted)));
    } else {
        // PlanView returns one Element with all tasks. It lives in
        // the scrollable region so a long task list pages cleanly
        // when it overflows the viewport.
        maya::PlanView plan;
        for (const auto& item : m.ui.todo.items) {
            maya::TaskStatus ts;
            switch (item.status) {
                case TodoStatus::Pending:    ts = maya::TaskStatus::Pending; break;
                case TodoStatus::InProgress: ts = maya::TaskStatus::InProgress; break;
                case TodoStatus::Completed:  ts = maya::TaskStatus::Completed; break;
            }
            plan.add(item.content, ts);
        }
        cfg.items.push_back(plan.build());

        int total = static_cast<int>(m.ui.todo.items.size());
        int done_count = 0;
        for (const auto& item : m.ui.todo.items)
            if (item.status == TodoStatus::Completed) ++done_count;
        cfg.footer.push_back(text(""));
        cfg.footer.push_back(h(
            text("  " + std::to_string(done_count) + "/" + std::to_string(total),
                 fg_bold(done_count == total ? success : info)),
            text(" completed", fg_dim(muted))
        ).build());
    }

    cfg.footer.push_back(text(""));
    cfg.footer.push_back(h(
        text("Esc", fg_of(fg)), text(" close", fg_dim(muted))
    ).build());

    return Picker{std::move(cfg)}.build();
}

} // namespace agentty::ui
