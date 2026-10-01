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
#include "agentty/tool/util/sandbox_broker.hpp"   // blocked_feed()
#include "agentty/tool/util/handoff_gate.hpp"     // handoff_feed()

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

    // Only the walls that are NOT strong.
    //
    // This used to list all twelve, and that was the mistake. Every capability
    // folded into one run-on line is a paragraph, and a paragraph next to a
    // form is what the eye skips -- so the one `none` in the list read exactly
    // like the eleven `strong`s around it. The full report now lives on the
    // ROWS (annotate_sandbox_form), where it answers the question the user
    // actually has, which is about the row they just changed.
    //
    // What stays here is the part no row owns: the walls that came out weaker
    // than asked for. Those are the pane's reason to exist, and a short list
    // of three is read where a list of twelve is not.
    std::string out;
    for (const auto& w : p.preview.walls) {
        if (w.strength == "strong") continue;
        // host.kernel_isolation is `none` on every process backend by
        // definition, not as a degradation -- see docs/design/
        // sandbox-boundary.md §13. Listing it here every time would train the
        // user to ignore this line, which is the opposite of the point.
        if (w.name == "host.kernel_isolation") continue;
        if (!out.empty()) out += "  \xc2\xb7  ";
        out += w.name + ": " + w.strength;
        if (!w.mechanism.empty()) out += " (" + w.mechanism + ")";
    }
    if (!out.empty()) out = "weaker than asked \xe2\x80\xa2 " + out;
    else
        out = "every wall this policy asks for is enforced strongly";

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

// What the sandbox actually stopped, this session.
//
// The line that changes behaviour. A wall report says what WOULD be enforced;
// this says what was. "cargo tried ptrace ATTACH pid=1" tells you what your
// toolchain does, where "your build failed" tells you nothing and sends people
// to turn the sandbox off.
//
// Newest FIRST here, reversing the feed's storage order, because the footer has
// a line or two of room and the most recent denial is the one you are debugging.
[[nodiscard]] std::string blocked_summary() {
    const auto feed = tools::util::sandbox::broker::blocked_feed();
    if (feed.empty()) return {};

    std::string out = "blocked: ";
    // At most three, oldest dropped. The whole feed can be 64 entries and the
    // footer is not a log viewer -- a summary that wraps the pane has stopped
    // summarising. The log has all of them (channel `tool`, site
    // `sandbox.broker`), which is where you go when three is not enough.
    std::size_t shown = 0;
    for (auto it = feed.rbegin(); it != feed.rend() && shown < 3; ++it, ++shown) {
        if (shown) out += "  \xc2\xb7  ";
        out += it->syscall + " " + it->detail;
        // The count only when it is >1: "× 1" is noise, "× 4096" is the
        // difference between a stray call and a loop hammering a denial.
        if (it->count > 1) out += " \xc3\x97 " + std::to_string(it->count);
    }
    if (feed.size() > shown)
        out += "  (+" + std::to_string(feed.size() - shown) + " more in the log)";
    return out;
}

// What the agent wrote that the HOST will later trust.
//
// Structurally separate from blocked_summary() above, and that is the point
// rather than tidiness: a blocked syscall is the sandbox working, and a trust
// handoff is the sandbox being *bypassed* -- the agent wrote an ordinary file
// in a directory it is allowed to write, and something outside the sandbox will
// execute it later. No wall in the report above can see that, because by the
// time VS Code runs .vscode/tasks.json there is no sandbox in the picture.
//
// So this is the one line in the footer that reports on a boundary the wall
// report cannot describe, which is exactly why it gets its own line instead of
// being folded in.
[[nodiscard]] std::string handoff_summary() {
    const auto feed = tools::util::handoff::handoff_feed();
    if (feed.empty()) return {};

    std::string out = "host-trusted writes: ";
    std::size_t shown = 0;
    for (auto it = feed.rbegin(); it != feed.rend() && shown < 2; ++it, ++shown) {
        if (shown) out += "  \xc2\xb7  ";
        // The basename, not the full path: the footer is narrow and the
        // interesting half of "/home/x/repo/.vscode/tasks.json" is the end.
        std::string_view p{it->write.path};
        if (const auto slash = p.rfind('/'); slash != std::string_view::npos)
            p.remove_prefix(slash + 1);
        // The KIND, not just the name. "tasks.json" means nothing to most
        // people; "an editor task -- VS Code can run this when the folder
        // opens" is the whole reason to care.
        out += std::string{p} + " (" +
               std::string{sandbox_cfg::explain(it->kind)} + ")";
    }
    if (feed.size() > shown)
        out += "  (+" + std::to_string(feed.size() - shown) + " more)";
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

    // Denials go ABOVE the wall report, because they outrank it: the walls
    // describe what would be enforced, and this is what actually happened.
    if (const auto blocked = blocked_summary(); !blocked.empty())
        form.note = blocked + "\n" + form.note;

    // And trust handoffs above THOSE, because they outrank a denial. A blocked
    // syscall is the sandbox holding; a handoff is the one thing here that
    // escapes it without breaking it, and it is the escape class every incident
    // in Pillar's July 2026 series actually used.
    if (const auto hand = handoff_summary(); !hand.empty())
        form.note = hand + "\n" + form.note;

    // The one thing this pane must never leave unsaid: a save here does NOT
    // change the running sandbox.
    //
    // The policy is sealed when the process starts (tool/util/sandbox.hpp),
    // so edits land on disk and take effect next launch. Everywhere else in
    // agentty saving means "in force now", so the exception has to be stated
    // rather than assumed -- a user who tightens the syscall filter and keeps
    // working would otherwise trust a wall that is not up yet. That is the
    // same class of lie as "sandbox: active" on a host that cannot sandbox,
    // only pointed at the future.
    //
    // Rendered FIRST in the note and marked as replacing the shared grammar,
    // because it outranks the wall report: the walls describe what WOULD be
    // enforced, and this says when.
    // The one thing this pane must never leave unsaid: a save here does NOT
    // change the running sandbox -- AND whether the next launch will actually
    // deliver what the rows describe.
    //
    // The second half was missing. The footer promised "applies on restart"
    // with nothing checking that claim, while the preview already knew three
    // ways it could be false (will not compile, engine cannot start, a
    // capability silently degrades). A promise about the future with no check
    // behind it is the same bug as "sandbox: active" with no sandbox, just
    // pointed forward -- so `restart_outcome` is consulted and its verdict
    // REPLACES the bare promise rather than sitting next to it.
    const auto& outcome = o->pane.restart_note;

    if (o->pane.saved_pending_restart) {
        form.note = (outcome.empty()
                        ? std::string{"saved \xc2\xb7 applies on restart, verified against "
                                      "this host (the running sandbox is unchanged)"}
                        : "saved \xc2\xb7 " + outcome)
                  + "\n" + form.note;
    } else if (form.dirty) {
        form.note = (outcome.empty()
                        ? std::string{"^S saves for the next launch \xc2\xb7 the running "
                                      "sandbox cannot be changed"}
                        : "^S saves, but " + outcome)
                  + "\n" + form.note;
    } else if (!outcome.empty()) {
        // Not dirty and not saved: the policy already on disk is one this host
        // cannot fully deliver. Worth saying unprompted -- otherwise the only
        // way to discover it is to edit something.
        form.note = outcome + "\n" + form.note;
    }

    return maya::Panel{form_config(form, info,
                                   &m.ui.sandbox_scroll,
                                   panel_viewport_h(),
                                   panel_terminal_cols())}.build();
}

}  // namespace agentty::ui
