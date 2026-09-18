// skills_panel::scan — build the read-only rows.
//
// Done once on open, not per frame: screen_body() walks the body line by
// line, and re-screening every skill every frame would be a cost paid for
// nothing. Same reasoning as the stats panel's projection — the cache
// lives on the open panel because its lifetime is exactly the panel's.

#include "agentty/runtime/panel/skills.hpp"

#include "agentty/tool/skills.hpp"

#include <algorithm>
#include <variant>

namespace agentty::skills_panel {

Open scan() {
    Open out;

    const auto& all = tools::skills::all();
    const auto approvals = tools::skills::load_approvals();

    out.rows.reserve(all.size());
    for (const auto& s : all) {
        Row r;
        // Sanitise at the boundary, once. Everything downstream of here is
        // safe to render inside agentty's own chrome — which matters because
        // the detail pane puts a description directly under agentty's
        // headings, and author text that can open a new line can impersonate
        // them (91% of confirmed-malicious skills carry injection).
        r.name        = tools::skills::sanitize_author_text(s.name, 64);
        r.description = tools::skills::sanitize_author_text(s.description, 300);
        r.origin      = tools::skills::sanitize_author_text(s.origin, 96);
        r.source      = s.source;
        r.dir         = s.dir.string();
        r.effects     = s.effects;
        r.resource_count = s.resources.size();

        r.gated = tools::skills::needs_trust_gate(s.effects);
        r.trusted = std::holds_alternative<scope::Trusted>(
            tools::skills::trust_of(s, approvals));

        // Screen every skill, not just effectful ones. The shadow-feature
        // finding is the whole reason: capability in the body with nothing
        // in the declaration showed up in 100% of advanced attacks in the
        // 98k-skill study, so "declares nothing" is not a reason to skip.
        r.findings = tools::skills::screen_body(s.body);
        r.has_critical = std::ranges::any_of(
            r.findings, [](const tools::skills::Finding& f) {
                return f.severity == tools::skills::Finding::Severity::Critical;
            });

        // Is another directory in the same scope claiming this name? The
        // losing copy is invisible everywhere — including this panel — so
        // the winner has to carry the warning, or nothing does.
        r.shadow_conflict = tools::skills::shadowed_within_scope(s.name);

        out.rows.push_back(std::move(r));
    }

    // Order: things needing attention first, then alphabetical. A viewer
    // that buries the one flagged skill under forty clean ones is a viewer
    // that answers the wrong question.
    std::ranges::stable_sort(out.rows, [](const Row& a, const Row& b) {
        const auto rank = [](const Row& r) {
            if (r.has_critical)          return 0;
            if (r.shadow_conflict)       return 1;   // something is hidden
            if (r.gated && !r.trusted)   return 2;
            return 3;
        };
        const int ra = rank(a), rb = rank(b);
        if (ra != rb) return ra < rb;
        return a.name < b.name;
    });

    return out;
}

} // namespace agentty::skills_panel
