#include "agentty/runtime/view/pickers.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <string_view>
#include <variant>
#include <vector>

#include <maya/widget/picker.hpp>
#include <maya/widget/plan_view.hpp>
#include <maya/widget/tool_body_preview.hpp>
#include <maya/platform/io.hpp>

#include "agentty/runtime/view/helpers.hpp"
#include "agentty/auth/vault.hpp"   // vault::signed_in — uniform OAuth status
#include "agentty/runtime/view/hints.hpp"
#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/code_block_picker.hpp"  // extract_code_blocks (palette gating)
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_helpers.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_args.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_body_preview.hpp"
#include "agentty/provider/registry.hpp"
#include "agentty/provider/chatgpt/responses.hpp"
#include "agentty/provider/copilot/copilot_oauth.hpp"
#include "agentty/provider/kimi/kimi_oauth.hpp"
#include "agentty/runtime/provider_rows.hpp"
#include "agentty/runtime/fused_models.hpp"
#include "agentty/provider/auth_state.hpp"
#include "agentty/provider/credentials.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/runtime/app/deps.hpp"   // deps().auth for the live auth badge
#include "agentty/auth/auth.hpp"          // auth::is_empty
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

namespace ov = agentty::ui::overlay;

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

[[nodiscard]] int picker_terminal_rows() {
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
    return term_rows;
}

[[nodiscard]] int picker_viewport_h() {
    const int term_rows = picker_terminal_rows();
    // Leave the chrome plus a small breathing margin so the picker's top
    // border sits strictly below the viewport top with the base behind it.
    const int avail = term_rows - kPickerChromeRows - 1;
    // Floor of 4 list rows keeps the picker usable even on a tiny term
    // (it scrolls); ceiling is the shared kViewportH.
    return std::clamp(avail, 4, kViewportH);
}

// Terminal WIDTH, resolved the same way picker_terminal_rows() resolves
// height: real ioctl first, COLUMNS only when there is no tty (pipe / test
// harness, where maya's query returns its hardcoded {80,24}).
[[nodiscard]] int picker_terminal_cols() {
    const auto sz = maya::platform::query_terminal_size(
        maya::platform::stdout_handle());
    int cols = sz.width.value;
    const bool have_tty = maya::platform::is_tty(
        maya::platform::stdout_handle());
    if (!have_tty) {
        if (const char* c = std::getenv("COLUMNS")) {
            if (const int n = std::atoi(c); n > 0) cols = n;
        }
    }
    if (cols <= 0) cols = 80;
    return cols;
}

// How many columns the provider badge may occupy.
//
// The badge is the grouping signal in a flat cross-provider list, so it must
// stay legible — but it competes with the model NAME, which is what the user
// is actually reading. On a wide terminal there is room for the full label
// ("GitHub Copilot"); on an 80-column one, spending 14 columns restating the
// provider on every row starves the names.
//
// Scales with the terminal instead of a magic constant: a fixed clamp is
// either too tight when wide or too greedy when narrow, and the picker is
// commonly used in a split pane.
[[nodiscard]] int picker_badge_max_cols() {
    const int cols = picker_terminal_cols();
    if (cols < 70)  return 8;    // narrow split: abbreviate hard
    if (cols < 100) return 12;   // typical 80-col terminal
    return 16;                   // wide: full labels fit
}

// Provider-badge hue, keyed on the model's capability TIER.
//
// The browse list is ordered strongest-first (see build_fused_rows), but that
// ordering was INVISIBLE: the user saw a reordered list with nothing to say
// why, and no way to tell a flagship from a 3B local model without reading
// every id. Colour is the cheapest way to make an existing ordering legible.
//
// Hues come from the shared palette's semantic ramp rather than new constants,
// and the mapping is intensity-ordered so it reads as a gradient even to
// someone who can't separate the hues: bright accent → blue → cyan → grey.
//
// Deliberately applied to the BADGE, not the name: the badge is already
// reference-weight, so this adds a signal without making the list shout. And
// it is never the ONLY carrier — the ordering itself, plus the ✦ reasoning
// mark, say the same thing without colour (WCAG 1.4.1: colour is never the
// sole means of conveying information).
[[nodiscard]] maya::Color tier_hue(ModelCapabilities::Tier t) {
    using T = ModelCapabilities::Tier;
    switch (t) {
        case T::Flagship: return role_brand_alt;  // bright magenta — the top lane
        case T::Mid:      return role_info;       // blue — the workhorse lane
        case T::Cheap:    return code_path;       // bright cyan — fast/small
        case T::Weak:     return muted;           // grey — tool-use unreliable
    }
    return muted;
}

// One key-binding hint in a footer strip: a key glyph + a short label,
// plus a priority that decides survival order when the picker is too
// narrow to show them all (higher = kept longer).
// (moved to agentty/runtime/view/hints.hpp — shared with diff-review)

// The reasoning-effort control footer, shared by the classic model picker AND
// the fused picker so both surfaces render IDENTICALLY and read/write the SAME
// state (m.d.effort, the per-model reasoning_override, m.d.show_reasoning).
// `model_id` is the highlighted row's model; `scope` its provider (empty =
// active provider) so a fused row's ladder reflects THAT host's contract.
// Returns the rows to append to a picker's footer (may be empty). Kept in ONE
// place so the two pickers can never diverge (the "off in one, on in the
// other" bug).
inline std::vector<Element> reasoning_effort_footer(const Model& m,
                                                    std::string_view model_id,
                                                    std::string_view scope = {}) {
    std::vector<Element> out;
    if (model_id.empty()) return out;

    const std::string hi_id{model_id};
    const auto caps = resolved_caps(hi_id, scope);

    // ── Show-reasoning toggle piece (^R), a global on/off display switch.
    // Appended inline to the reasoning line so effort + show/hide live in
    // ONE place. ✦ + accented when on, dim when off.
    auto append_show_reasoning = [&](std::vector<Element>& parts) {
        const bool on = m.d.show_reasoning;
        parts.push_back(text("  ", fg_dim(muted)));
        parts.push_back(text("^R ", fg_of(fg)));
        // Honest label when the DIALECT can't carry reasoning text back for
        // THIS model. First-party OpenAI uses Chat Completions, which does
        // not transmit GPT-5 reasoning at all (it is generated and billed,
        // but only the Responses API returns it). Showing "✦ shown" there
        // promises output that can never arrive — the user reasonably
        // reads that as a bug in agentty.
        //
        // Copilot is MIXED and therefore model-dependent: gpt-5* and
        // mai-code-* stream over the Responses API (reasoning text DOES
        // come back, measured), while claude-* and gpt-4.x are chat-only.
        // So the answer is per ROW, not per provider — the same Copilot
        // picker legitimately shows "✦ shown" on one line and "n/a on this
        // API" on the next.
        //
        // `scope` is the row's provider in the fused picker; empty means
        // the per-provider picker, i.e. the ACTIVE provider — which is
        // exactly what caps_provider_scope() publishes on every switch
        // (endpoint label for OpenAI-family, agent id for ACP, else the
        // default id).
        const std::string prov =
            scope.empty() ? caps_provider_scope() : std::string{scope};
        if (!provider::wire_streams_reasoning_text(prov, hi_id)) {
            parts.push_back(text("n/a on this API", fg_dim(muted)));
            return;
        }
        parts.push_back(text(on ? "\xe2\x9c\xa6 shown" : "hidden",
                             on ? fg_bold(accent) : fg_dim(muted)));
    };

    if (effort_capable(caps)) {
        // One line: the effort ladder (current tier bracketed ‹like this›
        // and accented), then the global ^R show/hide toggle. ←/→ cycles the
        // tier. (The per-model override state is already conveyed by the row's
        // own reasoning badge, so no separate ^E affordance here.)
        std::vector<Element> parts;
        parts.push_back(text("reasoning ", fg_dim(muted)));
        for (Effort lvl : available_efforts(caps)) {
            const std::string lbl{effort_label(lvl)};
            if (lvl == m.d.effort) {
                parts.push_back(text("\xe2\x80\xb9", fg_of(accent)));       // ‹
                parts.push_back(text(lbl, fg_bold(accent)));
                parts.push_back(text("\xe2\x80\xba ", fg_of(accent)));    // ›
            } else {
                parts.push_back(text(lbl + " ", fg_dim(muted)));
            }
        }
        append_show_reasoning(parts);
        out.push_back(h(std::move(parts)).build());
    } else {
        // No effort control on this model — still show the global ^R toggle.
        std::vector<Element> parts;
        parts.push_back(text("reasoning ", fg_dim(muted)));
        parts.push_back(text("off", fg_dim(muted)));
        append_show_reasoning(parts);
        out.push_back(h(std::move(parts)).build());
    }
    return out;
}

} // namespace


