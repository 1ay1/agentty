#pragma once
// Skills viewer — a read-only window onto what is installed and what it
// can do.
//
// Open it from the command palette (Ctrl+K → "Skills"). ↑/↓ move, Esc
// closes. Nothing here mutates anything, and that is deliberate: this
// panel does NOT approve skills.
//
// ── Why there is no approve key ──────────────────────────────────────────
//
// Anthropic measured that Claude Code users approve 93% of permission
// prompts; Akhawe & Felt measured 70% clickthrough on Chrome's SSL
// interstitial. An approval that is one keystroke away from a list you
// are already scrolling is an approval nobody reads — it becomes the
// habituated default, which is exactly the failure mode the install flow
// was designed to avoid.
//
// So the panel ANSWERS questions ("what is installed, what did it
// declare, what did agentty notice in it, is it approved") and refers the
// one action out to `agentty skill approve NAME`, where the consent
// screen can show the findings and the body. Visibility here, decision
// there.
//
// The same reasoning rules out a "trust all skills" toggle. A switch like
// that becomes line one of every install guide.
//
// ── What lives where ─────────────────────────────────────────────────────
//
//   tool/skills.hpp          Skill, EffectSet, trust_of, screen_body
//   panel/skills.hpp         this: selection + scroll, nothing else
//   view/panels/skills.cpp   the view: Model → SkillsPane::Config
//
// The rows are a PROJECTION of what is on disk. Like the stats panel, the
// cache lives on the open panel because its lifetime is exactly the
// panel's: opening it scans, closing it frees, and a user who never opens
// it pays nothing.

#include "agentty/scope/scope.hpp"
#include "agentty/tool/skills.hpp"

#include <maya/core/scroll_state.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace agentty::skills_panel {

// One row: a skill, resolved. Everything the view needs, computed once on
// open rather than per frame — screen_body() walks the body line by line,
// which is cheap but not free, and a panel that re-screened every skill on
// every frame would be paying that for nothing.
struct Row {
    std::string name;
    std::string description;   // already sanitised — see below
    std::string origin;        // "" for hand-authored
    std::string source;        // "user" | "project"
    std::string dir;
    tools::EffectSet effects{};

    // Resolved trust at scan time.
    bool gated   = false;      // declares effects, so trust applies
    bool trusted = false;      // approved, or prose, or user-authored

    // What screening found in the body. Empty for almost every skill.
    std::vector<tools::skills::Finding> findings;
    bool has_critical = false;

    std::size_t resource_count = 0;
};

struct Open {
    // Which row is selected. Mutable because the view corrects it when the
    // list shrinks underneath (a skill removed in another terminal) —
    // pointing past the end would render an empty detail pane with no
    // explanation.
    mutable int index = 0;

    // Body scroll, OWNED BY THE PANEL — same reasoning as stats_panel::Open:
    // a scroll the view reads must be a scroll the frame gate sees, and
    // visual_parts + parts_cover_all prove that at the type. Living on
    // Model::UI is what made the "panel ignored me, then caught up when I
    // typed" class of bug possible.
    mutable maya::ScrollState scroll = [] {
        maya::ScrollState s;
        s.auto_dispatch = false;   // the reducer owns this offset
        return s;
    }();

    // The scan. Filled on open; not refreshed per frame.
    //
    // Author-controlled strings in here are ALREADY sanitised
    // (sanitize_author_text) because they are rendered inside agentty's own
    // chrome — 91% of confirmed-malicious skills in Snyk's Feb-2026 audit
    // carried prompt injection, and a description that can open a new line
    // can impersonate the frame around it.
    std::vector<Row> rows;

    [[nodiscard]] bool empty() const noexcept { return rows.empty(); }

    [[nodiscard]] const Row* selected() const noexcept {
        if (rows.empty()) return nullptr;
        const auto i = static_cast<std::size_t>(
            index < 0 ? 0
                      : (static_cast<std::size_t>(index) >= rows.size()
                             ? rows.size() - 1
                             : static_cast<std::size_t>(index)));
        return &rows[i];
    }

    // Counts for the header line — computed here so the view stays a pure
    // formatter and the numbers can be asserted in a test without a render.
    [[nodiscard]] std::size_t pending_count() const noexcept {
        std::size_t n = 0;
        for (const auto& r : rows) if (r.gated && !r.trusted) ++n;
        return n;
    }

    [[nodiscard]] std::size_t flagged_count() const noexcept {
        std::size_t n = 0;
        for (const auto& r : rows) if (r.has_critical) ++n;
        return n;
    }
};

// Scan every discovered skill into rows: resolve trust, screen the body,
// sanitise author text. Pure with respect to the Model — it reads the
// skills store and the approvals file, and returns.
[[nodiscard]] Open scan();

} // namespace agentty::skills_panel
