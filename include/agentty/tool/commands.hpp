#pragma once
// agentty::tools::commands — user-authored slash commands.
//
// A slash command is a markdown prompt TEMPLATE the user invokes from the
// composer as `/name [arguments]`. On submit, the template body (with
// argument placeholders substituted) replaces the typed text and the turn
// runs as if the user had typed the expanded prompt. Commands are prompt
// MACROS — they complement the command palette (which dispatches host
// ACTIONS like NewThread) rather than overlapping it.
//
// File format (Claude Code compatible — .claude/commands/<name>.md loads
// unchanged):
//
//     ---
//     description: Review a file for security issues.   # optional
//     argument-hint: <file> [focus]                     # optional
//     ---
//     Review $1 for security problems, focusing on $2.
//     Full request: $ARGUMENTS
//
// Placeholders, substituted at submit time:
//   $ARGUMENTS   everything after `/name ` verbatim
//   $1 … $9      whitespace-split positional words (later words are NOT
//                re-joined: $2 of "a b c" is "b"). Absent → empty string.
//   $$           literal dollar sign (escape hatch)
//
// Discovery roots, precedence order (first hit on a name wins) — the exact
// mirror of the skills loader so the two ecosystems feel identical:
//
//   project   <cwd>/.agentty/commands/<name>.md
//   project   <cwd>/.agents/commands/<name>.md
//   project   <cwd>/.claude/commands/<name>.md     (Claude Code compat)
//   user      ~/.agentty/commands/<name>.md
//   user      ~/.agents/commands/<name>.md
//   user      ~/.claude/commands/<name>.md         (Claude Code compat)
//
// Subdirectories namespace the command name with `:` (Claude convention):
// .claude/commands/git/fixup.md is invoked as /git:fixup.
//
// Parsing is LENIENT like skills: missing frontmatter means the whole file
// is the body; a missing description falls back to the body's first line.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agentty::tools::commands {

struct Command {
    std::string name;          // invocation name (subdirs joined with ':')
    std::string description;   // frontmatter or body-first-line fallback
    std::string argument_hint; // frontmatter `argument-hint`
    std::string body;          // the prompt template
    std::string source;        // "project" | "user"
    std::filesystem::path file;// absolute path (diagnostics)
};

// Discover + parse every command under the project + user roots. Cached
// process-wide keyed on the roots' + files' mtimes (edit → next lookup
// re-scans). Bounded: kMaxCommands entries, kMaxBodyBytes per body.
[[nodiscard]] const std::vector<Command>& all();

// Exact-name lookup. nullptr when absent.
[[nodiscard]] const Command* find(std::string_view name);

// Substitute $ARGUMENTS / $1..$9 / $$ in `body` using the raw argument
// string `args` (everything the user typed after `/name `).
[[nodiscard]] std::string expand(std::string_view body, std::string_view args);

// If `text` is a slash-command invocation (`/name` or `/name args`, name
// matching a discovered command), return the fully expanded prompt.
// Otherwise std::nullopt — the text submits unchanged. A leading `/` that
// matches NO command falls through (the user may legitimately start a
// message with a path like /etc/hosts).
[[nodiscard]] std::optional<std::string> try_expand(std::string_view text);

// Test seam: force a re-scan on next all() regardless of mtime cache.
void invalidate_cache();

} // namespace agentty::tools::commands