// ── Fused cross-provider model picker ────────────────────────────────────
// One list over EVERY authed provider (docs/design/unified-model-picker.md).
// Rows are `provider · model` with a context-window trailing cell; sections
// (recent / all providers / sign in) render as is_header dividers. Selecting
// switches provider+model atomically; a dim "sign in to X" row routes to login.
Element fused_picker(const Model& m) {
    auto* picker = m.ui.overlay.get<ov::FusedPicker>();
    if (!picker) return text("");

    // Read the reducer-maintained cache — never rebuild per frame.
    const auto& rows = m.d.fused_rows;

    Picker::Config cfg;
    // Slot-assign mode: retitle so it's clear the pick fills a Smart Mode
    // role rather than switching the model you're chatting with, and say
    // which provider the list is scoped to (see fused_rows_for_model).
    const int slot = m.ui.smart_assign_slot;
    cfg.title = slot < 0
        ? std::string{" Models \xc2\xb7 all providers "}
        : std::string{" Smart Mode \xc2\xb7 pick "}
          + (slot == 0 ? "Strategic" : slot == 1 ? "Implementation" : "Utility")
          + " model ";
    cfg.accent   = accent;
    // The active-row edge bar shares the picker's accent, so "you are here"
    // reads as one visual language with the title and query caret instead of
    // introducing a third hue. The cursor bar (bright cyan) still wins on
    // overlap, which is correct — where you ARE outranks where you were.
    cfg.active_color = accent;
    cfg.viewport_h = picker_viewport_h();
    cfg.scroll     = &m.ui.fused_picker_scroll;

    cfg.header.push_back(
        picker->query.empty()
            ? h(text("\xf0\x9f\x94\x8d ", fg_of(muted)),
                query_caret(accent),
                text(std::string{slot < 0 ? "type to filter across providers"
                                          : "type to filter this provider"},
                     fg_italic(muted))
              ).build()
            : h(text("\xf0\x9f\x94\x8d ", fg_of(muted)),
                text(picker->query, fg_of(fg)),
                query_caret(accent)
              ).build());
    cfg.header.push_back(sep);   // rule under the filter (matches the other pickers)

    // Lazy-load hint: while any provider's catalog is still streaming in,
    // show a dim spinner-ish note so the (initially active-provider-only)
    // list visibly reads as "more coming", not "that's all". Failed providers
    // are surfaced separately so a network blip doesn't look like "that's all".
    {
        int failed = 0;
        std::vector<std::string_view> pending;
        for (const auto& c : m.d.provider_catalogs) {
            if (c.state == ProviderCatalog::State::Loading)
                pending.push_back(c.label);
            else if (c.state == ProviderCatalog::State::Failed) ++failed;
        }
        if (!pending.empty()) {
            // Name who we're waiting on — "⋯ loading Groq, Mistral…" answers
            // "where's X?" precisely, where a bare count made the user guess.
            std::string who;
            const std::size_t shown = std::min<std::size_t>(pending.size(), 3);
            for (std::size_t i = 0; i < shown; ++i) {
                if (i) who += ", ";
                who += pending[i];
            }
            if (pending.size() > shown)
                who += " +" + std::to_string(pending.size() - shown);
            cfg.header.push_back(text(
                "  " + std::string{m.s.spinner.current_frame()}
                    + " loading " + who + "\xe2\x80\xa6",
                fg_italic(muted)));
        }
        if (failed > 0)
            cfg.header.push_back(text(
                "  \xe2\x9a\xa0 " + std::to_string(failed)
                    + (failed == 1 ? " provider failed to refresh"
                                   : " providers failed to refresh")
                    + " \xc2\xb7 reopen to retry",
                fg_of(warn)));
    }

    if (rows.empty()) {
        // A dead end should say WHY it is empty and what to do next. The bare
        // "no models match" was the same message whether you had mistyped,
        // whether every catalog was still loading, or whether you were signed
        // out of everything — three very different situations with three
        // different next actions. It also returned before the footer was
        // built, so the picker offered no hint that Esc even worked.
        const bool loading = std::any_of(
            m.d.provider_catalogs.begin(), m.d.provider_catalogs.end(),
            [](const ProviderCatalog& c) {
                return c.state == ProviderCatalog::State::Loading;
            });
        const bool any_authed = std::any_of(
            m.d.provider_catalogs.begin(), m.d.provider_catalogs.end(),
            [](const ProviderCatalog& c) { return !c.models.empty(); });

        Picker::Config::Row nr;
        if (loading) {
            nr.leading = "  " + std::string{m.s.spinner.current_frame()}
                       + " loading model catalogs\xe2\x80\xa6";
        } else if (!any_authed) {
            nr.leading = "  no providers signed in \xc2\xb7 "
                         "^P to add one";
        } else if (!picker->query.empty()) {
            nr.leading = "  no model matches \xe2\x80\x9c" + picker->query
                       + "\xe2\x80\x9d \xc2\xb7 Backspace to widen";
        } else {
            nr.leading = "  no models available";
        }
        nr.leading_style = fg_italic(muted);
        cfg.rows.push_back(std::move(nr));
        cfg.selected = 0;
        // Keep the footer: an empty list is exactly when the user most needs
        // to be told how to leave or how to reach the provider picker.
        cfg.footer.push_back(key_hints({
            {"^P", "providers", 1},
            {"Esc", "close", 4},
        }));
        return Picker{std::move(cfg)}.build();
    }

    // Section dividers: RECENT vs ALL PROVIDERS, plus a query-gated SIGN IN
    // section — an un-authed provider whose name matches the query renders as
    // one dim actionable row so searching for it is never a dead end.
    enum class Section { Recent, All, SignIn };
    auto section_of = [](const FusedRow& r) {
        if (r.is_signin_offer()) return Section::SignIn;
        return r.recent ? Section::Recent : Section::All;
    };
    std::optional<Section> cur;
    int visual_selected = 0;
    // Size of the "all providers" section, for the header count below.
    int all_count = 0;
    for (const auto& r : rows)
        if (section_of(r) == Section::All) ++all_count;

    // ── Badge column width ──────────────────────────────────────────
    // maya's Picker asks callers to "pad badges to a common width" for column
    // alignment, and this picker never did — so with provider labels running
    // 3..14 chars ("Groq" .. "GitHub Copilot") every model NAME started at a
    // different column and the list could not be scanned vertically. The
    // provider badge is the grouping signal in a flat cross-provider list, so
    // a ragged column defeats the whole layout.
    //
    // Measured in DISPLAY COLUMNS via maya::string_width, not bytes: registry
    // labels are ASCII today, but a custom host is user-named and may hold
    // CJK or emoji, where a byte count would over-pad and re-break the very
    // alignment this exists to create. (The same helper the provider picker
    // and the hints strip already use.)
    int badge_w = 0;
    for (const auto& r : rows)
        if (!r.is_signin_offer())
            badge_w = std::max(badge_w, maya::string_width(r.label));
    badge_w = std::min(badge_w, picker_badge_max_cols());
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const auto& r = rows[static_cast<std::size_t>(i)];
        const Section sec = section_of(r);
        if (!cur || *cur != sec) {
            cur = sec;
            Picker::Config::Row hdr;
            hdr.is_header = true;
            hdr.leading = sec == Section::Recent ? "recent"
                        : sec == Section::All    ? "all providers"
                                                  : "not signed in";
            // Name the size of the browse list. An aggregator (OpenRouter et
            // al.) contributes hundreds of models to a 14-row viewport, and
            // without a count a tiny scrollbar is the only hint that the list
            // runs far past the screen — which reads as "my model is missing"
            // rather than "type to narrow". Browse-only: with a query active
            // the row count is the answer to the query, and a total would
            // just be noise next to it.
            if (sec == Section::All && picker->query.empty() && all_count > 0) {
                hdr.trailing = std::to_string(all_count) + " models \xc2\xb7 "
                               "type to filter";
                hdr.trailing_style = fg_italic(muted);
            }
            cfg.rows.push_back(std::move(hdr));
        }
        if (i == picker->index)
            visual_selected = static_cast<int>(cfg.rows.size());
        const bool selected = (i == picker->index);

        // Sign-in offer row: "<Provider>  — Enter to sign in". Dim, single
        // action, no trailing chips (there's no model yet).
        if (r.is_signin_offer()) {
            Picker::Config::Row row;
            row.selected      = selected;
            row.leading       = "  " + r.label;
            row.leading_style = selected ? fg_bold(fg) : fg_of(muted);
            row.trailing       = "Enter to sign in \xe2\x86\x92";   // →
            row.trailing_style = fg_dim(muted);
            cfg.rows.push_back(std::move(row));
            continue;
        }

        Picker::Config::Row row;
        row.selected = selected;   // drives the highlight bar + selected bg
        const bool active = r.active;
        const int bw = maya::string_width(r.label);
        row.badge         = bw < badge_w
                              ? r.label + std::string(
                                    static_cast<std::size_t>(badge_w - bw), ' ')
                              : r.label;
        // Active row keeps the brand accent (it is the "you are here" marker);
        // every other row is hued by capability tier, so the strongest-first
        // browse ordering is legible at a glance instead of unexplained.
        row.badge_style   = fg_dim(
            active ? accent
                   : tier_hue(static_cast<ModelCapabilities::Tier>(r.tier)));
        row.leading       = "  " + r.model_label;
        // The ACTIVE row is marked by maya's native edge bar (a coloured `▎`
        // in column 0, `active_color`) rather than a `● ` text prefix. Two
        // reasons: the bar costs no name width — it lives in chrome the
        // widget already reserves for the cursor — and it keeps every model
        // name starting at the SAME column, so the list scans vertically.
        // A text prefix indented exactly one row out of alignment.
        row.active        = active;
        // The model NAME is the primary content — the thing you are choosing
        // between — so it renders at full foreground. It used to be `muted` for
        // every non-active row, which dimmed the entire list to make ONE row
        // stand out; that inverted the hierarchy (reference chips out-shouted
        // the names) and made a long list read as uniformly unavailable. The
        // active row keeps bold plus the edge bar, which is enough to find it.
        row.leading_style = active ? fg_bold(fg) : fg_of(fg);
        // fzf-style match highlight: paint the query's matched chars in the
        // name so a big filtered list shows WHY each row is here. Use the same
        // `highlight` theme hue as the classic model picker (not `info`) so
        // the two pickers read identically. match_positions are offsets into
        // the NAME; shift them past the uniform two-space indent. (This used
        // to branch on `active`, which carried a wider "● " prefix — the
        // edge bar replaced it, so every row now has the same offset.)
        if (!r.match_positions.empty()) {
            constexpr int kPrefix = 2;
            row.highlight.reserve(r.match_positions.size());
            for (int p : r.match_positions) row.highlight.push_back(kPrefix + p);
            row.highlight_fg = highlight;   // same hue as the other pickers
        }
        // Trailing cell: context window, then the two marks. RIGHT-ALIGNED as
        // a fixed-width column — the number is a MEASUREMENT, and measurements
        // that don't share a decimal column can't be compared at a glance
        // ("1M" next to "200k" next to "8k" read as noise when ragged).
        //
        // The marks sit in their own two fixed slots after it, so ★ and ✦
        // always land in the same place: a row without a favourite leaves a
        // hole rather than sliding its ✦ left into the ★ column, which is what
        // made the old list look jittery as you scrolled.
        std::string ctx;
        if (const int win = r.model.context_window; win > 0) {
            if (win >= 1'000'000) {
                ctx = std::to_string(win / 1'000'000) + "M";
                if (win % 1'000'000 != 0) ctx += "+";        // 1.x M → "1M+"
            } else if (win >= 1000) {
                ctx = std::to_string(win / 1'000) + "k";
            } else {
                ctx = std::to_string(win);
            }
        }
        // Widest realistic context label is 5 columns ("200k", "1M+").
        std::string trailing = ctx.size() < 5
            ? std::string(5 - ctx.size(), ' ') + ctx
            : ctx;
        trailing += r.model.favorite ? "  \xe2\x98\x85" : "   ";      // ★
        // Reasoning badge (precomputed in build_fused_rows) — marks models
        // that can think, so "which of these reason" is legible across
        // providers.
        trailing += r.reasons ? " \xe2\x9c\xa6" : "  ";               // ✦
        row.trailing       = std::move(trailing);
        // Dim by default: the trailing cell is REFERENCE data, not the thing
        // you are choosing between. It used to share the composer's warm
        // accent with the model name's own emphasis, so every row shouted.
        // The active row keeps the accent so the current model still reads at
        // a glance.
        row.trailing_style = active ? fg_of(accent) : fg_dim(muted);
        // NO-TOOLS: a WORD appended to the NAME, not another glyph in the
        // trailing cell. Three reasons: (1) it is a disqualifier, not
        // reference data, and the trailing cell is dim-grey precisely
        // because it IS reference — a warning painted as reference reads as
        // decoration; (2) an unexplained glyph is a puzzle and this picker
        // has no room for a legend; (3) the trailing cell is the FIRST
        // thing dropped under width pressure (yields_trailing below), so
        // the one mark that decides whether the model works at all would
        // vanish on a narrow terminal. "chat only" is self-explanatory,
        // needs no key, and travels with the name it disqualifies.
        if (!r.tool_capable) row.leading += " \xc2\xb7 chat only";
        // Under width pressure the CHIPS give way, not the model name — the
        // name is what you are selecting; the context window and marks are
        // reference data. Without this the default policy (leading yields
        // first) truncated "Claude Sonnet 4.6" to keep "200k ★ ✦" intact on a
        // narrow split, which is exactly backwards.
        row.trailing_secondary = true;
        cfg.rows.push_back(std::move(row));
    }
    cfg.selected = visual_selected;

    // Reasoning-effort control for the highlighted model — the shared
    // reasoning_effort_footer. ←/→ mutates the global m.d.effort live (no
    // staged tier), so the chip, the footer and the wire can't disagree.
    if (picker->index >= 0 && picker->index < static_cast<int>(rows.size())) {
        const auto& hl = rows[static_cast<std::size_t>(picker->index)];
        if (!hl.is_signin_offer())
            for (auto& row : reasoning_effort_footer(m, hl.model.id.value,
                                                     hl.provider_id))
                cfg.footer.push_back(std::move(row));
    }

    // Footer hints follow the mode: in slot-assign Enter PINS a role and Esc
    // goes BACK to Smart Mode, so promising "switch"/"close" would misstate
    // what the keys do. ^/ and ^Tab are switch-only affordances.
    // Written as two branches rather than one `slot >= 0 ? key_hints({...})
    // : key_hints({...})` ternary, which made GCC's -Wdangling-pointer fire.
    // That warning is a FALSE POSITIVE, not a lifetime bug worth preserving:
    // key_hints takes its vector BY VALUE and moves it into the component's
    // capture, so the braced temp is consumed before the full expression
    // ends and nothing outlives it. GCC just loses track of the temp's
    // ownership across the two ternary arms. The branches are equivalent
    // code, read better, and match the surrounding footer style — so this is
    // a quieting rewrite, not a fix. Restoring the ternary would be correct
    // C++ and would only bring the noise back.
    if (slot >= 0)
        cfg.footer.push_back(key_hints({
            {"\xe2\x86\x91\xe2\x86\x93", "move", 5},
            {"1-9", "jump", 3},
            {"Enter", "pin to role", 5},
            {"^F", "favorite", 1},
            {"^L", "refresh", 2},
            {"Esc", "back", 4},
          }));
    else
        cfg.footer.push_back(key_hints({
            {"\xe2\x86\x91\xe2\x86\x93", "move", 5},
            {"1-9", "jump", 3},
            {"Enter", "switch", 5},
            {"^F", "favorite", 1},
            {"^R", "reasoning", 2},
            {"^L", "refresh", 2},
            {"^Tab", "prev", 2},
            {"Esc", "close", 4},
          }));
    return Picker{std::move(cfg)}.build();
}

