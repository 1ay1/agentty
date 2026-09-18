// skills.cpp — the read-only skills viewer.
//
// Pure adapter: builds maya::Panel::Config from Model state; the widget
// owns every chrome decision. Shared scaffolding: panels_prologue.hpp.
//
// ── What this panel is for ───────────────────────────────────────────────
//
// It answers four questions without the user leaving the TUI:
//
//   what is installed?          the list
//   what did it declare?        the effects chips
//   is it approved?             the trust marker
//   what did agentty notice?    the findings, in the detail footer
//
// It does NOT approve anything. That is a deliberate omission, not a gap:
// Anthropic measured 93% approval on Claude Code permission prompts, and
// an approve key one keystroke from a list you are already scrolling is
// the fastest possible route to a habituated yes. Approval lives behind
// `agentty skill approve NAME`, where the consent screen can show the
// findings AND the body.
//
// ── Row layout ───────────────────────────────────────────────────────────
//
//   leading    ! name            — the ! only when something needs a human
//   trailing   exec net · PENDING
//
// Colour carries severity, but never alone: every state also has a word or
// a glyph, because a colourblind user and a `theme::native` terminal both
// have to be able to read this.

#include "panels_prologue.hpp"

#include "agentty/runtime/panel/skills.hpp"
#include "agentty/tool/skills.hpp"

namespace agentty::ui {

Element skills_panel(const Model& m) {
    auto* o = m.ui.panel.get<pn::Skills>();
    if (!o) return nothing();

    Panel::Config cfg;
    cfg.title      = " Skills ";
    cfg.min_width  = kPanelStandard;
    cfg.viewport_h = panel_viewport_h();
    cfg.scroll     = &o->scroll;
    cfg.selected   = o->rows.empty() ? -1 : o->index;

    // Accent follows the worst state present, so the border itself carries
    // the headline before a single row is read.
    const auto flagged = o->flagged_count();
    const auto pending = o->pending_count();
    cfg.accent = flagged ? danger : (pending ? warn : highlight);

    cfg.items.reserve(o->rows.size());
    for (const auto& r : o->rows) {
        Panel::Item row;

        // A glyph, not just a colour: "!!" for something screening flagged,
        // "!" for unapproved-but-clean, two spaces for the ordinary case so
        // the names stay aligned in a monospace column.
        const char* mark = r.has_critical          ? "!! "
                         : (r.gated && !r.trusted) ? " ! "
                                                   : "   ";
        row.leading = std::string{mark} + r.name;
        row.leading_style = r.has_critical ? fg_of(danger)
                          : (r.gated && !r.trusted) ? fg_of(warn)
                                                    : fg_of(fg);

        // Trailing: what it declared, then its trust state. Prose skills
        // say "prose" rather than nothing — an empty column reads as
        // missing data, and "declares nothing" is a real answer.
        std::string decl = tools::skills::effects_to_frontmatter(r.effects);
        if (decl.empty()) decl = "prose";

        std::string state;
        if (r.gated) state = r.trusted ? "approved" : "PENDING";

        std::string trailing = decl;
        if (!state.empty()) trailing += " \xc2\xb7 " + state;   // ·
        row.trailing = std::move(trailing);
        row.trailing_style = (r.gated && !r.trusted) ? fg_of(warn)
                                                     : fg_dim(muted);

        cfg.items.push_back(std::move(row));
    }

    // ── Detail for the selected row ──────────────────────────────────────
    // The list answers "what is installed"; the footer answers "what is
    // THIS one", so the user never has to leave the panel to find out why a
    // row is marked.
    cfg.footer.push_back(text(""));

    if (const auto* sel = o->selected()) {
        if (!sel->description.empty()) {
            // Already sanitised at scan time — safe to render inside our
            // own chrome. (A description that can open a new line can
            // impersonate the headings around it.)
            cfg.footer.push_back(text("  " + sel->description, fg_dim(muted)));
            cfg.footer.push_back(text(""));
        }

        // Provenance. "you wrote this" vs "this arrived from elsewhere" is
        // the single most decision-relevant fact about a skill, so it gets
        // a plain sentence rather than a field label.
        if (!sel->origin.empty())
            cfg.footer.push_back(text("  from " + sel->origin, fg_dim(muted)));
        else if (sel->source == "project")
            cfg.footer.push_back(text("  came with this repo", fg_dim(muted)));
        else
            cfg.footer.push_back(text("  you wrote this", fg_dim(muted)));

        cfg.footer.push_back(text("  " + sel->dir, fg_dim(muted)));

        // What screening saw. Capped at four lines: a footer nobody can
        // read is a footer nobody reads, and the CLI shows the full set.
        if (!sel->findings.empty()) {
            cfg.footer.push_back(text(""));
            cfg.footer.push_back(text("  agentty noticed:", fg_of(fg)));

            int shown = 0;
            for (const auto& f : sel->findings) {
                if (shown++ >= 4) break;
                const bool crit =
                    f.severity == tools::skills::Finding::Severity::Critical;
                std::string line = "   ";
                line += crit ? "!! " : " ! ";
                if (f.line > 0) line += "line " + std::to_string(f.line) + "  ";
                line += f.detail;
                cfg.footer.push_back(
                    text(std::move(line), crit ? fg_of(danger) : fg_of(warn)));
            }
            if (sel->findings.size() > 4)
                cfg.footer.push_back(
                    text("   \xe2\x80\xa6 " +
                             std::to_string(sel->findings.size() - 4) +
                             " more \xe2\x80\x94 agentty skill list",
                         fg_dim(muted)));

            // The honest caveat, every time findings are shown. Without it
            // an empty findings list reads as a clean bill of health.
            cfg.footer.push_back(text(""));
            cfg.footer.push_back(
                text("  pattern matching, not proof \xe2\x80\x94 and no findings"
                     " is not a clean bill.",
                     fg_dim(muted)));
        }

        // The action, named but not bound. Saying WHERE the decision lives
        // is more useful than a key that makes it too easy.
        if (sel->gated && !sel->trusted) {
            cfg.footer.push_back(text(""));
            cfg.footer.push_back(
                text("  not approved \xe2\x80\x94 the agent cannot load it.",
                     fg_of(warn)));
            cfg.footer.push_back(
                text("  review and approve:  agentty skill approve "
                         + sel->name,
                     fg_dim(muted)));
        }
    } else {
        cfg.footer.push_back(
            text("  No skills installed.", fg_dim(muted)));
        cfg.footer.push_back(
            text("  Drop a SKILL.md under ~/.agentty/skills/, or:", fg_dim(muted)));
        cfg.footer.push_back(
            text("  agentty skill add ./path", fg_dim(muted)));
    }

    // Summary line: the headline count, only when there is something to
    // count. Silence is the right output for a healthy set.
    if (flagged || pending) {
        cfg.footer.push_back(text(""));
        std::string sum = "  ";
        if (flagged)
            sum += std::to_string(flagged)
                 + (flagged == 1 ? " skill flagged" : " skills flagged");
        if (flagged && pending) sum += ", ";
        if (pending)
            sum += std::to_string(pending) + " awaiting approval";
        cfg.footer.push_back(text(std::move(sum),
                                  flagged ? fg_of(danger) : fg_of(warn)));
    }

    cfg.footer.push_back(key_hints({
        {"\xe2\x86\x91\xe2\x86\x93", "move", 5},   // ↑↓
        {"Esc", "close", 4},
    }));

    return Panel{std::move(cfg)}.build();
}

} // namespace agentty::ui
