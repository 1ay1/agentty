#pragma once
// Command palette — the enum, the label/description table, and the open
// modal's UI state, kept in a single header so adding a new command is a
// one-file change (extend the enum, append a row to `kCommands`, then wire
// the selection in update.cpp's CommandPaletteSelect handler).

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace agentty {

enum class Command : std::uint8_t {
    NewThread,
    ReviewChanges,
    AcceptAll,
    RejectAll,
    CycleProfile,
    OpenModels,
    OpenProviders,
    OpenThreads,
    OpenPlan,
    RunCodeBlock,
    InspectToolOutputs,
    CompactContext,
    SmartMode,
    ResetSmartLearning,
    RewindCheckpoint,
    ForkThread,
    OpenPlugins,
    OpenCommands,
    OpenAgents,
    OpenHooks,
    OpenRagSettings,
    OpenLogin,
    SignOut,
    UpdateAgentty,
    Quit,
};

struct CommandDef {
    Command     id;
    const char* label;
    const char* description;
    const char* shortcut;   // direct global keybinding, or "" if palette-only
};

inline constexpr std::array kCommands = std::array{
    CommandDef{Command::NewThread,     "New thread",         "Start a fresh conversation", "Ctrl+N"},
    CommandDef{Command::ReviewChanges, "Review changes",     "Open the diff review pane", "Ctrl+R"},
    CommandDef{Command::AcceptAll,     "Accept all changes", "Apply every pending hunk", ""},
    CommandDef{Command::RejectAll,     "Reject all changes", "Discard every pending hunk", ""},
    CommandDef{Command::CycleProfile,  "Cycle profile",      "Write → Ask → Minimal", "Shift+Tab"},
    CommandDef{Command::OpenModels,    "Open model picker",  "Switch the active model", "Ctrl+/"},
    CommandDef{Command::OpenProviders, "Switch provider",    "Choose the LLM backend (Anthropic, OpenAI, …)", "Ctrl+P"},
    CommandDef{Command::OpenThreads,   "Open threads",       "Browse saved conversations", "Ctrl+J"},
    CommandDef{Command::OpenPlan,      "Open plan",          "View task progress", "Ctrl+T"},
    CommandDef{Command::RunCodeBlock,  "Run code block",     "Run a fenced block from the last reply", "Ctrl+G"},
    CommandDef{Command::InspectToolOutputs, "Inspect tool outputs", "Read tool outputs — the running tool is the live top row", "Ctrl+O"},
    CommandDef{Command::CompactContext,"Compact context",    "Replace history with a structured summary", ""},
    CommandDef{Command::SmartMode,     "Smart Mode",         "Configure role-based routing — send cheap grunt work to a cheaper model", "Ctrl+S"},
    CommandDef{Command::ResetSmartLearning, "Reset Smart Mode learning", "Forget this repo's learned routing priors + captured decompositions", ""},
    CommandDef{Command::RewindCheckpoint,"Rewind to checkpoint","Restore files + conversation to any earlier turn", ""},
    CommandDef{Command::ForkThread,    "Fork thread",        "Branch into a fresh thread with near-zero context — the parent transcript stays readable on demand", ""},
    CommandDef{Command::OpenPlugins,   "Plugins",            "MCP servers (mcp.json) — list & remove; add with `agentty plugin add`", ""},
    CommandDef{Command::OpenCommands,  "Slash commands",     "Discovered /commands — author in .agentty/commands/*.md", ""},
    CommandDef{Command::OpenAgents,    "Subagents",          "Task agent types — built-ins + your .agentty/agents/*.md", ""},
    CommandDef{Command::OpenHooks,     "Hooks",              "Lifecycle hooks + approval state (.agentty/hooks.json)", ""},
    CommandDef{Command::OpenRagSettings,"RAG",               "How proactive retrieval behaves: on / first turn only / off", ""},
    CommandDef{Command::OpenLogin,     "Sign in / add account", "Sign in — or add another OAuth / API-key account", ""},
    CommandDef{Command::SignOut,       "Sign out",           "Remove saved credentials and re-open sign-in", ""},
    CommandDef{Command::UpdateAgentty, "Update agentty",     "Download + install the new release (shown when one is available)", ""},
    CommandDef{Command::Quit,          "Quit",               "Exit agentty", "Ctrl+C"},
};

// Case-insensitive substring filter over kCommands. Returns the matching
// CommandDef pointers in their catalog order. Single source of truth for
// "what's visible in the palette right now" \u2014 both the view (rendering)
// and the dispatcher (resolving cursor index \u2192 Command) call this so they
// can never disagree about which command sits at which row.
//
// The previous design had the view filter independently and the dispatcher
// switch on the cursor's *raw* position into kCommands. With any non-empty
// query the two indices drift: typing "thread" left "Open threads" at
// visible row 1, but row 1 in the unfiltered enum was `ReviewChanges` \u2014
// pressing Enter ran the wrong command.
[[nodiscard]] inline std::vector<const CommandDef*>
filtered_commands(std::string_view query, bool update_available = true) {
    auto lower = [](unsigned char c) -> char {
        return static_cast<char>(std::tolower(c));
    };
    std::string needle;
    needle.reserve(query.size());
    for (char c : query) needle.push_back(lower(static_cast<unsigned char>(c)));

    std::vector<const CommandDef*> out;
    out.reserve(kCommands.size());
    for (const auto& cmd : kCommands) {
        // "Update agentty" only exists while an update is actually known —
        // a permanent-but-dead palette row would train users to ignore it.
        if (cmd.id == Command::UpdateAgentty && !update_available) continue;
        if (needle.empty()) { out.push_back(&cmd); continue; }
        // Match against label + description + shortcut so discovery works by
        // intent, not just the exact command name: "diff" finds "Review
        // changes", "api" finds "Switch provider", "ctrl+g" finds "Run code
        // block". Label matches still rank first (see the two-pass sort below).
        std::string hay;
        for (const char* field : {cmd.label, cmd.description, cmd.shortcut})
            for (const char* p = field; p && *p; ++p)
                hay.push_back(lower(static_cast<unsigned char>(*p)));
        if (hay.find(needle) != std::string::npos)
            out.push_back(&cmd);
    }
    // Rank label hits above description/shortcut-only hits so typing a command
    // name surfaces it at the top even when the same substring appears in some
    // other row's description. Stable within each group (catalog order).
    std::stable_partition(out.begin(), out.end(),
        [&](const CommandDef* c) {
            std::string lab;
            for (const char* p = c->label; p && *p; ++p)
                lab.push_back(lower(static_cast<unsigned char>(*p)));
            return lab.find(needle) != std::string::npos;
        });
    return out;
}

// Sum-type state, same shape as the other picker variants in
// `runtime/picker.hpp`. The query buffer + selected index live ONLY
// inside the Open alternative — they cannot exist while the palette
// is closed (used to be a bool + two fields where the bool gated their
// validity by convention; now the type system enforces it).
namespace palette {
struct Closed {};
struct Open {
    std::string query;
    int         index = 0;
};
} // namespace palette

using CommandPaletteState = std::variant<palette::Closed, palette::Open>;

// Index of `cmd` in the blank-query palette list (which is exactly
// kCommands order). Used to re-open the palette focused on the row the
// user last selected, e.g. after Esc-ing back out of a settings picker.
// Returns 0 if not found (defensive; every Command is in kCommands).
[[nodiscard]] inline int palette_index_of(Command cmd) noexcept {
    for (int i = 0; i < static_cast<int>(kCommands.size()); ++i)
        if (kCommands[static_cast<std::size_t>(i)].id == cmd) return i;
    return 0;
}

[[nodiscard]] inline bool is_open(const CommandPaletteState& s) noexcept {
    return std::holds_alternative<palette::Open>(s);
}
[[nodiscard]] inline       palette::Open* opened(CommandPaletteState& s)       noexcept { return std::get_if<palette::Open>(&s); }
[[nodiscard]] inline const palette::Open* opened(const CommandPaletteState& s) noexcept { return std::get_if<palette::Open>(&s); }

} // namespace agentty
