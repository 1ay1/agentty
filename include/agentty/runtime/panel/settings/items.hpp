// settings_items.hpp — the per-concern row model for the settings pickers
// (Ctrl+K → Plugins/Commands/Agents/Hooks). Built fresh each call from the
// live loaders so the picker always mirrors what's actually on disk.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "agentty/runtime/panel/settings/categories.hpp"

namespace agentty { struct Model; }

namespace agentty::settings {

// What activating a row does. Kept abstract so the reducer switches on the
// kind and the view renders the hint, without either hard-coding indices.
enum class Action : std::uint8_t {
    None,          // informational row (no Enter action)
    CycleProfile,  // General: Write → Ask → Minimal

    // ── Doors ─────────────────────────────────────────────────
    //
    // Actions that OPEN ANOTHER PANE, kept contiguous and bracketed by
    // kFirstDoor/kLastDoor below. The settings list paints these with a →
    // affordance instead of a health dot, because they have no health —
    // they lead somewhere.
    //
    // Contiguous because the arrow used to be a hand-listed switch, and
    // Appearance was left out of it: the row that opens the largest pane in
    // Settings was the only door without a handle, reading as a value row
    // that happened to do something on Enter. A range check cannot be
    // half-updated the way a case list can.
    OpenRag,       // General: open the RAG mode picker
    OpenAppearance,// General: open the Appearance pane (theme, density, motion)
    OpenSmart,     // General: open Smart Mode config

    ToggleChangesStrip, // General: show/hide the persistent "N changes" banner
    // (No RemovePlugin: removal is the two-step `d` → SettingsListRemove
    // flow, never an Enter action — a one-press destructive Enter and a
    // two-step d coexisting was a trap. See the reducer's Activate arm.)
    TogglePlugin,  // Plugins: enable/disable the WHOLE server (Enter, reversible)
    ToggleTool,    // Plugins: enable/disable one tool (arg=server, arg2=bare)
    ApprovePlugin, // Plugins: trust this project config so its servers connect
    ApproveHooks,  // Hooks: approve the active hooks file

    // ── Appearance ──────────────────────────────────────────────────
    //
    // One action per knob rather than a single CycleUiField(index): the
    // reducer then switches on a name instead of a number, so reordering
    // the rows cannot silently repoint a row at another setting.
    //
    // Appearance is NOT here. It was briefly a set of cyclic rows in this
    // list — Enter to step an enum, eleven of them in a flat wall — and it
    // was the wrong shape twice over: cycling four times to SEE the options
    // is worse than a list of them, and eleven ungrouped toggles have no
    // shape to read. It is a form pane now (panel/appearance.hpp), which is
    // where grouping, per-row help and provenance already live.
};

// Does activating this row OPEN ANOTHER PANE?
//
// One question, asked instead of a list maintained. The settings list uses
// it to paint the → affordance; anything else that needs to distinguish a
// door from a value can ask the same way rather than restating the set.
//
// A range over the contiguous block above, so adding a door between the
// brackets picks up the arrow with no second edit — which is exactly what
// went wrong before: Appearance was added to the enum, wired to a reducer,
// given a row, and left out of the one switch that paints the handle.
[[nodiscard]] constexpr bool opens_pane(Action a) noexcept {
    return a >= Action::OpenRag && a <= Action::OpenSmart;
}

struct Item {
    // Row health/status — drives the LEADING badge so a row communicates its
    // state at a glance (like every other agentty status surface), instead of
    // encoding the Enter ACTION in the badge (a red ✕ "remove" on a healthy
    // server read as an error). Removability is conveyed by the footer, not
    // the badge.
    enum class Status : std::uint8_t {
        Neutral,   // informational / no health signal
        Ok,        // healthy — connected, approved, present  (● green)
        Pending,   // in progress — connecting…              (◌ dim)
        Bad,       // failed / needs attention — error, unapproved (⚠ amber)
    };

    std::string primary;    // left/main text (name)
    std::string secondary;  // dim detail (command line, path, state)
    std::string hint;       // right-aligned action/CLI hint
    Action      action = Action::None;
    Status      status = Status::Neutral;
    std::string arg;        // action payload (plugin/server name)
    std::string arg2;       // secondary payload (e.g. bare tool name)
    bool        indented = false;  // render as a sub-row (a plugin's tool)
    bool        on = true;         // toggle state (for ToggleTool rows)
    // Effectively inactive though PRESENT: a tool under a DISABLED (or not-
    // connected) plugin. Its individual on/off is still shown (◉/○) so you
    // know what it'll be when the plugin comes back, but the whole row is
    // dimmed to signal "this can't run right now." Distinct from `on` (the
    // stored toggle state) so the toggle stays correct while the group reads
    // as off. Only set on tool sub-rows.
    bool        inactive = false;
    // Provenance for a plugin/server row: which scope it came from ("project"
    // / "user" / "explicit") for the badge, and the .agentty dir holding its
    // mcp.json so an edit (remove/toggle) routes to the RIGHT file instead of
    // always the user config. Empty on non-server rows.
    std::string scope_label;
    std::string config_dir;
    // A project-scope server whose config isn't trusted yet — it won't connect
    // until approved. On such a row, Enter APPROVES (grants trust) rather than
    // toggling, and the view shows the approve affordance.
    bool        untrusted = false;
};

// The rows for one category, live. `m` supplies profile/RAG/Smart state
// for the General category; the rest come from the tools:: loaders.
[[nodiscard]] std::vector<Item> items_for(const Model& m, Category cat);

// ── In-TUI add flows ────────────────────────────────────────────────
// Result of an add attempt: a human-readable message for the status toast
// (`ok` false ⇒ it's an error).
struct AddResult { bool ok; std::string message; };

// Plugins: parse a one-line "name command [args…]" spec and write it to
// the user mcp.json via tools::plugin. `--python foo.py` / `--uvx pkg` /
// `--npx pkg` shorthands are expanded exactly like the CLI.
[[nodiscard]] AddResult add_plugin_from_line(const std::string& line);

// Commands / Agents: create a starter <name>.md under the user root
// (~/.agentty/commands or ~/.agentty/agents) with a minimal template, so
// the user can open + edit it. Returns the path in `message` on success.
// Refuses to overwrite an existing file.
[[nodiscard]] AddResult create_starter(Category cat, const std::string& name);

} // namespace agentty::settings