// ── Provider picker helpers ──
namespace {

// Resolve the currently-active provider id so the picker can mark the
// active row. Anthropic when kind==Anthropic, else the endpoint label.
[[nodiscard]] std::string active_provider_id() {
    const auto& sel = provider::active();
    if (sel.kind == provider::Kind::OpenAI) return sel.openai_endpoint.label;
    if (sel.kind == provider::Kind::ExternalAcp) return sel.acp_agent_id;
    return std::string{provider::default_provider_id()};
}

// Per-row auth status for the trailing column. Local backends need none;
// Anthropic reflects the LIVE deps().auth (✓ signed in / ⚠ sign in);
// OpenAI-family checks its env-var chain; ChatGPT is native OAuth.

} // namespace

Element provider_picker(const Model& m) {
    auto* picker = m.ui.overlay.get<ov::ProviderPicker>();
    if (!picker) return nothing();

    Picker::Config cfg;
    cfg.title      = " Providers ";
    cfg.accent     = highlight;
    cfg.min_width  = 52;
    cfg.viewport_h = picker_viewport_h();
    cfg.scroll     = &m.ui.provider_picker_scroll;
    cfg.selected   = picker->index;

    const std::string active_id = active_provider_id();

    auto env_has = [](std::string_view name) -> bool {
        if (name.empty()) return false;
        const char* v = std::getenv(std::string{name}.c_str());
        return v && *v;
    };

    // The one ordered, query-filtered row list — the SAME list the reducer
    // resolves a selection against (see build_provider_rows). The cursor is a
    // plain index into it; there is no offset math on either side.
    auto settings = app::deps().load_settings();
    const std::vector<std::string> saved_custom_hosts =
        provider::saved_custom_hosts(settings.provider_keys);
    const auto rows = ui::build_provider_rows(saved_custom_hosts, picker->query);

    // Live search header (mirrors the model picker). Backspace trims; typing
    // narrows. Hidden ACP/custom rows return the moment the query is cleared.
    cfg.header.push_back(h(text("\xf0\x9f\x94\x8d ", fg_of(muted)),
        text(picker->query.empty() ? "type to filter providers\xe2\x80\xa6"
                                   : picker->query,
             picker->query.empty() ? fg_italic(muted) : fg_of(fg))
    ).build());
    cfg.header.push_back(sep);

    // Trailing auth-status column for a built-in preset. One place, so every
    // provider's badge stays consistent.
    auto preset_note = [&](const provider::ProviderPreset& p, bool active)
        -> std::pair<std::string, maya::Color> {
        auto signed_badge = [&](std::string signed_label) {
            return std::pair<std::string, maya::Color>{
                active ? std::string{"\xe2\x9c\x93 signed in \xc2\xb7 accounts"}
                       : std::move(signed_label),
                active ? success : muted};
        };
        // OAuth providers whose token lives in their own transport: one
        // uniform status line, answered by the auth vault. Was three
        // near-identical name-keyed blocks calling three different
        // signed_in() probes; vault::signed_in dispatches over the same
        // table, so a new OAuth provider gets its status row for free.
        if (p.token_in_transport) {
            if (auth::vault::signed_in(std::string{p.id}))
                return signed_badge(std::string{p.label} + " (signed in)");
            return {"\xe2\x9a\xa0 sign in with " + std::string{p.label}, warn};
        }
        if (p.is_local || p.auth == provider::AuthStyle::None)
            return {"\xe2\x97\x8f local", info};
        if (p.kind() == provider::Kind::Anthropic) {
            // On-disk credential store is authoritative and independent of the
            // currently-active provider (do NOT read deps().auth here).
            if (auth::anthropic_signed_in())
                return {active ? "\xe2\x9c\x93 signed in \xc2\xb7 accounts" : "\xe2\x9c\x93 signed in", success};
            return {"\xe2\x9a\xa0 sign in", warn};
        }
        // Hosted API-key provider: distinguish a SAVED (pasted) key — which
        // ^D signs out — from an ENV key, which can't be removed in-app, so
        // the badge must not imply ^D will work on it.
        switch (provider::auth_source(p, settings)) {
            case provider::AuthSource::Saved:
                return {active ? "\xe2\x9c\x93 signed in \xc2\xb7 key"
                               : "\xe2\x9c\x93 key saved", success};
            case provider::AuthSource::Env: {
                std::string ev;
                for (auto e : p.auth_env) if (env_has(e)) { ev = e; break; }
                return {"\xe2\x97\x8f key from " + ev, info};
            }
            case provider::AuthSource::Local:
                return {"\xe2\x97\x8f local", info};
            case provider::AuthSource::None:
            default: break;
        }
        std::string_view want = p.auth_env.front();
        return {want.empty() ? "\xe2\x9a\xa0 no key" : "\xe2\x9a\xa0 " + std::string{want}, warn};
    };

    cfg.rows.reserve(rows.size());
    int i = 0;
    for (const auto& r : rows) {
        const bool sel = (i == picker->index);
        Picker::Config::Row row;

        if (const auto* p = r.preset()) {
            const bool active = (p->id == active_id);
            const bool confirming = (picker->confirm_remove == std::string{p->id});
            auto [note, note_color] = preset_note(*p, active);
            row.leading        = std::string{p->label} + "  " + std::string{p->blurb};
            // Primary content renders at full foreground (same rule as the
            // model picker): dimming EVERY non-active row to make one stand
            // out inverts the hierarchy — it makes the whole list read as
            // unavailable while the trailing status chip out-shouts the name.
            // Bold + the ● marker is enough to find the active row.
            row.leading_style  = active ? fg_bold(fg) : fg_of(fg);
            if (confirming) {
                // Del/d armed on a preset that has a saved key — second press
                // signs out (clears the key), the preset itself stays.
                row.trailing       = "\xe2\x9c\x97 press again to sign out";
                row.trailing_style = fg_of(warn);
            } else {
                row.trailing       = note;
                row.trailing_style = fg_of(note_color);
            }
            row.active         = active;
        } else if (const auto* agent = r.acp()) {
            const bool active = (agent->id == active_id);
            row.leading        = agent->id + "  external ACP agent (" + agent->command + ")";
            row.leading_style  = active ? fg_bold(fg) : fg_of(fg);
            row.trailing       = "\xe2\x97\x8f agent";
            row.trailing_style = fg_of(info);
            row.active         = active;
        } else if (const auto* spec = r.custom_host()) {
            const bool active = (*spec == active_id);
            const bool confirming = (picker->confirm_remove == *spec);
            row.leading        = *spec + "  custom OpenAI-compatible host";
            row.leading_style  = active ? fg_bold(fg) : fg_of(fg);
            if (confirming) {
                row.trailing       = "\xe2\x9c\x97 press again to remove";
                row.trailing_style = fg_of(warn);
            } else {
                row.trailing       = "\xe2\x9c\x93 ready";
                row.trailing_style = fg_of(success);
            }
            row.active         = active;
        } else {   // NewCustomHost sentinel
            row.leading        = std::string{"Custom host\xe2\x80\xa6  "}
                               + "any OpenAI-compatible server (host:port)";
            row.leading_style  = fg_of(muted);
            row.trailing       = "\xe2\x9c\x8e edit";
            row.trailing_style = fg_of(info);
        }
        row.selected = sel;
        // Under width pressure the STATUS chip gives way, not the provider
        // name — the name is what you select; "✓ signed in · accounts" is
        // reference data. Same policy as the command palette and the model
        // picker; maya's default (leading yields first) is for rows where the
        // trailing cell matters more, like the file picker's diffstat.
        row.trailing_secondary = true;
        cfg.rows.push_back(std::move(row));
        ++i;
    }

    // Enter opens the accounts drill-down on an account-capable OAuth
    // provider, otherwise it switches. Read straight off the highlighted row.
    const bool row_has_accounts = [&] {
        if (picker->index < 0 || picker->index >= static_cast<int>(rows.size()))
            return false;
        const auto& row = rows[static_cast<std::size_t>(picker->index)];
        // Custom hosts hold multiple keys (accounts) — Enter drills in.
        if (const auto* spec = row.custom_host())
            return provider::credentials::add_method(*spec)
                   != provider::credentials::AddMethod::None;
        const auto* p = row.preset();
        if (!p) return false;
        // Every account-capable provider (OAuth + hosted API key) shows the
        // account drill-down on Enter; only keyless local servers don't.
        return provider::credentials::add_method(p->id)
               != provider::credentials::AddMethod::None;
    }();

    cfg.footer.push_back(text(""));
    cfg.footer.push_back(h(
        text("\xe2\x9c\x93", fg_of(success)), text(" ready  ", fg_dim(muted)),
        text("\xe2\x9a\xa0", fg_of(warn)),    text(" set the named key first  ", fg_dim(muted))
    ).build());
    cfg.footer.push_back(key_hints({
        {"\xe2\x86\x91\xe2\x86\x93", "move", 5},        // ↑↓
        {"type", "filter", 4},
        {"Enter", row_has_accounts ? "accounts" : "switch", 5},
        {"^D", picker->confirm_remove.empty() ? "remove" : "confirm", 2},
        {"^/", "models", 3},                       // cross-hint: model picker
        {"Esc", "close", 4},
    }));

    return Picker{std::move(cfg)}.build();
}

