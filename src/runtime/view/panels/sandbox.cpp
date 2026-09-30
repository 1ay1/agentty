// sandbox.cpp — the Sandbox pane (filesystem, network, syscalls, limits).
//
// The rows are a plain form, projected through the shared form_config like
// every other form pane, so this pane contributes no chrome of its own.
//
// What IS specific here is the subtitle and the note: together they are the
// live wall report, and the report is the reason the pane exists.
//
// ── Why the report, and not just the switches ────────────────────────────
//
// A boundary you cannot observe is a boundary you cannot trust. The failure
// mode is never "the switch was set wrong" -- it is "the switch said active
// and nothing was enforced", and agentty has shipped exactly that (issue #21:
// "sandbox: active" while every command died on a uid_map denial). Two more
// of the same shape turned up while building this: the claybin backend was
// compiled out of the default build, so every row here configured dead code;
// and a missing mremap in the syscall profile made curl report "out of
// memory" on a machine with free RAM, which reads like a network denial and
// is not one.
//
// So the pane reports what is actually enforcing, per capability, straight
// out of claybin's GuaranteeReport -- and names the MECHANISM, because
// "strong" alone is a claim while "strong via landlock abi 10" is a claim
// with its receipt attached.
//
// It is a compile, not a spawn: claybin's compile() is pure over a described
// host, so the report tracks the rows on every keystroke without running
// anything. The reducer recomputes it after each edit, which is what keeps it
// from ever describing a config the rows no longer say.

#include "panels_prologue.hpp"
#include "agentty/runtime/view/form_panel.hpp"
#include "agentty/runtime/panel/sandbox.hpp"

#include <string>

namespace agentty::ui {

namespace {

// The walls, folded into one line per capability. Rendered in the form's
// `note` (the pane-specific footer) rather than as hand-built elements: the
// whole point of form_config is that no pane draws its own chrome.
[[nodiscard]] std::string wall_summary(const pn::SandboxPane& p) {
    if (!p.preview.compiled) {
        // A policy that will not compile is the most important thing on
        // screen: the boundary the rows describe cannot be built at all.
        // claybin refuses rather than degrading, so the error is specific
        // and worth showing verbatim.
        return "cannot enforce: " + p.preview.error;
    }

    std::string out;
    for (const auto& w : p.preview.walls) {
        if (!out.empty()) out += "  \xc2\xb7  ";
        out += w.name + ": " + w.strength;
        if (!w.mechanism.empty()) out += " (" + w.mechanism + ")";
    }

    // Anything the policy ASKED for that this host cannot deliver. Never
    // folded in with the walls above: a capability that silently degraded is
    // the exact lie this pane exists to prevent, so it gets its own clause.
    if (!p.preview.unenforceable.empty()) {
        out += "\nthis host cannot enforce: ";
        for (std::size_t i = 0; i < p.preview.unenforceable.size(); ++i) {
            if (i) out += ", ";
            out += p.preview.unenforceable[i];
        }
    }
    return out;
}

}  // namespace

maya::Element sandbox_panel(const Model& m) {
    auto* o = m.ui.panel.get<ui::panel::Sandbox>();
    if (!o) return nothing();

    // The form is rebuilt by the reducer, but the report is a VIEW concern:
    // it is derived, it is only ever read here, and threading it through the
    // model would give two owners for one string.
    auto form = o->pane.form;
    form.note = wall_summary(o->pane);

    return maya::Panel{form_config(form, info,
                                   &m.ui.sandbox_scroll,
                                   panel_viewport_h(),
                                   panel_terminal_cols())}.build();
}

}  // namespace agentty::ui
