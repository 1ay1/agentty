---
title: Slash Commands
description: Author reusable prompt macros as Markdown files and invoke them from the composer as /name — with argument substitution. Claude Code compatible.
nav_section: Tools
nav_order: 55
slug: slash-commands
---

A slash command is a **prompt template** you invoke from the composer as `/name [arguments]`. On submit, the template — with your arguments substituted in — replaces what you typed and the turn runs as if you'd written the whole thing. Commands are prompt **macros**: save the phrasing of a task you run often ("review this file for security issues", "write a conventional-commit message for the staged diff") and fire it with a few keystrokes.

They complement the [command palette](/docs/keybindings) ([[Ctrl+K]]), which dispatches app *actions* like New Thread. The common actions can be typed as `/name` too (see below).

## Authoring a command

Drop a Markdown file in `.agentty/commands/` (project) or `~/.agentty/commands/` (all projects). The file name is the command name; the body is the prompt template.

```markdown
---
description: Review a file for security issues.   # optional
argument-hint: <file> [focus]                     # optional
---
Review $1 for security problems, focusing on $2.
Full request: $ARGUMENTS
```

Save that as `.agentty/commands/review.md` and type `/review auth.cpp tokens` — the model receives *"Review auth.cpp for security problems, focusing on tokens…"*.

| Frontmatter | Meaning |
|-------------|---------|
| `description` | one-line summary shown in the `/` menu (falls back to the body's first line) |
| `argument-hint` | usage hint shown under the menu for the selected command, e.g. `<file> [focus]` |

Both are optional — a file with no frontmatter is treated as the whole body.

## Argument substitution

Placeholders are replaced at submit time:

| Placeholder | Expands to |
|-------------|-----------|
| `$ARGUMENTS` | everything you typed after `/name `, verbatim |
| `$1` … `$9` | whitespace-split positional words (`$2` of `a b c` is `b`; later words are not re-joined) |
| `$$` | a literal dollar sign (escape hatch) |

A placeholder with no matching argument expands to an empty string, so a command degrades gracefully when called with fewer args than it references.

## Invoking

Type `/` at the start of the composer. A menu opens right above the composer and filters as you type. What you type stays in the composer the whole time.

The menu lists everything you can run by name, in this order:

- built-in actions: `/new`, `/compact`, `/model`, `/rename <title>`, `/fork`, `/rewind`, `/review`, `/threads`, `/settings`, `/login`, `/quit`
- your commands (this page)
- [skills](/docs/skills): `/name task` loads the skill and runs the task with it
- MCP server prompts: `/name args`, filled in from the prompt's arguments

A name taken by an earlier group wins, so a command file called `new.md` doesn't replace `/new`.

Keys while it's open:

| Key | Does |
|-----|------|
| [[Tab]] | fill in the selected name and a space, ready for arguments |
| [[Enter]] | run it if what you typed is a full name, otherwise fill it in like Tab |
| [[Up]] / [[Down]], [[Ctrl+P]] / [[Ctrl+N]] | pick a row |
| [[Esc]] | hide the menu and keep the text |

Enter never runs a guess. If the name you typed isn't a known one, the menu closes by itself and the text is sent as a normal message, so starting a message with a path like `/etc/hosts` works.

The same names work over [ACP](/docs/acp) and with `agentty run "/name args"`.

## Namespacing

Subdirectories namespace the command name with `:` — `.agentty/commands/git/fixup.md` is invoked as `/git:fixup`. Nesting up to three levels deep is supported (`a:b:c`).

## Discovery & precedence

Commands are discovered from six roots; on a name collision the **first** hit wins (project shadows user):

```text
<cwd>/.agentty/commands/<name>.md      # project
<cwd>/.agents/commands/<name>.md
<cwd>/.claude/commands/<name>.md       # Claude Code compatible
~/.agentty/commands/<name>.md          # user (all projects)
~/.agents/commands/<name>.md
~/.claude/commands/<name>.md
```

:::tip
Because agentty reads `.claude/commands/`, your existing **Claude Code slash commands load unchanged** — same file format, same `$ARGUMENTS` / `$1` placeholders, same subdirectory namespacing.
:::

Files are picked up live (an mtime signature refreshes the cache), so editing a command takes effect without a restart. Up to 128 commands, 64 KiB each.

## Browsing commands in the app

Type `/` to see them all. Settings → **Slash commands** also lists every command and where it came from (project or user). To author one, create the `.md` file. The [[Ctrl+K]] palette shows each action's `/name` next to it.

## Related

- [Skills](/docs/skills) — model-invoked capability packs (same lenient Markdown + frontmatter format, different purpose: the *model* pulls a skill in; *you* fire a slash command).
- [Subagents](/docs/subagents) — delegate a whole task to an isolated agent.
- [Keybindings](/docs/keybindings) — the command palette for app actions.
