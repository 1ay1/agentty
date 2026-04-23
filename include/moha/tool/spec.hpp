#pragma once
// moha::tools::spec — the compile-time tool catalog.
//
// Each tool factory in `src/tool/tools/*.cpp` populates a `ToolDef` at
// runtime. The shape of *every* tool — its name, its capabilities, its
// streaming behavior — is also fixed at compile time, and lives here as
// a `constexpr std::array` of `ToolSpec`s.
//
// Three reasons this exists as a separate compile-time table:
//
// 1. Static cross-checks. The block of `static_assert`s at the bottom
//    proves properties about the catalog that no runtime test can
//    guarantee — "every WriteFs tool actually has WriteFs", "no
//    read-only tool accidentally got the Exec capability", "the bash
//    tool's name is `bash`, not `Bash`". These are caught at the build
//    where they originate, not at run time.
//
// 2. Single source of truth for catalog metadata. Factories in
//    `src/tool/tools/*` reference `tools::spec::lookup("bash")` for
//    their name / description / effects / eager-streaming flag, so a
//    typo there is impossible — there's only one place to write the
//    string `"bash"`, and the lookup either returns the spec or the
//    factory fails to compile.
//
// 3. Wire-format generators (e.g. the JSON tool list sent to Anthropic)
//    can iterate over a `constexpr` table without paying a runtime
//    init cost. Today the wire path still uses the runtime `registry()`
//    vector; the spec table lets us migrate that progressively.

#include <array>
#include <string_view>

#include "moha/tool/effects.hpp"

namespace moha::tools::spec {

struct ToolSpec {
    std::string_view name;            // wire identifier — must be unique
    EffectSet        effects;         // capability set; drives the policy
    bool             eager_input_streaming;   // FGTS opt-in flag (Anthropic)
};

// ── The full tool catalog, in the same display order as the runtime
// registry (`src/tool/registry.cpp:build_registry`). Order matters: the
// model has a recall bias toward earlier entries, so `edit` precedes
// `write` to nudge against full-file rewrites.
//
// Description text is intentionally NOT in the catalog — it's wire-only
// metadata (each tool's factory composes its own help text, sometimes
// platform-conditional like bash on Windows). Cross-validating
// descriptions buys nothing; cross-validating effects + names matters.
inline constexpr std::array kCatalog = {
    ToolSpec{"read",            {Effect::ReadFs},                     false},
    ToolSpec{"edit",            {Effect::ReadFs, Effect::WriteFs},    true},
    ToolSpec{"write",           {Effect::WriteFs},                    true},
    ToolSpec{"bash",            {Effect::Exec},                       true},
    ToolSpec{"grep",            {Effect::ReadFs},                     false},
    ToolSpec{"glob",            {Effect::ReadFs},                     false},
    ToolSpec{"list_dir",        {Effect::ReadFs},                     false},
    ToolSpec{"todo",            {} /* pure */,                        true},
    ToolSpec{"web_fetch",       {Effect::Net},                        false},
    ToolSpec{"web_search",      {Effect::Net},                        false},
    ToolSpec{"find_definition", {Effect::ReadFs},                     false},
    ToolSpec{"diagnostics",     {Effect::Exec},                       false},
    ToolSpec{"git_status",      {Effect::ReadFs},                     false},
    ToolSpec{"git_diff",        {Effect::ReadFs},                     false},
    ToolSpec{"git_log",         {Effect::ReadFs},                     false},
    ToolSpec{"git_commit",      {Effect::WriteFs},                    true},
};

// Compile-time lookup. Returns a pointer to the spec, or nullptr if
// the name doesn't exist. Used by the runtime factories to populate
// `ToolDef::name` / `description` / `effects` from the table.
[[nodiscard]] constexpr const ToolSpec* lookup(std::string_view name) noexcept {
    for (const auto& s : kCatalog) if (s.name == name) return &s;
    return nullptr;
}

// Fixed-string non-type template parameter so a tool factory can write
// `spec::require<"bash">()` and have the misspelling caught at compile
// time. The instantiation site evaluates `lookup` in a constant
// expression and `static_assert`s on the result.
template <std::size_t N>
struct FixedName {
    char data[N];
    consteval FixedName(const char (&s)[N]) {
        for (std::size_t i = 0; i < N; ++i) data[i] = s[i];
    }
    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return {data, N - 1};   // strip trailing NUL
    }
};