Element thread_list(const Model& m) {
    auto* picker = m.ui.overlay.get<ov::ThreadList>();
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
            const bool confirming = (picker->confirm_remove == t.id.value);
            Picker::Config::Row row;
            // "● " marks the thread you're IN — the anchor for both the
            // picker and the ^←→ / Alt+←→ quick-cycle. Non-current rows
            // get a two-space gutter so titles stay column-aligned.
            row.leading        = (is_current ? "\xe2\x97\x8f " : "  ")
                               + (t.title.empty() ? "(untitled)" : t.title);
            // Thread TITLES are what you are choosing between — full
            // foreground. Same hierarchy rule as the model/provider pickers.
            row.leading_style  = is_current ? fg_bold(info) : fg_of(fg);
            if (confirming) {
                row.badge       = "\xe2\x9a\xa0";           // ⚠
                row.badge_style = fg_of(warn);
                row.leading_style = fg_bold(warn);
                row.trailing       = "press d again to confirm";
                row.trailing_style = fg_of(warn);
            } else {
                row.trailing       = timestamp_full(t.updated_at);
                row.trailing_style = fg_dim(muted);
            }
            row.selected = (i == picker->index);
            // The TITLE is what you are choosing; the timestamp is reference
            // data and yields first on a narrow terminal.
            row.trailing_secondary = true;
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
        {"D", picker->confirm_remove.empty() ? "remove" : "confirm", 3},
        {"^/Alt+\xe2\x86\x90\xe2\x86\x92", "cycle", 1},   // ^←→ / Alt+←→
        {"Esc", "close", 4},
    }));

    return Picker{std::move(cfg)}.build();
}

// Smart Mode config overlay: a master Enabled toggle + the three role slots,
// each showing its RESOLVED model (pinned, or the auto-fill). See
// docs/design/smart-mode.md.
Element smart_mode_overlay(const Model& m) {
    auto* o = m.ui.overlay.get<ov::SmartMode>();
    if (!o) return nothing();

    const auto& sm = m.d.smart;
    const std::string parent = m.d.model_id.value;

    // Resolve each role for DISPLAY (what would actually run right now).
    // Provider-scoped like the wire path, so a pin made under another provider
    // shows the auto-fill that will really serve the turn rather than a model
    // this endpoint cannot stream.
    auto shown = [&](smart::ModelRole role) -> std::string {
        auto rp = smart::resolve_role(role, parent, m.d.effort,
                                      m.d.available_models, sm,
                                      active_provider_id());
        std::string label = pretty_model_label(rp.model);
        return label.empty() ? rp.model : label;
    };
    auto slot_suffix = [&](const smart::SlotOverride& ov) -> std::string {
        return (sm.enabled && ov.set) ? "  \xc2\xb7 pinned" : "  \xc2\xb7 auto";
    };

    Picker::Config cfg;
    cfg.title      = " Smart Mode ";
    cfg.accent     = success;
    cfg.min_width  = 60;
    cfg.viewport_h = picker_viewport_h();
    cfg.selected   = o->index;

    const bool on = sm.enabled;
    struct Row { std::string lead, trail; };
    // FOUR rows: the master switch and the three role slots. There used to be
    // eleven — seven of them sub-layer toggles. Three of those folded into the
    // master switch (a toggle earns its place only where a reasonable user
    // would reasonably choose either way; "make my compaction more expensive"
    // isn't such a choice) and four were deleted with the self-supervised
    // layers. Developer escape hatches live in AGENTTY_SMART_NO_* env vars.
    std::vector<Row> rows = {
        {std::string{on ? "\xe2\x97\x8f Smart Mode" : "\xe2\x97\x8b Smart Mode"},
         smart::tuning::enabled_override()
             ? std::string{on ? "on (env pin)" : "off (env pin)"}
             : std::string{on ? "on" : "off"}},
        {"  Strategic",      on ? shown(smart::ModelRole::Strategic)      + slot_suffix(sm.strategic)      : std::string{"\xe2\x80\x94"}},
        {"  Implementation", on ? shown(smart::ModelRole::Implementation) + slot_suffix(sm.implementation) : std::string{"\xe2\x80\x94"}},
        {"  Utility",        on ? shown(smart::ModelRole::Utility)        + slot_suffix(sm.utility)        : std::string{"\xe2\x80\x94"}},
    };
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        Picker::Config::Row r;
        r.leading        = rows[static_cast<std::size_t>(i)].lead;
        r.leading_style  = (i == 0)
            ? (on ? Style{}.with_fg(success).with_bold() : fg_dim(muted))
            : (on ? fg_of(fg) : fg_dim(muted));
        r.trailing       = rows[static_cast<std::size_t>(i)].trail;
        r.trailing_style = fg_dim(muted);
        r.selected       = (i == o->index);
        cfg.rows.push_back(std::move(r));
    }

    cfg.footer.push_back(text(""));
    {
        // Live SESSION state — the two adaptive inputs that move the next
        // turn's route (cascade bias + tier momentum). Without this line the
        // "learning" is a black box: a route that shifted from a session
        // regret is indistinguishable from classifier noise. Only shown when
        // either is non-neutral (neutral state = no noise).
        if (sm.enabled
            && (m.s.smart_effort_bias != 0
                || m.s.smart_turn_complexity != smart::Complexity::Standard)) {
            std::string live = "  This session: ";
            if (m.s.smart_effort_bias != 0) {
                live += "effort bias ";
                live += (m.s.smart_effort_bias > 0 ? "+" : "");
                live += std::to_string(m.s.smart_effort_bias);
            }
            if (m.s.smart_turn_complexity != smart::Complexity::Standard) {
                if (m.s.smart_effort_bias != 0) live += " \xc2\xb7 ";
                live += "momentum ";
                live += smart::to_string(m.s.smart_turn_complexity);
            }
            cfg.footer.push_back(text(std::move(live), fg_dim(muted)));
        }
    }
    {
        std::vector<Hint> hints = {
            {"\xe2\x86\x91\xe2\x86\x93", "move", 5},        // ↑↓
            {"Enter", o->index == 0 ? "toggle" : "set model", 4},
        };
        // `x` only acts on the three model-slot rows (1-3) — advertising it
        // on the master row promises a key that silently does nothing.
        if (o->index >= 1) hints.push_back({"x", "auto", 3});
        hints.push_back({"Esc", "close", 4});
        cfg.footer.push_back(key_hints(std::move(hints)));
    }
    return Picker{std::move(cfg)}.build();
}

