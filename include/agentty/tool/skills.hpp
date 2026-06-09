#pragma once
// agentty::tools::skills — Agent Skills (agentskills.io) engine.
//
// Implements the open Agent Skills standard with full three-tier
// progressive disclosure:
//
//   Tier 1  Catalog: name + description of every skill ride the system
//           prompt (~50-100 tokens each). Always present, never large.
//   Tier 2  Instructions: the full SKILL.md body, loaded on demand via
//           the `skill` tool when the model decides the task matches.
//   Tier 3  Resources: bundled files (scripts/, references/, assets/)
//           enumerated in the activation result but NEVER eagerly read —
//           the model `read`s the specific file when the instructions
//           call for it. Skill directories are allowlisted for reads so
//           tier-3 fetches outside the workspace don't trip the boundary.
//
// A skill is a directory holding a `SKILL.md` with YAML frontmatter:
//
//     ---
//     name: pdf-extract
//     description: Extract text + tables from PDF files using pdfplumber.
//     compatibility: Requires Python 3.10+ and pdfplumber   # optional
//     allowed-tools: bash read                              # optional
//     disable-model-invocation: true                        # optional
//     ---
//     <full markdown body: detailed instructions, code snippets, ...>
//
// Discovery roots, in precedence order (first hit on a name wins):
//
//   project   <cwd>/.agentty/skills/<slug>/SKILL.md   (native)
//   project   <cwd>/.agents/skills/<slug>/SKILL.md    (cross-client convention)
//   project   <cwd>/.claude/skills/<slug>/SKILL.md    (Claude Code compat)
//   user      ~/.agentty/skills/<slug>/SKILL.md       (native)
//   user      ~/.agents/skills/<slug>/SKILL.md        (cross-client convention)
//   user      ~/.claude/skills/<slug>/SKILL.md        (Claude Code compat)
//
// The `.agents/skills/` convention means skills installed by any
// compliant client (Claude Code, Codex, Cursor, ...) are automatically
// visible to agentty and vice versa. Project skills shadow user skills
// with the same name (the universal convention).
//
// Parsing is deliberately LENIENT (per the spec's client-implementation
// guidance): unquoted colons in descriptions parse fine, a name that
// mismatches its directory still loads, and a missing description falls
// back to the body's first non-blank line. Only an unreadable or empty
// SKILL.md skips the entry.

#include <filesystem>
#include <string>
#include <vector>

namespace agentty::tools::skills {

struct Skill {
    std::string name;          // frontmatter `name` (or slug fallback)
    std::string description;   // frontmatter `description` (or body first line)
    std::string body;          // markdown after the frontmatter
    std::string source;        // "project" | "user" (provenance for the catalog)
    std::string compatibility; // frontmatter `compatibility` (env requirements)
    std::string allowed_tools; // frontmatter `allowed-tools` (experimental)
    bool        user_only = false; // `disable-model-invocation`: hidden from the
                                   // model-facing catalog, loadable explicitly
    std::filesystem::path dir; // absolute skill directory (tier-3 base path)
    // Bundled resource files, relative to `dir` (e.g. "scripts/run.py",
    // "references/REFERENCE.md"). Enumerated at discovery, capped at
    // kMaxResources, never eagerly read. SKILL.md itself is excluded.
    std::vector<std::string> resources;
};

// Discover + parse every skill under the project + user roots. Bounded:
// at most kMaxSkills entries, each body capped at kMaxBodyBytes. Result
// is cached process-wide keyed by the roots' AND each SKILL.md's mtime
// (an in-place edit to a skill is picked up next turn). Project skills
// shadow user skills with the same name.
[[nodiscard]] const std::vector<Skill>& all();

// Look up one skill by exact name. nullptr if absent. Finds
// `disable-model-invocation` skills too (explicit lookup is allowed;
// only the catalog hides them).
[[nodiscard]] const Skill* find(std::string_view name);

// Render the compact tier-1 catalog block for the system prompt:
//   <skills>
//   Available skills (load full instructions with the `skill` tool):
//   - name: description
//   ...
//   </skills>
// Skills flagged `disable-model-invocation` are omitted entirely (hidden
// beats listed-but-blocked: the model never wastes a turn on them).
// Empty string when no eligible skills exist (no block emitted).
[[nodiscard]] std::string catalog_block();

// Render one skill's tier-2 activation payload: the body wrapped in
// <skill_content name="...">, the absolute skill directory (so the model
// can resolve relative references), the compatibility note when present,
// and the <skill_resources> listing of bundled files. This is what the
// `skill` tool returns.
[[nodiscard]] std::string activation_payload(const Skill& s);

// ── Activation tracking (spec: deduplicate activations) ──────────────
// Once a skill body is in the conversation, re-injecting it doubles the
// token cost for zero signal. `note_activated` returns true the FIRST
// time a name is recorded (caller should return the full payload) and
// false on repeats (caller returns a short already-active sentinel).
// `reset_activations` clears the set — called on thread swap / new
// thread / session load, where the old tool_results leave context.
[[nodiscard]] bool note_activated(std::string_view name);
void reset_activations();

inline constexpr std::size_t kMaxSkills     = 64;
inline constexpr std::size_t kMaxBodyBytes  = 64 * 1024;
inline constexpr std::size_t kMaxResources  = 32;

} // namespace agentty::tools::skills
