---
title: Permission Profiles
description: Ask, Write, and Minimal — how agentty gates writes, shell, and network.
nav_section: User Manual
nav_order: 30
slug: profiles
---

A profile decides which tool effects run automatically and which prompt you first. Cycle them anytime with [[S-Tab]]; your choice persists across sessions.

| Profile | Pure reads | Writes / edits | Shell / build | Network |
|---|---|---|---|---|
| **Write** | auto | auto | auto | auto |
| **Ask** (default) | auto | prompt | prompt | prompt |
| **Minimal** | prompt | prompt | prompt | prompt |

## Ask (default)

Read-only tools run automatically; writes, shell calls, and network calls each prompt before running. The safe default for an unfamiliar repo.

## Write (autonomous)

Everything runs without prompting. Use this when you trust the task and want agentty to move fast — paired with the sandbox and workspace boundary, an autonomous run still can't escape your project directory or read your secrets.

## Minimal

The most conservative profile — **every** tool prompts first, including pure reads, search, and definition lookup. Use it when you want to approve each step explicitly, even inspection. (In ACP mode this is the tier that makes Zed prompt on reads too.)

:::tip
The permission policy is a compile-time `constexpr` matrix guarded by `static_assert`s. Changing a policy cell breaks the build, not a test nobody runs — the safety guarantee is structural.
:::
