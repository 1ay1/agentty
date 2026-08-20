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

// Palette section a command belongs to. Rendered as a colour-coded badge
// (hue-stable under the cursor) so the flat list reads as grouped families,
// and folded into the fuzzy filter so `changes` surfaces the whole cluster.
// Order here defines the section order in the palette.
enum class Category : std::uint8_t {
    Thread,     // new / fork / compact / rewind
    Changes,    // review / accept / reject
    Navigate,   // threads / plan / tool outputs / code block
    Config,     // profile / model / provider / smart / rag / plugins / hooks / commands / agents
    Account,    // sign in / out
    General,    // quit / update
};

[[nodiscard]] constexpr std::string_view category_label(Category c) noexcept {
    switch (c) {
        case Category::Thread:   return "Thread";
        case Category::Changes:  return "Changes";
        case Category::Navigate: return "Go";
        case Category::Config:   return "Config";
        case Category::Account:  return "Account";
        case Category::General:  return "";
    }
    return "";
}

struct CommandDef {
    Command     id;
    const char* label;
    const char* description;
    const char* shortcut;   // direct global keybinding, or "" if palette-only
    Category    category = Category::General;
    bool        danger   = false;  // destructive: discards work / mutates worktree
};

inline constexpr std::array kCommands = std::array{
    // ── Thread ──────────────────────────────────────────────────────────
    CommandDef{Command::NewThread,     "New thread",         "Start a fresh conversation", "Ctrl+N", Category::Thread},
    CommandDef{Command::ForkThread,    "Fork thread",        "Branch into a fresh thread with near-zero context — the parent transcript stays readable on demand", "", Category::Thread},
    CommandDef{Command::CompactContext,"Compact context",    "Replace history with a structured summary", "", Category::Thread},
    CommandDef{Command::RewindCheckpoint,"Rewind to checkpoint","Restore files + conversation to any earlier turn", "", Category::Thread, /*danger=*/true},
    // ── Changes ─────────────────────────────────────────────────────────
    CommandDef{Command::ReviewChanges, "Review changes",     "Open the diff review pane", "Ctrl+R", Category::Changes},
    CommandDef{Command::AcceptAll,     "Accept all changes", "Apply every pending hunk", "", Category::Changes},
    CommandDef{Command::RejectAll,     "Reject all changes", "Discard every pending hunk", "", Category::Changes, /*danger=*/true},
    // ── Go (navigate) ───────────────────────────────────────────────────
    CommandDef{Command::OpenThreads,   "Open threads",       "Browse saved conversations", "Ctrl+J", Category::Navigate},
    CommandDef{Command::OpenPlan,      "Open plan",          "View task progress", "Ctrl+T", Category::Navigate},
    CommandDef{Command::InspectToolOutputs, "Inspect tool outputs", "Read tool outputs — the running tool is the live top row", "Ctrl+O", Category::Navigate},
    CommandDef{Command::RunCodeBlock,  "Run code block",     "Run a fenced block from the last reply", "Ctrl+G", Category::Navigate},
    // ── Config ──────────────────────────────────────────────────────────
    CommandDef{Command::CycleProfile,  "Cycle profile",      "Write → Ask → Minimal", "Shift+Tab", Category::Config},
    CommandDef{Command::OpenModels,    "Switch model",       "Switch the active model", "Ctrl+/", Category::Config},
    CommandDef{Command::OpenProviders, "Switch provider",    "Choose the LLM backend (Anthropic, OpenAI, …)", "Ctrl+P", Category::Config},
    CommandDef{Command::SmartMode,     "Smart Mode",         "Configure role-based routing — send cheap grunt work to a cheaper model", "Ctrl+S", Category::Config},
    CommandDef{Command::ResetSmartLearning, "Reset Smart Mode learning", "Forget this repo's learned routing priors + captured decompositions", "", Category::Config, /*danger=*/true},
    CommandDef{Command::OpenRagSettings,"Retrieval (RAG)",   "How proactive retrieval behaves: on / first turn only / off", "", Category::Config},
    CommandDef{Command::OpenPlugins,   "MCP servers",        "Plugins / MCP servers (mcp.json) — list & remove; add with `agentty plugin add`", "", Category::Config},
    CommandDef{Command::OpenCommands,  "Slash commands",     "Discovered /commands — author in .agentty/commands/*.md", "", Category::Config},
    CommandDef{Command::OpenAgents,    "Subagents",          "Task agent types — built-ins + your .agentty/agents/*.md", "", Category::Config},
    CommandDef{Command::OpenHooks,     "Hooks",              "Lifecycle hooks + approval state (.agentty/hooks.json)", "", Category::Config},
    // ── Account ─────────────────────────────────────────────────────────
    CommandDef{Command::OpenLogin,     "Sign in / add account", "Sign in — or add another OAuth / API-key account", "", Category::Account},
    CommandDef{Command::SignOut,       "Sign out",           "Remove saved credentials and re-open sign-in", "", Category::Account, /*danger=*/true},
    // ── General ─────────────────────────────────────────────────────────
    CommandDef{Command::UpdateAgentty, "Update agentty",     "Download + install the new release (shown when one is available)", "", Category::General},
    CommandDef{Command::Quit,          "Quit",               "Exit agentty", "Ctrl+C", Category::General},
};

