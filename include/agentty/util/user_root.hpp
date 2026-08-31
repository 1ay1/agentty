#pragma once
// agentty::util::user_root — THE single per-user storage root: ~/.agentty
//
// ─────────────────────────────────────────────────────────────────────────
// Why ONE root, and why it is NOT $XDG_CONFIG_HOME/agentty
// ─────────────────────────────────────────────────────────────────────────
// agentty's per-user footprint is config + data + secrets + cache + logs,
// tightly coupled: settings reference threads, memory feeds prompts,
// credentials gate everything. The XDG base-dir spec would scatter that
// across FOUR roots (config/data/state/cache) — and history shows what a
// multi-root layout does to this codebase: files landed by coin-flip
// (modelsdev.json — a cache — in .config; stderr.log in .config while
// agentty.log sat in ~/.agentty; a comment in main.cpp that said
// `~/.agentty` beside code that wrote config_dir()). A layout without one
// obvious answer for "where does a new file go" guarantees drift.
//
// Putting everything under $XDG_CONFIG_HOME/agentty instead would be
// worse, not better:
//   • ~/.config is what people BACK UP and SYNC as dotfiles. agentty's
//     dir holds OAuth refresh tokens and full conversation history —
//     silently syncing secrets + hundreds of MB of threads into a
//     dotfiles repo is a real harm, not a style nit.
//   • The spec itself says .config is for CONFIG. Threads are data,
//     tokens are secrets, modelsdev.json is a cache, logs are state.
//     "Technically they're all conf files" is not true, and honoring the
//     spec would mean the four-way scatter, not .config-for-everything.
//
// So agentty follows the convention of its peers (Claude Code, Ghostty's
// macOS layout, gh, aws, gcloud, cargo, rustup…): ONE app dotdir, owner-
// only, with an internal layout that tells every future file exactly
// where it belongs:
//
//   ~/.agentty/
//     settings.json, mcp.json, hooks.json, skills/, agents/, commands/
//                      ← user-facing config: top level, discoverable,
//                        the part a user might hand-edit or diff
//     threads/  memory.jsonl
//                      ← data: conversation history, learned memory
//     credentials/     ← secrets (0700 dir, 0600 files): credentials.json,
//                        accounts.json, provider OAuth files + locks
//     cache/           ← refetchable: modelsdev.json, update_check.json
//     logs/            ← agentty.log, stderr.log
//
// One `rm -rf ~/.agentty` resets the app. One `chmod 0700` is the whole
// security story. One directory to document. The layout matches the
// PROJECT-side .agentty (scope.hpp's dialect ladder) so "the .agentty
// dir" means the same thing at both loci.
//
// $AGENTTY_HOME overrides the root wholesale (tests, containers, multi-
// profile setups). $XDG_* is deliberately NOT consulted — a fixed name
// keeps `docs`, error messages, and support answers unconditional.
//
// Migration: the pre-consolidation layout kept credentials & caches in
// $XDG_CONFIG_HOME/agentty (~/.config/agentty). user_root() migrates
// those files into place ONCE (copy+verify, then best-effort remove) the
// first time it runs against an old install; see migrate_legacy_config()
// in user_root.cpp.

#include <filesystem>

namespace agentty::util {

// The per-user root (~/.agentty or $AGENTTY_HOME). Created 0700 on first
// use; migration from the legacy ~/.config/agentty split-root layout runs
// once per process (idempotent, best-effort, never throws).
[[nodiscard]] std::filesystem::path user_root();

// Subdirectory accessors — each creates its dir on first use. Use these
// instead of joining literals so the layout stays enforced in one place.
[[nodiscard]] std::filesystem::path user_credentials_dir();  // secrets, 0700
[[nodiscard]] std::filesystem::path user_cache_dir();        // refetchable
[[nodiscard]] std::filesystem::path user_logs_dir();         // diagnostics

}  // namespace agentty::util
