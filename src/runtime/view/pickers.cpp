#include "agentty/runtime/view/pickers.hpp"

#include <algorithm>
#include <cstdlib>
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
    // Prefer the real ioctl height. When it's unavailable (no tty: a pipe,
    // a test harness, `agentty | cat`) ws_col is 0 and the query returns
    // maya's hardcoded {80,24} fallback — which is NOT the real viewport. In
    // that no-tty case only, fall back to the LINES env var (the same way
    // maya's view build-phase resolves dims), so the picker clamp uses the
    // true height and never overflows a short viewport. A valid ioctl always
    // wins over LINES (which may be stale). (This is also what routes the
    // scrollback fuzz's simulated term_h into the clamp — without it the
    // picker is sized against a phantom 24-row terminal and strands rows on
    // small shapes.)
    int term_rows = sz.height.value;
    const bool have_tty = maya::platform::is_tty(
        maya::platform::stdout_handle());
    if (!have_tty) {
        if (const char* lines_env = std::getenv("LINES")) {
            if (const int n = std::atoi(lines_env); n > 0) term_rows = n;
        }
    }
    if (term_rows <= 0) term_rows = 40;
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

    // Live search header — mirrors the command palette. Type to filter,
    // Backspace to trim; the row list below is the filtered subset.
    cfg.header.push_back(h(text("\xf0\x9f\x94\x8d ", fg_of(muted)),
        text(picker->query.empty() ? "type to filter models\xe2\x80\xa6" : picker->query,
             picker->query.empty() ? fg_italic(muted) : fg_of(fg))
    ).build());
    cfg.header.push_back(sep);

    // Build the filtered index list (case-insensitive substring over the
    // display name). Empty query → every row, in order.
    std::vector<int> vis;
    vis.reserve(m.d.available_models.size());
    for (int i = 0; i < static_cast<int>(m.d.available_models.size()); ++i)
        if (pick::fuzzy_contains(
                m.d.available_models[static_cast<std::size_t>(i)].display_name,
                picker->query))
            vis.push_back(i);

    if (m.d.available_models.empty()) {
        cfg.items.push_back(text(
            m.s.models_loading
                ? "  Loading models\xe2\x80\xa6"
                : "  No models available \xe2\x80\x94 check the provider/key, then Esc.",
            fg_italic(muted)));
    } else if (vis.empty()) {
        cfg.items.push_back(text("  no models match", fg_italic(muted)));
    } else {
        cfg.rows.reserve(vis.size());
        for (int vi = 0; vi < static_cast<int>(vis.size()); ++vi) {
            const auto& mi = m.d.available_models[
                static_cast<std::size_t>(vis[static_cast<std::size_t>(vi)])];
            const bool sel    = vi == picker->index;
            const bool active = mi.id == m.d.model_id;
            const auto caps   = ModelCapabilities::from_id(mi.id.value);
            Picker::Config::Row row;
            row.leading        = mi.display_name;
            row.leading_style  = active ? fg_bold(fg) : fg_of(muted);
            // Trailing: favourite star, plus the reasoning-effort tier on the
            // highlighted row when the model supports it (←/→ cycles it).
            std::string trailing = mi.favorite ? "\xe2\x98\x85" : "";
            if (sel && caps.supports_effort() && m.d.effort != Effort::None) {
                if (!trailing.empty()) trailing += "  ";
                trailing += "\xe2\x97\x87 " + std::string{effort_label(m.d.effort)};
            }
            row.trailing       = std::move(trailing);
            row.trailing_style = fg_of(warn);
            row.selected = sel;
            row.active   = active;
            cfg.rows.push_back(std::move(row));
        }
    }

    cfg.footer.push_back(text(""));
    // Reasoning-effort line: shown only when the highlighted model supports
    // effort. Names the current tier and the ←/→ binding that cycles it.
    if (!vis.empty()) {
        const int hi = std::clamp(picker->index, 0,
            static_cast<int>(vis.size()) - 1);
        const auto caps = ModelCapabilities::from_id(
            m.d.available_models[
                static_cast<std::size_t>(vis[static_cast<std::size_t>(hi)])].id.value);
        if (caps.supports_effort())
            cfg.footer.push_back(h(
                text("\xe2\x86\x90\xe2\x86\x92", fg_of(fg)),
                text(" reasoning effort: ", fg_dim(muted)),
                text(std::string{effort_label(m.d.effort)}, fg_bold(accent))
            ).build());
    }
    cfg.footer.push_back(key_hints({
        {"\xe2\x86\x91\xe2\x86\x93", "move", 5},        // ↑↓
        {"type", "filter", 2},
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

    // Virtual trailing row: "Custom host…" — opens a free-text endpoint
    // entry for any OpenAI-compatible server (llama.cpp, vLLM, a remote
    // host:port). Kept out of the registry so preset_for / from_spec stay
    // clean; the reducer maps this row index to the CustomHostInput modal.
    {
        Picker::Config::Row row;
        row.leading        = std::string{"Custom host\xe2\x80\xa6  "}
                           + "any OpenAI-compatible server (host:port)";
        row.leading_style  = fg_of(muted);
        row.trailing       = "\xe2\x9c\x8e edit";   // ✎ edit
        row.trailing_style = fg_of(info);
        row.selected       = (i == picker->index);
        cfg.rows.push_back(std::move(row));
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
            const bool is_current = (t.id == m.d.current.id);
            Picker::Config::Row row;
            // "● " marks the thread you're IN — the anchor for both the
            // picker and the ^←→ / Alt+←→ quick-cycle. Non-current rows
            // get a two-space gutter so titles stay column-aligned.
            row.leading        = (is_current ? "\xe2\x97\x8f " : "  ")
                               + (t.title.empty() ? "(untitled)" : t.title);
            row.leading_style  = is_current ? fg_bold(info) : fg_of(muted);
            row.trailing       = timestamp_full(t.updated_at);
            row.trailing_style = fg_dim(muted);
            row.selected = (i == picker->index);
            cfg.rows.push_back(std::move(row));
            ++i;
        }
    }

    cfg.footer.push_back(text(""));
    // Positional readout — same "k/N" the ^←→ / Alt+←→ toast shows, so
    // the two navigation surfaces speak one coordinate system.
    if (!m.d.threads.empty()) {
        cfg.footer.push_back(text(
            "  " + std::to_string(picker->index + 1) + "/"
                + std::to_string(m.d.threads.size()),
            fg_dim(muted)));
    }
    cfg.footer.push_back(key_hints({
        {"\xe2\x86\x91\xe2\x86\x93", "move", 5},        // ↑↓
        {"PgUp/PgDn", "page", 2},
        {"Enter", "open", 5},
        {"N", "new", 3},
        {"^/Alt+\xe2\x86\x90\xe2\x86\x92", "cycle", 1},   // ^←→ / Alt+←→
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

// Ctrl+G — code blocks from the newest assistant reply. Row = ①-style
// index + language tag + first-line preview + line count. The digit
// shortcut in the key handler maps 1-based onto these rows, so the
// leading number is the affordance that teaches the fast path.
Element code_block_picker(const Model& m) {
    auto* o = code_block_picker_opened(m.ui.code_blocks);
    if (!o) return nothing();

    Picker::Config cfg;
    cfg.title      = " Run Code Block ";
    cfg.accent     = success;
    cfg.min_width  = 60;
    cfg.viewport_h = picker_viewport_h();
    cfg.scroll     = &m.ui.code_blocks_scroll;
    cfg.selected   = o->blocks.empty() ? -1 : o->index;

    cfg.rows.reserve(o->blocks.size());
    for (int i = 0; i < static_cast<int>(o->blocks.size()); ++i) {
        const auto& b = o->blocks[static_cast<std::size_t>(i)];
        // First line of the (cleaned) body as the preview — what the
        // user visually matches against the reply on screen.
        std::string_view body{b.body};
        auto eol = body.find('\n');
        std::string preview{body.substr(0, eol == std::string_view::npos
                                             ? body.size() : eol)};
        const bool runnable = code_block_picker::is_shell_language(b.language);
        Picker::Config::Row row;
        row.leading = std::to_string(i + 1) + "  " + preview;
        row.leading_style  = runnable ? fg_of(fg) : fg_dim(muted);
        row.trailing = (b.language.empty() ? std::string{"sh"} : b.language)
                     + " \xc2\xb7 " + std::to_string(b.line_count)
                     + (b.line_count == 1 ? " line" : " lines");
        row.trailing_style = fg_dim(muted);
        row.selected = (i == o->index);
        cfg.rows.push_back(std::move(row));
    }

    cfg.footer.push_back(text(""));
    cfg.footer.push_back(key_hints({
        {"\xe2\x86\x91\xe2\x86\x93", "move", 5},        // ↑↓
        {"Enter/1-9", "run", 3},
        {"e", "edit", 4},
        {"y", "copy", 4},
        {"Esc", "close", 4},
    }));

    return Picker{std::move(cfg)}.build();
}

// Post-run result card. The user already watched the full output live
// on the real terminal (and it remains in native scrollback above the
// TUI); this card shows the summary — command, exit code, size — plus
// the LAST few lines (errors live at the end) and the decision keys.
Element code_block_result_card(const Model& m) {
    auto* r = code_block_result(m.ui.code_blocks);
    if (!r) return nothing();

    const bool ok_exit = !r->timed_out && r->exit_code == 0;

    Picker::Config cfg;
    cfg.title      = " Run Result ";
    cfg.accent     = ok_exit ? success : danger;
    cfg.min_width  = 60;
    cfg.viewport_h = picker_viewport_h();
    cfg.scroll     = &m.ui.code_blocks_scroll;
    cfg.selected   = -1;   // read-only — no cursor row

    // Header: "$ command" + status line.
    {
        std::string cmd_line = r->command;
        // First line only — a multi-line block reads badly in a header.
        if (auto eol = cmd_line.find('\n'); eol != std::string::npos) {
            cmd_line.resize(eol);
            cmd_line += " \xe2\x80\xa6";   // …
        }
        cfg.header.push_back(h(text("$ ", fg_bold(cfg.accent)),
                               text(std::move(cmd_line), fg_of(fg))).build());
        std::size_t lines = r->output.empty() ? 0 : 1;
        for (char c : r->output) if (c == '\n') ++lines;
        std::string status = r->timed_out
            ? "timed out"
            : "exit " + std::to_string(r->exit_code);
        status += " \xc2\xb7 " + std::to_string(lines) + " lines \xc2\xb7 ";
        status += (r->output.size() >= 1024)
            ? std::to_string(r->output.size() / 1024) + " KB"
            : std::to_string(r->output.size()) + " B";
        cfg.header.push_back(text("  " + status,
            ok_exit ? fg_of(muted) : fg_bold(danger)));
        cfg.header.push_back(sep);
    }

    // Full capture in the scrollable region — the user can page through
    // everything before deciding. Line Elements are cheap; the picker's
    // viewport paints only the visible slice.
    {
        std::string_view out{r->output};
        if (out.empty()) {
            cfg.items.push_back(text("  (no output captured)", fg_italic(muted)));
        } else {
            std::size_t pos = 0;
            while (pos <= out.size()) {
                std::size_t eol = out.find('\n', pos);
                std::size_t len = (eol == std::string_view::npos ? out.size() : eol) - pos;
                cfg.items.push_back(text("  " + std::string{out.substr(pos, len)},
                                         fg_of(muted)));
                if (eol == std::string_view::npos) break;
                pos = eol + 1;
            }
        }
    }

    cfg.footer.push_back(text(""));
    cfg.footer.push_back(key_hints({
        {"a", "attach to composer", 6},
        {"y", "copy", 4},
        {"Esc", "discard", 4},
    }));

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
