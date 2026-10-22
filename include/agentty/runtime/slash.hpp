#pragma once
// agentty::slash — what a leading `/name` in a prompt means.
//
// One catalog of every slash name (built-in actions, the user's commands,
// skills, MCP prompts) and one resolver over it. The TUI composer menu, the
// TUI submit, ACP and `agentty run` all use these, so they agree on what
// `/name` is. Pure: no IO, no discovery. Callers build the Catalog from what
// they already loaded.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agentty::slash {

enum class Kind : std::uint8_t { Action, Command, Skill, Prompt };

// Built-in actions that make sense typed. Each runs a host action, not a
// prompt. `takes_args` actions use the rest of the line (`/rename x`).
enum class Action : std::uint8_t {
    New,
    Compact,
    Model,
    Rename,
    Fork,
    Rewind,
    Review,
    Threads,
    Settings,
    Login,
    Quit,
};

struct Entry {
    std::string name;          // without the leading '/'
    std::string description;
    std::string hint;          // argument hint, "" when none
    Kind        kind = Kind::Command;
    Action      action = Action::New;   // only for Kind::Action
    bool        takes_args = false;     // only for Kind::Action
};

struct Catalog {
    std::vector<Entry> entries;   // deduped by name, precedence order
};

// The built-in actions, in the order the menu lists them.
[[nodiscard]] const std::vector<Entry>& builtin_actions();

// Merge sources into one catalog. Earlier sources win a name clash:
// actions, then commands, then skills, then MCP prompts.
struct Source {
    std::string name, description, hint;
};
[[nodiscard]] Catalog make_catalog(const std::vector<Source>& commands,
                                   const std::vector<Source>& skills,
                                   const std::vector<Source>& prompts,
                                   bool with_actions = true);

// The `/name` a text starts with, and what follows it.
struct Head {
    std::string_view name;   // without '/', may be empty ("/")
    std::string_view args;   // after the first whitespace, trimmed at the front
    bool             has_space = false;   // a space follows the name
};
// nullopt unless the text starts with '/' (no leading whitespace).
[[nodiscard]] std::optional<Head> head(std::string_view text);

// What submitting `text` should do.
struct Resolved {
    enum class What : std::uint8_t { Plain, Action, Command, Skill, Prompt };
    What        what = What::Plain;
    const Entry* entry = nullptr;   // into the catalog; null for Plain
    std::string args;               // the rest of the line
};
[[nodiscard]] Resolved resolve(std::string_view text, const Catalog& cat);

// ── The composer menu ────────────────────────────────────────────────────

// Whether the menu should show for this text and caret, and the query it
// filters by. It shows while the caret is inside a leading `/name` token on
// the first line: no space typed yet, no second '/', no newline before it.
struct MenuQuery {
    std::string_view query;   // the name typed so far (may be empty)
};
[[nodiscard]] std::optional<MenuQuery> menu_query(std::string_view text, int cursor);

struct Match {
    const Entry* entry = nullptr;
    int          score = 0;
    std::vector<int> hits;   // byte offsets into entry->name that matched
};
// Entries for `query`, best first: exact name, name prefix, fuzzy name,
// then description hits. Empty query lists everything in catalog order.
[[nodiscard]] std::vector<Match> filter(const Catalog& cat, std::string_view query);

// The text after completing `entry` into `text` (the `/name` token is
// replaced, a space is added, anything after the token is kept) and the
// new caret, just after the space.
struct Completed {
    std::string text;
    int         cursor = 0;
};
[[nodiscard]] Completed complete(std::string_view text, const Entry& entry);

}  // namespace agentty::slash

// ── Model-facing helpers (the TUI) ───────────────────────────────────────
namespace agentty { struct Model; }

namespace agentty::slash {

// The catalog for the TUI: built-ins, then the library's commands and
// skills, then the MCP prompts the plugin snapshot holds.
[[nodiscard]] Catalog catalog_of(const Model& m);

// The menu's rows for the current composer, or empty when it's hidden
// (no leading /name token, dismissed with Esc, or nothing matches).
[[nodiscard]] std::vector<Match> menu_rows(const Model& m, const Catalog& cat);

}  // namespace agentty::slash

// ── Headless callers (ACP, `agentty run`) ───────────────────────────────
#include "agentty/util/io.hpp"

namespace agentty::slash {

// The catalog from disk + the live MCP pool: commands, skills, MCP prompts.
// `with_actions` adds the TUI's built-ins (a headless caller handles its
// own few, like ACP's /new and /compact, before asking).
[[nodiscard]] Catalog discover(Io io, bool with_actions = false);

// What `text` should be sent as, when it names a command, skill or MCP
// prompt: the expanded template, the skill's instructions plus the task, or
// the rendered prompt. nullopt when it isn't one (send `text` as typed).
// `skill_already_active` lets a caller that has the transcript skip
// re-sending a skill body that's already in it.
//
// `usage` is set when the line can't be sent: an MCP prompt missing a
// required argument. The returned text is then a one-line note for the user
// ("/srv:p needs query — try /srv:p <query>"), never a message for the model.
struct Expanded {
    std::string text;
    bool        usage = false;
};
[[nodiscard]] std::optional<Expanded> expand_checked(
    Io io, std::string_view text, const Catalog& cat,
    bool skill_already_active = false);
[[nodiscard]] std::optional<std::string> expand(
    Io io, std::string_view text, const Catalog& cat,
    bool skill_already_active = false);

}  // namespace agentty::slash
