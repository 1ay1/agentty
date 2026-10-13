---
title: Hooks
description: Run your own shell commands around every tool call — block a dangerous command before it runs, or log and react after. Consent-gated by design.
nav_section: Tools
nav_order: 45
slug: hooks
---

Hooks are shell commands agentty runs **around tool calls**. A `pre_tool` hook fires before a tool executes and can **block** it; a `post_tool` hook fires after a tool produces a result. Use them to enforce a policy the model can't override — reject any `bash` command touching `/etc`, log every file write to your audit system, run a formatter after each edit.

Hooks are **consent-gated**: a hooks file never runs until you explicitly approve it in a shell, and any change to it re-gates. This is deliberate — a hook is arbitrary code that runs on your machine.

## The two events

| Event | Fires | Can it block? |
|-------|-------|---------------|
| **`pre_tool`** | just before a tool executes | **yes** — exit non-zero blocks the call |
| **`post_tool`** | just after a tool returns a result | no — fire-and-forget |

These are the only two events; there are no session/turn/prompt hooks. Both are scoped to individual **tool calls**.

## Defining hooks

Author a `hooks.json` file. The first that exists wins:

1. `.agentty/hooks.json` (project)
2. `~/.agentty/hooks.json` (all projects)

```json
{
  "pre_tool": [
    { "match": "bash", "run": "~/.agentty/guard.sh" }
  ],
  "post_tool": [
    { "run": "logger -t agentty" }
  ]
}
```

Each entry has:

| Field | Required | Meaning |
|-------|----------|---------|
| `run` | **yes** | the shell command to execute |
| `match` | no | a POSIX extended regex matched against the **tool name**; omit to match every tool |

`match` is a regex on the tool name only (not arguments or paths). A malformed regex fails closed — the hook simply never fires.

## What a hook receives

A hook is a normal shell command, run through the **same OS sandbox as the `bash` tool** — it's workspace-confined exactly like model-driven shell. agentty passes context through environment variables:

| Variable | Value |
|----------|-------|
| `AGENTTY_HOOK_EVENT` | `pre_tool` or `post_tool` |
| `AGENTTY_HOOK_TOOL` | the tool name (`bash`, `edit`, …) |
| `AGENTTY_HOOK_PAYLOAD_FILE` | path to a mode-`0600` temp file holding the JSON payload |

The payload file contains:

```json
// pre_tool
{ "event": "pre_tool", "tool": "bash", "args": "<tool args as JSON string>" }

// post_tool
{ "event": "post_tool", "tool": "bash", "args": "…", "result": "<tool output>" }
```

The payload travels via a file (not stdin) so a multi-MB tool result can't blow the environment-size limit; `result` is truncated to 4 MiB.

## What a hook can do

- **`pre_tool` — block or allow.** Exit **non-zero to block** the tool call; the hook's **captured output becomes the reason** shown to the model as the tool error (or, if the hook printed nothing, `blocked by pre_tool hook (\`<cmd>\` exited N)`). Exit **0 to allow**. The first blocking hook wins.
- **`post_tool` — observe.** Its exit code is ignored; it can't rewrite the result or block anything. Use it for logging, notifications, or side effects.

A hook **cannot** inject text into the conversation or modify a tool's output. A `pre_tool` hook's only lever is *block with a reason*.

Each hook command is capped at **30 seconds**. Set `AGENTTY_NO_HOOKS=1` to disable all hooks entirely.

### Example: block writes outside the repo

```sh
#!/bin/sh
# ~/.agentty/guard.sh — a pre_tool hook on `bash`/`edit`/`write`
payload=$(cat "$AGENTTY_HOOK_PAYLOAD_FILE")
case "$payload" in
  *'/etc/'*|*'~/.ssh'*)
    echo "refusing: touches a protected path"
    exit 1 ;;   # non-zero → blocked, message shown to the model
esac
exit 0
```

## Approving a hooks file

A hooks file — or **any byte change** to one — is **inert until you approve it**. Approval stores the file's SHA-256 in `~/.agentty/hooks_approved.json`; change one byte and it re-gates. Unapproved hook commands are **never executed**.

```bash
agentty hooks           # or: agentty hooks list — show hooks + approval state
agentty hooks approve   # prints the file, asks y/N, stores the hash
```

In the app, [[Ctrl+K]] → **Hooks** shows the file and its approval state, but pressing Enter there **won't** approve inline — it points you back to `agentty hooks approve` in a shell. Approval is deliberately a terminal action so the y/N prompt is never owned by a picker you might dismiss by reflex.

:::warn
A hook runs arbitrary code on your machine with your permissions (inside the workspace sandbox). Only approve a `hooks.json` whose `run` commands you've read. The consent gate exists precisely so a synced dotfile or a cloned repo can't run code without you seeing it first.
:::

## Related

- [Sandboxing & permissions](/docs/sandboxing) — the OS sandbox hooks run inside, and the permission profiles for tools.
- [Tools](/docs/tools) — the native tools whose names you match on.
