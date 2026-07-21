---
title: Permission Profiles
description: Ask, Write, and Minimal — how agentty gates writes, shell, and network.
nav_section: User Manual
nav_order: 30
slug: profiles
---

A profile decides which tool effects run automatically and which prompt you first. New installs start in **Write** (the [ACP bridge](/docs/acp) starts in **Ask**); cycle anytime with [[S-Tab]], and your choice persists across sessions.

| Profile | Pure reads | Writes / edits | Shell / build | Network |
|---|---|---|---|---|
| **Write** (default) | auto | auto | auto | auto |
| **Ask** | auto | prompt | prompt | prompt |
| **Minimal** | prompt | prompt | prompt | prompt |

## Write (autonomous, default)

The default tier. Every tool runs without prompting so agentty can move fast — and because each shell call is sandboxed (`bwrap` / `sandbox-exec`) and the file tools refuse paths outside your workspace, even an autonomous run can't escape your project directory or read your secrets.

## Ask

Read-only tools still run automatically, but writes, shell calls, and network calls each prompt before running. Cycle here with [[S-Tab]] when you want to eyeball each change before it lands — handy in an unfamiliar repo.

## Minimal

The most conservative profile — **every** tool prompts first, including pure reads, search, and definition lookup. Use it when you want to approve each step explicitly, even inspection. (In ACP mode this is the tier that makes Zed prompt on reads too.)

:::tip
The permission policy is a compile-time `constexpr` matrix guarded by `static_assert`s. Changing a policy cell breaks the build, not a test nobody runs — the safety guarantee is structural.
:::