// `spec::require<"bash">()` returns the catalog entry for "bash".
// Compile error if no entry with that name exists — there is no way
// to silently create a tool whose name isn't in the catalog.
template <FixedName Name>
[[nodiscard]] consteval const ToolSpec& require() {
    constexpr std::string_view name_v = Name.view();
    constexpr const ToolSpec* s = lookup(name_v);
    static_assert(s != nullptr,
                  "tool name not in moha::tools::spec::kCatalog — add an "
                  "entry there before calling spec::require<...>");
    return *s;
}

// ── Compile-time correctness proofs of the catalog ───────────────────────
// These are the safety net: every property a reader might assume about
// a tool's capabilities is verified at build time. Anyone editing the
// catalog above gets an instant signal if they break an invariant.
namespace proofs {

// Every name in the catalog is unique.
consteval bool all_names_unique() {
    for (std::size_t i = 0; i < kCatalog.size(); ++i)
        for (std::size_t j = i + 1; j < kCatalog.size(); ++j)
            if (kCatalog[i].name == kCatalog[j].name) return false;
    return true;
}
static_assert(all_names_unique(), "tool catalog has duplicate names");

// Every tool has a non-empty name (the wire requires it).
consteval bool all_names_present() {
    for (const auto& s : kCatalog) if (s.name.empty()) return false;
    return true;
}
static_assert(all_names_present(), "every tool needs a non-empty name");

// Lookup works for every catalog entry.
static_assert(lookup("bash")     != nullptr);
static_assert(lookup("git_commit") != nullptr);
static_assert(lookup("nonexistent") == nullptr);

// Capability invariants — the rules we want to never violate.

// `bash` and `diagnostics` are the only Exec tools; nothing else gets
// arbitrary code execution.
consteval bool only_known_exec_tools() {
    for (const auto& s : kCatalog) {
        if (!s.effects.has(Effect::Exec)) continue;
        if (s.name != "bash" && s.name != "diagnostics") return false;
    }
    return true;
}
static_assert(only_known_exec_tools(),
              "Only `bash` and `diagnostics` may carry Effect::Exec — adding "
              "another Exec tool requires a separate review and updating this "
              "static_assert");

// Tools that mutate the filesystem must NOT also be Exec — those are
// strictly more dangerous and would belong in the bash family if they
// needed both. (Keeps the policy table clean.)
consteval bool no_writefs_and_exec_combo() {
    for (const auto& s : kCatalog)
        if (s.effects.has(Effect::WriteFs) && s.effects.has(Effect::Exec))
            return false;
    return true;
}
static_assert(no_writefs_and_exec_combo(),
              "no tool may carry both WriteFs and Exec — promote to bash");

// Pure tools must have empty effects.
static_assert(lookup("todo")->effects.empty());

// Read-side tools must NOT have WriteFs / Net / Exec.
consteval bool readonly_invariants() {
    constexpr std::string_view kReadOnly[] = {
        "read","grep","glob","list_dir","find_definition",
        "git_status","git_diff","git_log",
    };
    for (auto n : kReadOnly) {
        auto* s = lookup(n);
        if (!s) return false;
        if (s->effects.has(Effect::WriteFs)) return false;
        if (s->effects.has(Effect::Exec))    return false;
        if (s->effects.has(Effect::Net))     return false;
    }
    return true;
}
static_assert(readonly_invariants(),
              "a tool listed as read-only carries a write/exec/net capability");

// Network tools must be exactly the web ones.
consteval bool only_web_is_net() {
    for (const auto& s : kCatalog) {
        if (!s.effects.has(Effect::Net)) continue;
        if (s.name != "web_fetch" && s.name != "web_search") return false;
    }
    return true;
}
static_assert(only_web_is_net(),
              "Only web_fetch/web_search may carry Effect::Net");

} // namespace proofs

} // namespace moha::tools::spec