Element command_palette(const Model& m) {
    auto* o = m.ui.overlay.get<ov::CommandPalette>();
    if (!o) return nothing();

    // Live visibility context — the SAME predicate the reducer uses, so a row
    // the dispatcher would reject never renders (no dead Accept-all).
    PaletteContext pctx;
    pctx.update_available    = !m.s.update_latest.empty();
    pctx.has_pending_changes = !m.d.pending_changes.empty();
    pctx.has_code_block      = [&] {
        for (auto it = m.d.current.messages.rbegin();
             it != m.d.current.messages.rend(); ++it)
            if (it->role == Role::Assistant && !it->text.empty()
                && !code_block_picker::extract_code_blocks(it->text).empty())
                return true;
        return false;
    }();
    auto scored = match_commands(o->query, pctx);
    std::vector<const CommandDef*> matches;
    matches.reserve(scored.size());
    for (const auto& s : scored) matches.push_back(s.cmd);

    Picker::Config cfg;
    cfg.title      = " Command Palette ";
    cfg.accent     = highlight;
    cfg.min_width  = 54;
    cfg.viewport_h = picker_viewport_h();
    cfg.scroll     = &m.ui.command_palette_scroll;
    cfg.selected   = matches.empty() ? -1 : o->index;

    cfg.header.push_back(
        o->query.empty()
            ? h(text("\xe2\x8c\x98 ", fg_bold(highlight)),   // ⌘
                query_caret(highlight),
                text("type to filter\xe2\x80\xa6", fg_italic(muted))
              ).build()
            : h(text("\xe2\x8c\x98 ", fg_bold(highlight)),
                text(o->query, fg_of(fg)),
                query_caret(highlight)
              ).build());
    cfg.header.push_back(sep);

    // Each category owns a hue so the flat list reads as coloured bands; the
    // badge keeps its hue on the selected row (Picker contract), so the
    // grouping survives the cursor. General rows carry no badge (Quit/Update
    // don't need a section chip).
    auto category_hue = [](Category c) -> Color {
        switch (c) {
            case Category::Thread:   return info;
            case Category::Changes:  return success;
            case Category::Navigate: return highlight;
            case Category::Config:   return warn;
            case Category::Account:  return muted;
            case Category::General:  return muted;
        }
        return muted;
    };

    if (matches.empty()) {
        cfg.items.push_back(text(
            o->query.empty() ? "  no commands available"
                             : "  no command matches \"" + o->query + "\"",
            fg_italic(muted)));
    } else {
        cfg.rows.reserve(matches.size() + 6);
        // On the EMPTY query we render real SECTION HEADERS between category
        // groups — true nesting, VS Code / Raycast style. The moment the user
        // types, headers vanish and the list goes flat-with-ranking (empty
        // categories would be noise, and label-hit ranking reorders anyway).
        // Headers are non-selectable rows; the cursor (o->index) indexes the
        // header-FREE `matches`, so we only set Row::selected on real rows and
        // point cfg.selected at the header-adjusted display position — the
        // reducer/dispatch stay entirely header-unaware.
        const bool show_headers = o->query.empty();
        Category   last_cat     = Category::General;
        bool       first_group  = true;
        int        display_row  = 0;   // position INCLUDING headers, for scroll
        int        sel_display  = -1;

        for (int i = 0; i < static_cast<int>(matches.size()); ++i) {
            const auto& cmd = *matches[static_cast<std::size_t>(i)];

            const bool group_start =
                show_headers && (first_group || cmd.category != last_cat);
            if (group_start) {
                if (auto lab = category_label(cmd.category); !lab.empty()) {
                    Picker::Config::Row hdr;
                    hdr.is_header = true;
                    // A connector header: "┌─ THREAD" so the eye reads it as the
                    // TOP of a bracket whose │ spine (below) runs down the
                    // group's rows — real visual containment, tree-style.
                    std::string up{lab};
                    for (char& ch : up) ch = static_cast<char>(std::toupper(
                        static_cast<unsigned char>(ch)));
                    hdr.leading       = "\xe2\x94\x8c\xe2\x94\x80 " + up;   // ┌─ 
                    hdr.leading_style = fg_of(category_hue(cmd.category));
                    cfg.rows.push_back(std::move(hdr));
                    ++display_row;
                }
                last_cat    = cmd.category;
                first_group = false;
            }

            Picker::Config::Row row;

            // ── Tree spine ── on the sectioned (empty-query) view each command
            // carries a │ gutter in its section's hue, so the group reads as a
            // bracket hanging off its ┌─ header. The badge cell (between the
            // cursor edge and the label) is exactly the right slot for it, and
            // it stays coloured on the selected row (Picker keeps badge hue).
            // The last row of a group closes the bracket with └.
            if (show_headers) {
                const bool group_end =
                    (i + 1 >= static_cast<int>(matches.size()))
                    || matches[static_cast<std::size_t>(i + 1)]->category != cmd.category;
                row.badge       = group_end ? "\xe2\x94\x94 "    // └ 
                                            : "\xe2\x94\x82 ";   // │ 
                row.badge_style = fg_dim(category_hue(cmd.category));
            }

            // ── Label, with live toggle/mode state folded in ──
            std::string label{cmd.label};
            if (cmd.id == Command::SmartMode)
                label += m.d.smart.enabled ? "  (on)" : "  (off)";
            else if (cmd.id == Command::ToggleChangesStrip)
                label += m.d.show_changes_strip ? "  (shown)" : "  (hidden)";
            row.leading = std::move(label);
            row.leading_style = cmd.danger ? fg_of(danger) : fg_of(fg);
            // Highlight the fuzzy-matched characters (Raycast-style) so the
            // ranking is legible: with "re" typed, the "Re" in Review/Reject
            // lights up. Positions came from the scored matcher.
            if (!o->query.empty()) {
                row.highlight    = scored[static_cast<std::size_t>(i)].positions;
                row.highlight_fg = cmd.danger ? danger : highlight;
            }

            // ── Trailing: description · shortcut. The LABEL is what you
            // select, so mark the trailing SECONDARY — the widget shrinks it
            // first and keeps a gap, so on a narrow (phone/SSH) terminal the
            // description gracefully truncates (then vanishes) while the label
            // and shortcut stay whole, instead of the label being eaten.
            std::string trailing{cmd.description};
            if (cmd.shortcut && *cmd.shortcut) {
                trailing += "  \xc2\xb7  ";
                trailing += cmd.shortcut;
            }
            row.trailing           = std::move(trailing);
            row.trailing_style     = fg_dim(muted);
            row.trailing_secondary = true;
            row.selected = (i == o->index);
            if (i == o->index) sel_display = display_row;
            cfg.rows.push_back(std::move(row));
            ++display_row;
        }
        // Scroll tracks the header-adjusted cursor position.
        cfg.selected = sel_display;
    }

    cfg.footer.push_back(text(""));
    // Count anchor when the list is scrolled — same grammar as the @ / # pickers.
    if (static_cast<int>(matches.size()) > kViewportH) {
        cfg.footer.push_back(text(
            "  " + std::to_string(o->index + 1) + "/"
                + std::to_string(matches.size()) + " commands",
            fg_dim(muted)));
    }
    cfg.footer.push_back(key_hints({
        {"\xe2\x86\x91\xe2\x86\x93", "move", 5},   // ↑↓
        {"type", "filter", 3},
        {"Enter", "run", 6},
        {"Esc", "close", 3},
    }));

    return Picker{std::move(cfg)}.build();
}