// Live context that decides which conditionally-visible rows appear. A dead
// row (Accept-all with no diff, Run-code-block with no fenced reply) trains
// users to distrust the palette, so we hide it — the same discipline already
// applied to "Update agentty". Defaults keep every row visible so callers
// that don't care (tests, palette_index_of) behave as before.
struct PaletteContext {
    bool update_available   = true;
    bool has_pending_changes = true;   // gates Review / Accept-all / Reject-all
    bool has_code_block     = true;    // gates Run code block
};

// True iff `cmd` should be VISIBLE given the live context. Keeping this in one
// predicate (rather than scattered `if`s) means the view and dispatcher agree
// on visibility for free, since both go through filtered_commands().
[[nodiscard]] inline bool command_visible(const CommandDef& cmd,
                                          const PaletteContext& ctx) noexcept {
    switch (cmd.id) {
        case Command::UpdateAgentty: return ctx.update_available;
        case Command::ReviewChanges:
        case Command::AcceptAll:
        case Command::RejectAll:     return ctx.has_pending_changes;
        case Command::RunCodeBlock:  return ctx.has_code_block;
        default:                     return true;
    }
}

// Case-insensitive substring filter over kCommands. Returns the matching
// CommandDef pointers in their catalog order. Single source of truth for
// "what's visible in the palette right now" — both the view (rendering)
// and the dispatcher (resolving cursor index → Command) call this so they
// can never disagree about which command sits at which row.
//
// The previous design had the view filter independently and the dispatcher
// switch on the cursor's *raw* position into kCommands. With any non-empty
// query the two indices drift: typing "thread" left "Open threads" at
// visible row 1, but row 1 in the unfiltered enum was `ReviewChanges` —
// pressing Enter ran the wrong command.
[[nodiscard]] inline std::vector<const CommandDef*>
filtered_commands(std::string_view query, PaletteContext ctx) {
    auto lower = [](unsigned char c) -> char {
        return static_cast<char>(std::tolower(c));
    };
    std::string needle;
    needle.reserve(query.size());
    for (char c : query) needle.push_back(lower(static_cast<unsigned char>(c)));

    std::vector<const CommandDef*> out;
    out.reserve(kCommands.size());
    for (const auto& cmd : kCommands) {
        if (!command_visible(cmd, ctx)) continue;
        if (needle.empty()) { out.push_back(&cmd); continue; }
        // Match against label + description + shortcut + CATEGORY so discovery
        // works by intent, not just the exact command name: "diff" finds
        // "Review changes", "api" finds "Switch provider", "changes" surfaces
        // the whole Changes cluster, "ctrl+g" finds "Run code block". Label
        // matches still rank first (see the two-pass sort below).
        std::string hay;
        for (const char* field : {cmd.label, cmd.description, cmd.shortcut})
            for (const char* p = field; p && *p; ++p)
                hay.push_back(lower(static_cast<unsigned char>(*p)));
        for (char c : category_label(cmd.category))
            hay.push_back(lower(static_cast<unsigned char>(c)));
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

// Back-compat overload: the original (query, update_available) shape. Keeps
// existing call sites + tests working; new call sites pass a PaletteContext.
[[nodiscard]] inline std::vector<const CommandDef*>
filtered_commands(std::string_view query, bool update_available = true) {
    return filtered_commands(query, PaletteContext{.update_available = update_available});
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