Element mention_palette(const Model& m) {
    auto* o = m.ui.overlay.get<ov::Mention>();
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
        text(o->query.empty() ? " your changed files first · type to filter…"
                              : (" " + o->query),
             o->query.empty() ? fg_italic(muted) : fg_of(fg))
    ).build());
    cfg.header.push_back(sep);

    if (o->files.empty()) {
        // Distinguish "still indexing" from "genuinely empty" — the walk
        // runs on a background thread; if it hasn't landed the picker
        // opened with an empty snapshot. files_ready() tells them apart.
        cfg.items.push_back(text(
            files_ready() ? "  workspace empty (or no readable files)"
                          : "  indexing workspace… (type to filter as it fills)",
            fg_italic(muted)));
    } else if (matches.empty()) {
        cfg.items.push_back(text("  no matches", fg_italic(muted)));
    } else {
        cfg.rows.reserve(matches.size());
        for (int i = 0; i < static_cast<int>(matches.size()); ++i) {
            const auto& path = o->files[matches[static_cast<std::size_t>(i)]];
            auto [name, dir] = split_name_dir(path);
            Picker::Config::Row row;
            // Git-status badge — the working-set signal, colour-coded so the
            // file you're editing is unmistakable at a glance. Padded to a
            // fixed width so leading text aligns across rows.
            if (auto tag = file_git_tag(path); tag != GitTag::None) {
                auto label = git_tag_label(tag);
                row.badge = "● " + std::string{label};
                row.badge_style =
                    tag == GitTag::Modified          ? fg_of(maya::Color::yellow())
                  : tag == GitTag::Staged            ? fg_of(maya::Color::green())
                  : tag == GitTag::Untracked         ? fg_of(info)
                  : /* RecentlyCommitted */            fg_dim(muted);
            }
            row.leading        = std::string{name};
            row.leading_style  = fg_of(fg);
            // Light up the matched characters of the filename so the fuzzy
            // rank is legible (re-score the name in-view against the query;
            // the workspace scorer ranks but doesn't return positions).
            if (!o->query.empty()) {
                auto fm = fuzzy::score(name, o->query);
                if (fm.matched()) { row.highlight = std::move(fm.positions);
                                    row.highlight_fg = highlight; }
            }
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
    auto* o = m.ui.overlay.get<ov::Symbol>();
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
        cfg.items.push_back(text(
            symbols_ready() ? "  no symbols indexed"
                            : "  indexing symbols… (type to filter as it fills)",
            fg_italic(muted)));
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
            // Highlight the matched chars of the symbol NAME (which is the
            // leading segment, so its offsets map directly onto row.leading).
            if (!o->query.empty()) {
                auto fm = fuzzy::score(sym.name, o->query);
                if (fm.matched()) { row.highlight = std::move(fm.positions);
                                    row.highlight_fg = highlight; }
            }
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
    auto* o = m.ui.overlay.get<ov::CodeBlocks>();
    if (!o) return nothing();

    Picker::Config cfg;
    cfg.title      = " Run Code Block ";
    cfg.accent     = success;
    cfg.min_width  = 60;
    cfg.viewport_h = picker_viewport_h();
    cfg.scroll     = &m.ui.code_blocks_scroll;
    cfg.selected   = o->blocks.empty() ? -1 : o->index;

    cfg.rows.reserve(o->blocks.size());
    // First pass width: the badge column is padded to a common width so the
    // preview text of every row starts at the same column (a ▶ run badge and
    // a `python` tag badge would otherwise misalign). Mirrors the tool
    // output viewer's badge alignment.
    int badge_w = 0;
    for (const auto& b : o->blocks) {
        const bool runnable = code_block_picker::is_shell_language(b.language);
        const std::string lang = b.language.empty() ? std::string{"sh"} : b.language;
        const std::string bd = runnable ? std::string{" \xe2\x96\xb6 "}
                                        : " " + lang + " ";
        badge_w = std::max(badge_w, string_width(bd));
    }
    for (int i = 0; i < static_cast<int>(o->blocks.size()); ++i) {
        const auto& b = o->blocks[static_cast<std::size_t>(i)];
        // First non-blank line of the (cleaned) body as the preview — what
        // the user visually matches against the reply on screen. Skipping
        // leading blanks means a block that opens with a comment/newline
        // still previews its first real command instead of an empty row.
        std::string_view body{b.body};
        while (!body.empty() && (body.front() == '\n' || body.front() == '\r'))
            body.remove_prefix(1);
        auto eol = body.find('\n');
        std::string preview{body.substr(0, eol == std::string_view::npos
                                             ? body.size() : eol)};
        const bool runnable = code_block_picker::is_shell_language(b.language);
        const std::string lang = b.language.empty() ? std::string{"sh"}
                                                    : b.language;

        Picker::Config::Row row;
        // Badge = the run affordance, a stable colour anchor (NOT dimmed on
        // the selected row, so "which of these actually runs" reads at a
        // glance): a ` ▶ ` play glyph in the success hue for a runnable
        // block, or the language tag in muted for one we can only edit/copy.
        if (runnable) {
            row.badge       = " \xe2\x96\xb6 ";           // ▶
            row.badge_style = Style{}.with_fg(success).with_bold();
        } else {
            row.badge       = " " + lang + " ";
            row.badge_style = fg_dim(muted);
        }
        if (int bw = string_width(row.badge); bw < badge_w)
            row.badge.append(static_cast<std::size_t>(badge_w - bw), ' ');
        // Leading: the 1-9 fast-path number (the affordance that teaches the
        // shortcut) + the first-line preview.
        row.leading = std::to_string(i + 1) + "  " + preview;
        row.leading_style  = runnable ? fg_of(fg) : fg_dim(muted);
        // Trailing: language · line count. For a runnable block the language
        // moved into meaning (the ▶ badge), so the tag here is just the
        // dialect; for a non-runnable one the badge already carries it, so
        // show only the size to avoid repeating the tag.
        row.trailing = (runnable ? lang + " \xc2\xb7 " : std::string{})
                     + std::to_string(b.line_count)
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
    auto* r = m.ui.overlay.get<ov::CodeBlockResult>();
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

    // Full capture, line-numbered like the tool output viewer: a right-
    // aligned gutter + a status-hued │ pipe (red on a failed/timed-out run,
    // muted otherwise), then the raw line. The numbers make it easy to say
    // "line 42 is where it broke" and match the Ctrl+O viewer so the two
    // output surfaces read the same.
    //
    // Manual windowing (same discipline as tool_output_viewer): the capture
    // is capped at 2 MB — up to tens of thousands of lines — so building an
    // Element per line into a scroll container that lays out ALL of them
    // would hitch on every keypress. Instead materialise the rows ONCE into
    // a static cache keyed by the output bytes, then feed only the visible
    // [y, y+vh) slice to the picker (cfg.scroll = nullptr), so a frame costs
    // O(viewport) regardless of capture size.
    struct ResultBodyCache {
        const void*          bytes_key = nullptr;
        std::size_t          bytes_len = 0;
        bool                 ok        = false;
        std::vector<Element> rows;
    };
    static ResultBodyCache cache;   // UI thread only — same discipline as the pickers' statics

    const void* bytes_key = static_cast<const void*>(r->output.data());
    if (cache.bytes_key != bytes_key || cache.bytes_len != r->output.size()
        || cache.ok != ok_exit) {
        cache.bytes_key = bytes_key;
        cache.bytes_len = r->output.size();
        cache.ok        = ok_exit;
        cache.rows.clear();

        std::string_view out{r->output};
        if (out.empty()) {
            cache.rows.push_back(
                text("  (no output captured)", fg_italic(muted))
                | height(1) | overflow(Overflow::Hidden));
        } else {
            const Color pipe_hue = ok_exit ? muted : danger;
            std::vector<std::string_view> lines;
            {
                std::size_t pos = 0;
                while (pos <= out.size()) {
                    std::size_t eol = out.find('\n', pos);
                    std::size_t len =
                        (eol == std::string_view::npos ? out.size() : eol) - pos;
                    lines.push_back(out.substr(pos, len));
                    if (eol == std::string_view::npos) break;
                    pos = eol + 1;
                }
            }
            const int gutter_w = static_cast<int>(
                std::to_string(std::max<std::size_t>(1, lines.size())).size());
            cache.rows.reserve(lines.size());
            for (std::size_t i = 0; i < lines.size(); ++i) {
                std::string num = std::to_string(i + 1);
                if (static_cast<int>(num.size()) < gutter_w)
                    num.insert(0, gutter_w - num.size(), ' ');
                cache.rows.push_back(
                    hstack().width(Dimension::percent(100))(
                      text("  " + num + " ", fg_dim(warn)),
                      text("\xe2\x94\x82 ", fg_dim(pipe_hue)),   // │
                      text(std::string{lines[i]},
                           ok_exit ? fg_of(muted) : fg_of(fg))
                          | clip | grow(1.0f) | shrink(1.0f)
                    ).build()
                    | height(1) | overflow(Overflow::Hidden));
            }
        }
    }

    // Window the cached rows to the viewport; maintain scroll bounds here so
    // the reducer's clamp stays correct, and feed only the visible slice.
    const int total_rows = static_cast<int>(cache.rows.size());
    const int vh = std::max(1, cfg.viewport_h);
    auto& sc = m.ui.code_blocks_scroll;
    sc.max_y = std::max(0, total_rows - vh);
    sc.y     = std::clamp(sc.y, 0, sc.max_y);
    cfg.scroll = nullptr;
    const int first = sc.y;
    const int last  = std::min(total_rows, first + vh);
    for (int i = first; i < last; ++i)
        cfg.items.push_back(cache.rows[static_cast<std::size_t>(i)]);

    cfg.footer.push_back(text(""));
    // Scroll-position readout: which lines of the capture are on screen —
    // the manual window has no scrollbar, so this is the affordance that
    // there is more output above/below (same grammar as the tool viewer).
    if (total_rows > vh) {
        cfg.footer.push_back(text(
            "  " + std::to_string(first + 1) + "\xe2\x80\x93"   // –
                 + std::to_string(last) + " / "
                 + std::to_string(total_rows) + " lines",
            fg_dim(muted)));
    }
    cfg.footer.push_back(key_hints({
        {"a", "attach to composer", 6},
        {"y", "copy", 4},
        {"Esc", "discard", 4},
    }));

    return Picker{std::move(cfg)}.build();
}

// Ctrl+O tool-output viewer. Two stages inside one Picker chrome:
//
//   LIST  — one row per settled tool call (newest first): a category-
//           coloured tool badge ("Read", "Bash", "Edit" — same hue as
//           the transcript card), the detail line, and "ok · 1.2s ·
//           48 KB" trailing. Enter opens the body.
//   BODY  — the FULL stored output of the selected call in the
//           scrollable region (the timeline card elides long bodies;
//           this is where the elided middle lives). ←/→ hop straight
//           to the previous/next output; Esc returns to the list.
//
// An overlay — not in-place card expansion — because the transcript's
// committed rows are immutable native scrollback; growing a card there
// would rewrite committed rows (HardReset corruption class). The overlay
// paints strictly over the live viewport, same as every other picker.
Element tool_output_viewer(const Model& m) {
    const auto* o = m.ui.overlay.get<ov::ToolViewer>();
    if (!o) return nothing();

    const int sz = static_cast<int>(o->entries.size());
    const int cur = std::clamp(o->index, 0, std::max(0, sz - 1));

    Picker::Config cfg;
    // Overlay stretch supplies all available columns. A large minimum used to
    // force the picker past phone/SSH terminal bounds and clip its right side;
    // keep only the border's structural floor and let row flex do the rest.
    cfg.min_width  = 1;
    cfg.viewport_h = picker_viewport_h();
    cfg.scroll     = &m.ui.tool_viewer_scroll;

    // "n/N" position — shown in both stages so the user always knows
    // where they are in the output stack.
    const std::string pos =
        std::to_string(cur + 1) + "/" + std::to_string(sz);

    if (!o->viewing) {
        // ── LIST stage ──
        cfg.title    = " Tool Outputs \xc2\xb7 " + std::to_string(sz) + " ";
        cfg.accent   = highlight;
        cfg.selected = sz == 0 ? -1 : cur;
        // LIST chrome is border+padding (4) plus blank+hint footer (2).
        // At truly tiny heights, drop the optional footer and devote every
        // remaining row to selectable outputs.
        const int list_term_rows = picker_terminal_rows();
        const bool compact_list = list_term_rows < 7;
        cfg.viewport_h = std::clamp(
            list_term_rows - (compact_list ? 4 : 6), 1, kViewportH);

        // Badge column width: longest display name across the entries,
        // so every detail line starts at the same column and the badge
        // hues read as a vertical colour strip.
        int badge_w = 0;
        for (const auto& e : o->entries)
            badge_w = std::max(badge_w, string_width(e.title));

        cfg.rows.reserve(o->entries.size());
        for (int i = 0; i < sz; ++i) {
            const auto& e = o->entries[static_cast<std::size_t>(i)];
            Picker::Config::Row row;
            const Color cat_hue = tool_category_color(e.name);
            if (e.is_live) {
                // The currently-running tool, pinned to the top. A bright
                // "● LIVE" badge in the tool's hue reads as "streaming now";
                // the detail line carries the tool name so the row is still
                // self-describing.
                std::string badge = "\xe2\x97\x8f LIVE";
                const int live_w = string_width(badge);
                if (live_w < badge_w)
                    badge.append(static_cast<std::size_t>(badge_w - live_w), ' ');
                row.badge          = std::move(badge);
                row.badge_style    = fg_bold(cat_hue);
                row.leading        = e.title + (e.detail.empty() ? "" : "  " + e.detail);
                row.leading_style  = fg_of(fg);
                row.trailing       = e.trailing;
                row.trailing_style = fg_dim(cat_hue);
                row.selected       = (i == cur);
                cfg.rows.push_back(std::move(row));
                continue;
            }
            row.badge = e.title;
            const int title_w = string_width(e.title);
            if (title_w < badge_w)
                row.badge.append(static_cast<std::size_t>(badge_w - title_w), ' ');
            // Category hue — the same colour identity the transcript
            // card used, so "which tool was that?" is answered by hue
            // before the label is even read. Failures go red on the
            // badge too: status outranks category.
            row.badge_style    = e.failed ? fg_bold(danger)
                                          : fg_bold(cat_hue);
            row.leading        = e.detail.empty() ? std::string{"\xe2\x80\xa6"}
                                                  : e.detail;
            row.leading_style  = e.failed ? fg_of(danger) : fg_of(fg);
            row.trailing       = e.trailing;
            row.trailing_style = e.failed ? fg_of(danger) : fg_dim(muted);
            row.selected       = (i == cur);
            cfg.rows.push_back(std::move(row));
        }
        if (!compact_list) {
            cfg.footer.push_back(text(""));
            cfg.footer.push_back(key_hints({
                {"\xe2\x86\x91\xe2\x86\x93", "move", 5},   // ↑↓
                {"Enter", "view", 6},
                {"y", "copy", 4},
                {"Esc", "close", 3},
            }));
        }
        return Picker{std::move(cfg)}.build();
    }

    // ── BODY stage ──
    const auto& e = o->entries[static_cast<std::size_t>(cur)];
    const Color tool_hue = e.failed ? danger : tool_category_color(e.name);
    cfg.title    = (e.is_live ? std::string(" \xe2\x97\x8f LIVE \xc2\xb7 ") + e.title
                              : " " + e.title)
                 + " \xc2\xb7 " + pos + " ";
    cfg.accent   = tool_hue;
    cfg.selected = -1;   // read-only — no cursor row; manual scroll rules
    // Normal BODY chrome is 9 rows: border+padding (4), header+separator
    // (2), blank+range+hints footer (3). Below 10 terminal rows there is
    // room for none of that optional chrome, so the border title carries
    // identity and every remaining row goes to output. This keeps the modal
    // inside all viable terminal heights (5+ rows) instead of inheriting the
    // shared picker's four-row viewport floor.
    const int body_term_rows = picker_terminal_rows();
    const bool compact_body = body_term_rows < 10;
    cfg.viewport_h = std::clamp(
        body_term_rows - (compact_body ? 4 : 9), 1, kViewportH);

    // Header: coloured tool name + detail, then the status line — the
    // user never loses track of WHICH output they're reading, and ←/→
    // visibly swaps this header as they hop entries. Full-width hstack
    // so the position indicator pins right even on narrow terminals.
    if (!compact_body) {
        cfg.header.push_back(
            hstack().width(Dimension::percent(100))(
                text(" " + e.title, fg_bold(tool_hue))
                    | clip | shrink(1.0f),
                text(e.detail.empty() ? "" : "  " + e.detail, fg_of(fg))
                    | clip | grow(1.0f) | shrink(3.0f),
                text(e.trailing + " ", e.failed ? fg_of(danger) : fg_dim(muted))
                    | clip | shrink(2.0f)
            ).build());
        cfg.header.push_back(sep);
    }

    // Body rows — built ONCE per viewed entry, then windowed per frame.
    //
    // Why: a 256 KiB output renders to thousands of row Elements. The
    // previous shape rebuilt the full ToolBodyPreview tree (deep-copying
    // the output through its Config) on EVERY key repeat, and handed the
    // whole thing to a scroll container that laid out ALL rows to paint
    // ~20 — the "viewer lags/hangs" report. Now:
    //   * the rows are materialised once into a function-local cache
    //     keyed by (entries identity, index, output bytes identity);
    //   * each frame we copy only the visible [y, y+vh) slice into the
    //     picker — no scroll container, no full-content layout — so a
    //     frame costs O(viewport) regardless of output size;
    //   * scroll bounds are written to the host ScrollState here (the
    //     reducer's clamp reads max_y), replacing the widget writeback
    //     the scroll container used to do.
    //
    // The cache is safe across frames: entries are snapshotted at open
    // (immutable while Open), the vector buffer is stable under Model
    // moves, and the output-bytes key guards against allocator reuse
    // after a close/reopen.
    struct BodyCache {
        const void* entries_key = nullptr;
        int         index       = -1;
        const void* bytes_key   = nullptr;
        std::size_t bytes_len   = 0;
        std::uint64_t call_key  = 0;
        std::vector<Element> rows;
    };
    static BodyCache cache;   // UI thread only — same discipline as pickers’ statics

    const void* entries_key = static_cast<const void*>(o->entries.data());
    const void* bytes_key   = static_cast<const void*>(e.output.data());
    const auto call_key     = e.call.compute_render_key();
    if (cache.entries_key != entries_key || cache.index != cur
        || cache.bytes_key != bytes_key || cache.bytes_len != e.output.size()
        || cache.call_key != call_key) {
        cache.entries_key = entries_key;
        cache.index       = cur;
        cache.bytes_key   = bytes_key;
        cache.bytes_len   = e.output.size();
        cache.call_key    = call_key;
        cache.rows.clear();

        using Kind = maya::ToolBodyPreview::Kind;
        auto bp = tool_body_preview_config(e.call);
        const bool structured =
            !e.failed && (bp.kind == Kind::EditDiff || bp.kind == Kind::GitDiff
                       || bp.kind == Kind::FileRead || bp.kind == Kind::FileWrite
                       || bp.kind == Kind::TodoList);

        if (structured) {
            bp.show_all   = true;    // no "⋯ N more" elision — full output
            bp.tail_only  = e.is_live;  // live: newest pinned bottom; settled: from top
            bp.show_streaming_placeholder = false;
            Element body = maya::ToolBodyPreview{std::move(bp)}.build();
            // The preview renders as one vstack of row Elements. Explode
            // it so the window slice below can address individual rows;
            // the wrapper vstack carries no styling of its own.
            if (auto* box = maya::as_box(body);
                box && box->layout.direction == maya::FlexDirection::Column
                && !box->children.empty()) {
                cache.rows.reserve(box->children.size());
                for (auto& child : box->children) {
                    // Manual viewport accounting below is row-based. Keep
                    // every structured preview child to exactly one visual
                    // row just like the plain-text fallback; otherwise a
                    // narrow FileWrite/Todo label can wrap while max_y still
                    // counts it as one, making later rows unreachable.
                    cache.rows.push_back(
                        std::move(child)
                        | height(1)
                        | overflow(Overflow::Hidden));
                }
            } else {
                cache.rows.push_back(
                    std::move(body)
                    | height(1)
                    | overflow(Overflow::Hidden));
            }
        } else if (e.output.empty()) {
            cache.rows.push_back(
                text("  (no output captured)", fg_italic(muted))
                | height(1)
                | overflow(Overflow::Hidden));
        } else {
            // Line-numbered fallback: right-aligned gutter + dim pipe in
            // the tool's category hue (red pipe on failure), then the
            // raw line. Every line of every plain-text output numbered.
            const Color pipe_hue = e.failed ? danger : tool_hue;
            std::vector<std::string_view> lines;
            {
                std::string_view b{e.output};
                std::size_t p = 0;
                while (p <= b.size()) {
                    std::size_t nl = b.find('\n', p);
                    std::size_t len = (nl == std::string_view::npos ? b.size() : nl) - p;
                    lines.push_back(b.substr(p, len));
                    if (nl == std::string_view::npos) break;
                    p = nl + 1;
                }
            }
            const int gutter_w = static_cast<int>(
                std::to_string(std::max<std::size_t>(1, lines.size())).size());
            cache.rows.reserve(lines.size());
            for (std::size_t i = 0; i < lines.size(); ++i) {
                std::string num = std::to_string(i + 1);
                if (static_cast<int>(num.size()) < gutter_w)
                    num.insert(0, gutter_w - num.size(), ' ');
                cache.rows.push_back(
                    hstack().width(Dimension::percent(100))(
                      text("  " + num + " ", fg_dim(warn)),
                      text("\xe2\x94\x82 ", fg_dim(pipe_hue)),   // │
                      text(std::string{lines[i]},
                           e.failed ? fg_of(danger) : fg_of(muted))
                          | clip | grow(1.0f) | shrink(1.0f)
                    ).build()
                    | height(1) | overflow(Overflow::Hidden));
            }
        }
    }

    // Window the cached rows to the viewport. No scroll container — the
    // picker paints the slice inline; scroll bounds are maintained here
    // so the reducer's clamp (ToolViewerMove) stays correct.
    const int total_rows = static_cast<int>(cache.rows.size());
    const int vh = std::max(1, cfg.viewport_h);
    auto& sc = m.ui.tool_viewer_scroll;
    sc.max_y = std::max(0, total_rows - vh);
    // Live entry tails by default: pin to the newest output (bottom) unless
    // the user has scrolled up to read earlier lines. `auto_tail` re-engages
    // when they scroll back to the bottom (maintained in the reducer). For a
    // settled entry the saved scroll position rules.
    if (e.is_live && m.ui.tool_viewer_tail)
        sc.y = sc.max_y;
    sc.y     = std::clamp(sc.y, 0, sc.max_y);
    cfg.scroll = nullptr;
    const int first = sc.y;
    const int last  = std::min(total_rows, first + vh);
    for (int i = first; i < last; ++i)
        cfg.items.push_back(cache.rows[static_cast<std::size_t>(i)]);

    if (total_rows == 0)
        cfg.items.push_back(
            text("  waiting for output\xe2\x80\xa6", fg_italic(muted))
            | height(1)
            | overflow(Overflow::Hidden));

    if (!compact_body) {
        cfg.footer.push_back(text(""));
        // Position line: which rows of the output are on screen — the manual
        // window has no scrollbar, so this is the scroll affordance.
        if (total_rows > vh) {
            std::string pos_line =
                "  " + std::to_string(first + 1) + "\xe2\x80\x93"      // –
                     + std::to_string(last) + " / "
                     + std::to_string(total_rows) + " rows";
            if (e.is_live && m.ui.tool_viewer_tail)
                pos_line += "  \xc2\xb7 tailing";
            cfg.footer.push_back(text(pos_line,
                fg_dim(e.is_live && m.ui.tool_viewer_tail ? tool_hue : muted)));
        }
        std::vector<Hint> viewer_hints;
        if (e.is_live) {
            viewer_hints = {
                {"\xe2\x86\x91\xe2\x86\x93", "scroll", 5},        // ↑↓
                {"End", "tail", 4},
                {"\xe2\x86\x90\xe2\x86\x92", "prev/next", 4},     // ←→
                {"Esc", "back", 3},
            };
        } else {
            viewer_hints = {
                {"\xe2\x86\x91\xe2\x86\x93", "scroll", 5},        // ↑↓
                {"\xe2\x86\x90\xe2\x86\x92", "prev/next", 4},     // ←→
                {"y", "copy", 4},
                {"Esc", "back", 3},
            };
        }
        cfg.footer.push_back(key_hints(std::move(viewer_hints)));
    }
    return Picker{std::move(cfg)}.build();
}

// Rewind checkpoint picker. One row per checkpointed user turn (oldest at
// the top, newest at the bottom nearest the composer — same spatial order
// as the transcript). Each row: turn number + one-line prompt preview
// (leading), and "Nm ago · diffstat" (trailing) where the diffstat shows
// what the worktree has changed SINCE that point so the rewind is never
// blind. Enter rewinds; the destructive files+transcript revert is the
// existing RestoreCheckpoint flow.
Element checkpoint_picker(const Model& m) {
    auto* o = m.ui.overlay.get<ov::Checkpoints>();
    if (!o) return nothing();

    // Relative "time ago" from a wall-clock ms stamp — local to the view;
    // no shared helper exists and the grammar here is picker-specific.
    auto ago = [](std::int64_t ts_ms) -> std::string {
        if (ts_ms <= 0) return {};
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::int64_t s = (now - ts_ms) / 1000;
        if (s < 0)     s = 0;
        if (s < 45)    return "just now";
        if (s < 3600)  return std::to_string(s / 60)   + "m ago";
        if (s < 86400) return std::to_string(s / 3600) + "h ago";
        return std::to_string(s / 86400) + "d ago";
    };

    Picker::Config cfg;
    cfg.title      = " Rewind to Checkpoint ";
    cfg.accent     = warn;
    cfg.min_width  = 52;
    cfg.viewport_h = picker_viewport_h();
    cfg.scroll     = &m.ui.checkpoints_scroll;
    cfg.selected   = o->entries.empty() ? -1 : o->index;

    cfg.rows.reserve(o->entries.size());
    for (int i = 0; i < static_cast<int>(o->entries.size()); ++i) {
        const auto& e = o->entries[static_cast<std::size_t>(i)];
        Picker::Config::Row row;
        row.leading = "#" + std::to_string(e.turn) + "  " + e.preview;
        row.leading_style = fg_of(fg);

        // Trailing: "<time> · <diffstat>". The diffstat is filled in async;
        // until then a subtle ellipsis so the row doesn't jump.
        std::string when = ago(e.timestamp_ms);
        std::string stat;
        switch (e.diff_state) {
            case checkpoint_picker::Entry::DiffState::Loading:
                stat = "\xe2\x80\xa6";   // …
                break;
            case checkpoint_picker::Entry::DiffState::Failed:
                stat = "";
                break;
            case checkpoint_picker::Entry::DiffState::Ready:
                if (e.clean) {
                    stat = "no changes";
                } else {
                    stat = std::to_string(e.files_changed)
                         + (e.files_changed == 1 ? " file" : " files");
                    if (e.insertions > 0) stat += " +" + std::to_string(e.insertions);
                    if (e.deletions  > 0) stat += " \xe2\x88\x92" + std::to_string(e.deletions); // −
                }
                break;
        }
        std::string trailing = when;
        if (!stat.empty())
            trailing += (when.empty() ? "" : " \xc2\xb7 ") + stat;
        row.trailing = std::move(trailing);
        // Green when a real rewind (has changes), dim when clean/no-stat.
        const bool has_changes =
            e.diff_state == checkpoint_picker::Entry::DiffState::Ready && !e.clean;
        row.trailing_style = has_changes ? fg_of(success) : fg_dim(muted);
        row.selected = (i == o->index);
        cfg.rows.push_back(std::move(row));
    }

    cfg.footer.push_back(text(""));
    cfg.footer.push_back(text(
        "  Restores files and rewinds the transcript here.",
        fg_dim(muted)));
    cfg.footer.push_back(key_hints({
        {"\xe2\x86\x91\xe2\x86\x93", "move", 5},   // ↑↓
        {"Enter", "rewind", 3},
        {"Esc", "cancel", 4},
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
