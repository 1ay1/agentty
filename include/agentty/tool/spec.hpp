#pragma once
// agentty::tools::spec — the compile-time tool catalog.
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
#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

#include "agentty/tool/effects.hpp"
#include "agentty/domain/id.hpp"

namespace agentty::tools::spec {

enum class Kind : std::uint8_t;  // forward-declared; defined after ToolSpec

struct ToolSpec {
    std::string_view name;            // wire identifier — must be unique
    Kind             kind;            // closed-set discriminator; match `name`
    EffectSet        effects;         // capability set; drives the policy
    bool             eager_input_streaming;   // FGTS opt-in flag (Anthropic)
    // Wall-clock watchdog deadline. The reducer schedules
    // `Cmd::after(max_seconds, ToolTimeoutCheck{id})` when a tool of
    // this kind transitions to Running; the handler force-fails it if
    // it's still running when the timer fires. `0s` means "no overlay
    // timeout" — used for tools that own their own timeout via the
    // subprocess runner (bash, diagnostics) so we don't double-gate.
    //
    // Pick by the tool's longest-but-still-reasonable runtime: a slow
    // NFS read might take 20 s; a recursive grep across a Linux kernel
    // tree might take a minute. The watchdog is the safety net for
    // "the worker thread is wedged"; legitimate slow-but-progressing
    // workloads should fit comfortably under the chosen ceiling.
    //
    // Typed as `std::chrono::seconds` so callers can't accidentally
    // feed this to a millisecond-expecting scheduler without an
    // explicit `duration_cast` — the unit is in the type.
    std::chrono::seconds max_seconds;

    // Per-tool output character budget enforced at the dispatcher
    // boundary (DynamicDispatch::execute, tool.hpp). 0 means "no
    // cap" — the tool already produces bounded output by design
    // (todo, git_commit). Anything else is a hard ceiling: text
    // longer than this is truncated according to `trunc_strategy`
    // with a one-line "[... N chars elided ...]" marker so the
    // model knows the response was clipped and can request
    // specifics. Inspired by Claude Code's per-tool output caps —
    // agentty's individual tools already chunk output (Read uses
    // offset/limit, Bash uses tail-only, Grep caps matches), but
    // this is the safety net that covers tools whose internal
    // limiting fails on pathological input (a 10 MB single-line
    // log file from `bash`, a Read on a file with 2000 long
    // lines, a Grep on a runaway pattern). The cap on tools that
    // already self-limit is generous so it almost never trips on
    // healthy inputs.
    //
    // Picked by character count, not tokens, because agentty doesn't
    // run a tokenizer locally. ~4 chars/token rule of thumb so a
    // 30k-char cap ≈ 7.5k tokens.
    int max_output_chars;

    // Strategy used when output exceeds max_output_chars. Each
    // strategy preserves the part of the output the tool's
    // typical caller cares about most:
    //
    //   Head     — keep the first N chars, drop the tail. Right
    //              for Read / Edit / Write where the answer is
    //              "the requested chunk, in order".
    //   Tail     — keep the last N chars, drop the head. Right
    //              for Bash / Diagnostics where the latest log
    //              line / error message matters more than the
    //              setup output.
    //   HeadTail — keep the first ~70% + last ~30%, elide middle.
    //              Right for Grep / Web / Git where both the
    //              start (most-relevant matches, page header,
    //              first commits) AND the end (summary / "N more
    //              matches" hint, page footer, recent commits)
    //              carry signal.
    enum class TruncStrategy : std::uint8_t { Head, Tail, HeadTail };
    TruncStrategy trunc_strategy;
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
// Short-hand so the catalog reads as `20s` rather than
// `std::chrono::seconds{20}`. Scoped to the catalog array so the UDL
// doesn't leak to headers that include this one.
namespace detail {
using sec = std::chrono::seconds;
}

// ── Closed-set identity for every tool in the catalog ───────────────────
// A typed discriminator so call sites stop hand-rolling `name == "bash"`
// string compares. Add a tool: add a Kind arm, add a row to the catalog
// with the matching `kind`, and `kind_of("new_tool") == Kind::NewTool`
// at compile time — the `static_assert` wall below proves every Kind has
// exactly one catalog entry and every catalog entry has a Kind.
//
// Wire identity stays the `name` string field (Anthropic's vocabulary is
// the source of truth on the wire); Kind is the internal closed set the
// reducer/views dispatch on.
enum class Kind : std::uint8_t {
    Read,
    Edit,
    Write,
    Bash,
    Grep,
    Glob,
    ListDir,
    Todo,
    WebFetch,
    WebSearch,
    FindDefinition,
    Diagnostics,
    GitStatus,
    GitDiff,
    GitLog,
    GitCommit,
    Remember,
    Forget,
    Wipe,
    Task,
    Skill,
    SearchDocs,
    SearchCode,
    RepoMap,
};

inline constexpr std::array kCatalog = {
    //         name              kind                  effects                              eager   timeout            chars   strategy
    ToolSpec{"read",            Kind::Read,           {Effect::ReadFs},                     false,   detail::sec{20},   80000,  ToolSpec::TruncStrategy::Head},
    ToolSpec{"edit",            Kind::Edit,           {Effect::ReadFs, Effect::WriteFs},    true,    detail::sec{20},   40000,  ToolSpec::TruncStrategy::Head},
    ToolSpec{"write",           Kind::Write,          {Effect::WriteFs},                    true,    detail::sec{20},   40000,  ToolSpec::TruncStrategy::Head},
    ToolSpec{"bash",            Kind::Bash,           {Effect::Exec},                       true,    detail::sec{0},    30000,  ToolSpec::TruncStrategy::Tail},   // subprocess-managed; tail-only matches log-stream usage
    ToolSpec{"grep",            Kind::Grep,           {Effect::ReadFs},                     false,   detail::sec{45},   30000,  ToolSpec::TruncStrategy::HeadTail},
    ToolSpec{"glob",            Kind::Glob,           {Effect::ReadFs},                     false,   detail::sec{30},   25000,  ToolSpec::TruncStrategy::Head},
    ToolSpec{"list_dir",        Kind::ListDir,        {Effect::ReadFs},                     false,   detail::sec{20},   25000,  ToolSpec::TruncStrategy::Head},
    ToolSpec{"todo",            Kind::Todo,           {} /* pure */,                        true,    detail::sec{5},    0,      ToolSpec::TruncStrategy::Head},   // in-memory; never large
    ToolSpec{"web_fetch",       Kind::WebFetch,       {Effect::Net},                        false,   detail::sec{30},   30000,  ToolSpec::TruncStrategy::Head},
    ToolSpec{"web_search",      Kind::WebSearch,      {Effect::Net},                        false,   detail::sec{20},   25000,  ToolSpec::TruncStrategy::HeadTail},
    ToolSpec{"find_definition", Kind::FindDefinition, {Effect::ReadFs},                     false,   detail::sec{30},   25000,  ToolSpec::TruncStrategy::HeadTail},
    ToolSpec{"diagnostics",     Kind::Diagnostics,    {Effect::Exec},                       false,   detail::sec{0},    30000,  ToolSpec::TruncStrategy::Tail},   // subprocess-managed; tail-only
    ToolSpec{"git_status",      Kind::GitStatus,      {Effect::ReadFs},                     false,   detail::sec{20},   30000,  ToolSpec::TruncStrategy::HeadTail},
    ToolSpec{"git_diff",        Kind::GitDiff,        {Effect::ReadFs},                     false,   detail::sec{20},   60000,  ToolSpec::TruncStrategy::HeadTail},  // diffs can be big; bigger budget
    ToolSpec{"git_log",         Kind::GitLog,         {Effect::ReadFs},                     false,   detail::sec{20},   30000,  ToolSpec::TruncStrategy::HeadTail},
    ToolSpec{"git_commit",      Kind::GitCommit,      {Effect::WriteFs},                    true,    detail::sec{30},   0,      ToolSpec::TruncStrategy::Head},   // pre-commit hooks can be slow; output stays small
    // Memory tools — append/remove records in ~/.agentty/memory.jsonl (user)
    // or <workspace>/.agentty/memory.jsonl (project). Loaded back into the
    // system prompt under <learned-memory> on every turn. Tiny IO footprint,
    // bounded by the file caps in src/tool/memory_store.cpp; output is one
    // confirmation line, never large.
    ToolSpec{"remember",        Kind::Remember,       {Effect::WriteFs},                    false,   detail::sec{5},    2000,   ToolSpec::TruncStrategy::Head},
    ToolSpec{"forget",          Kind::Forget,         {Effect::WriteFs},                    false,   detail::sec{5},    2000,   ToolSpec::TruncStrategy::Head},
    ToolSpec{"wipe_memory",     Kind::Wipe,           {Effect::WriteFs},                    false,   detail::sec{5},    2000,   ToolSpec::TruncStrategy::Head},
    // Subagent dispatch. Spawns an isolated agent loop (own context,
    // own tool budget, depth-capped) that runs to completion and
    // returns ONE condensed result. Carries Effect::Exec: a subagent
    // can run bash/write/etc., so it is at least as powerful as bash
    // and must gate identically. Owns its own wall-clock budget inside
    // the loop (bounded turn count) like the subprocess tools, so the
    // overlay watchdog is 0. HeadTail: the subagent's final summary
    // carries signal at both ends.
    ToolSpec{"task",            Kind::Task,           {Effect::Exec},                       false,   detail::sec{0},    40000,  ToolSpec::TruncStrategy::HeadTail},
    // Skill loader — reads one on-demand SKILL.md body from disk
    // (agentskills.io roots: .agentty/.agents/.claude × project/user).
    // ReadFs; bodies are capped at 64 KiB by the skills module, so the
    // 64000 char budget is generous headroom. Head: a skill doc reads
    // top-down.
    ToolSpec{"skill",           Kind::Skill,          {Effect::ReadFs},                     false,   detail::sec{10},   64000,  ToolSpec::TruncStrategy::Head},
    // search_docs — agentic document/knowledge RAG. Reads the corpus
    // files (ReadFs) and queries the local Ollama embed endpoint (Net)
    // for hybrid BM25+dense retrieval. HeadTail keeps the top hits AND
    // the trailing lower-ranked block; 60s covers a first-call index
    // build + batch embed of a modest corpus.
    ToolSpec{"search_docs",     Kind::SearchDocs,     {Effect::ReadFs, Effect::Net},        false,   detail::sec{60},   30000,  ToolSpec::TruncStrategy::HeadTail},
    // search_code — SEMANTIC code retrieval (hybrid complement to grep):
    // BM25 + optional dense embeddings over source chunks, for conceptual
    // queries that share no token with the code. Reads the tree (ReadFs) and
    // queries the local Ollama embed endpoint (Net). First call walks +
    // indexes the tree, so the 60s budget mirrors search_docs.
    ToolSpec{"search_code",     Kind::SearchCode,     {Effect::ReadFs, Effect::Net},        false,   detail::sec{60},   30000,  ToolSpec::TruncStrategy::HeadTail},
    // repo_map — aider-style token-budgeted PageRank skeleton of the
    // codebase (def/ref graph, signature lines). The output is already
    // budget-packed by the tool itself (≤60 KB by its own budget arg),
    // so the dispatcher cap is a safety net. Head: the map is ranked
    // best-first, so truncation from the tail loses only low-rank files.
    ToolSpec{"repo_map",        Kind::RepoMap,        {Effect::ReadFs},                     false,   detail::sec{30},   60000,  ToolSpec::TruncStrategy::Head},
};

// Wire-string → Kind. `std::nullopt` for names not in the catalog so the
// caller (reducer guards, runtime dispatch) can react to an unknown tool
// explicitly instead of silently falling through a string compare chain.
[[nodiscard]] constexpr std::optional<Kind> kind_of(std::string_view name) noexcept {
    for (const auto& s : kCatalog) if (s.name == name) return s.kind;
    return std::nullopt;
}

// Kind → wire-string. Total: every Kind has an entry by the static_assert
// wall below, so the fallback is unreachable in well-formed builds.
[[nodiscard]] constexpr std::string_view name_of(Kind k) noexcept {
    for (const auto& s : kCatalog) if (s.kind == k) return s.name;
    return {};
}

// Compile-time lookup. Returns a pointer to the spec, or nullptr if
// the name doesn't exist. Used by the runtime factories to populate
// `ToolDef::name` / `description` / `effects` from the table.
[[nodiscard]] constexpr const ToolSpec* lookup(std::string_view name) noexcept {
    for (const auto& s : kCatalog) if (s.name == name) return &s;
    return nullptr;
}

// ── Scheduling-effects view ──────────────────────────────────────
// `effects` answers the PERMISSION question ("how much trust does this
// tool need?"); this answers the CONCURRENCY question ("what does it
// actually contend on?"). They coincide for every tool except `task`:
//
//   • task carries Effect::Exec for permission — a subagent can run bash,
//     so it must gate like bash.
//   • But for SCHEDULING, Exec means "exclusive: unbounded blast radius,
//     nothing else may run" — which serialised every task and blocked all
//     sibling tools behind a running subagent. That defeats the tool's
//     entire purpose: task exists precisely so the model can fan out
//     several isolated investigations CONCURRENTLY (Claude Code runs its
//     Task tool in parallel waves for the same reason). The subagent's
//     own tool calls are individually effect-gated + path-scheduled
//     inside its loop, so the parent scheduling it as coarse-Exec is
//     double-counting contention that the inner dispatch already manages.
//
// So task schedules as {ReadFs, Net} — composable with reads, with other
// tasks, and with disjoint-path writers — while its permission gate stays
// Exec. Any future tool with the same split gets a row here.
[[nodiscard]] constexpr EffectSet sched_effects(const ToolSpec& s) noexcept {
    if (s.name == "task") return {Effect::ReadFs, Effect::Net};
    return s.effects;
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
                  "tool name not in agentty::tools::spec::kCatalog — add an "
                  "entry there before calling spec::require<...>");
    return *s;
}

// ── Compile-time correctness proofs of the catalog ───────────────────────
// These are the safety net: every property a reader might assume about
// a tool's capabilities is verified at build time. Anyone editing the
// catalog above gets an instant signal if they break an invariant.
namespace proofs {

// NOTE on sanitizer builds: GCC's ASan/UBSan instrument the module-level
// `constexpr kCatalog` global, and that instrumentation leaks into the
// `consteval` evaluation of the proofs below ("not a constant expression").
// The proofs are valid — they compile and pass in every NORMAL build, which is
// the real gate. Under AGENTTY_SANITIZER_BUILD we skip re-evaluating them so
// the sanitizer can do its actual job (RUNTIME memory-safety checking). See
// the CMake AGENTTY_SANITIZE_ALL block for the full rationale.
#ifndef AGENTTY_SANITIZER_BUILD

// Every name in the catalog is unique.
consteval bool all_names_unique() {
    for (std::size_t i = 0; i < kCatalog.size(); ++i)
        for (std::size_t j = i + 1; j < kCatalog.size(); ++j)
            if (kCatalog[i].name == kCatalog[j].name) return false;
    return true;
}
static_assert(all_names_unique(), "tool catalog has duplicate names");

// Every Kind arm appears exactly once in the catalog. Pair with the name
// uniqueness check: together they prove `kind_of(name)` is injective and
// `name_of(kind)` is total.
consteval bool kinds_bijective() {
    constexpr Kind kAll[] = {
        Kind::Read, Kind::Edit, Kind::Write, Kind::Bash,
        Kind::Grep, Kind::Glob, Kind::ListDir, Kind::Todo,
        Kind::WebFetch, Kind::WebSearch, Kind::FindDefinition,
        Kind::Diagnostics, Kind::GitStatus, Kind::GitDiff,
        Kind::GitLog, Kind::GitCommit,
        Kind::Remember, Kind::Forget,
        Kind::Wipe,
        Kind::Task,
        Kind::Skill,
        Kind::SearchDocs,
        Kind::SearchCode,
        Kind::RepoMap,
    };
    if (std::size(kAll) != kCatalog.size()) return false;
    for (auto k : kAll) {
        int hits = 0;
        for (const auto& s : kCatalog) if (s.kind == k) ++hits;
        if (hits != 1) return false;
    }
    // And for every row the reverse holds (name↔kind round-trip).
    for (const auto& s : kCatalog) {
        auto k = kind_of(s.name);
        if (!k || *k != s.kind) return false;
        if (name_of(s.kind) != s.name) return false;
    }
    return true;
}
static_assert(kinds_bijective(),
              "spec::Kind and kCatalog must be in bijection — every Kind arm "
              "needs exactly one row whose `kind` matches and whose `name` "
              "round-trips through kind_of/name_of");

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
        if (s.name != "bash" && s.name != "diagnostics" && s.name != "task")
            return false;
    }
    return true;
}
static_assert(only_known_exec_tools(),
              "Only `bash`, `diagnostics`, and `task` may carry Effect::Exec — "
              "adding another Exec tool requires a separate review and updating "
              "this static_assert");

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
        "read","grep","glob","list_dir","find_definition","repo_map",
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
        if (s.name != "web_fetch" && s.name != "web_search" &&
            s.name != "search_docs" && s.name != "search_code") return false;
    }
    return true;
}
static_assert(only_web_is_net(),
              "Only web_fetch/web_search/search_docs/search_code may carry Effect::Net");

// ── Per-tool timeout proofs ─────────────────────────────────────────────
// Pin the wall-clock-watchdog values so a careless edit (set 0 on the
// wrong tool, or 6000 on a fast one) breaks the build instead of
// silently changing runtime behaviour.

// Subprocess-managed tools (bash, diagnostics) have their own
// timeout in `subprocess.cpp`; the overlay watchdog must be 0 to
// avoid double-gating.
consteval bool subprocess_tools_have_no_overlay_timeout() {
    auto* b = lookup("bash");
    auto* d = lookup("diagnostics");
    return b && d && b->max_seconds == std::chrono::seconds{0}
                  && d->max_seconds == std::chrono::seconds{0};
}
static_assert(subprocess_tools_have_no_overlay_timeout(),
              "bash/diagnostics must have max_seconds=0 — they own their timeout");

// Every other tool MUST have a finite, sensible overlay timeout. Zero
// would mean "never time out", and that's the bug the watchdog exists
// to prevent. Cap at 5 minutes so no tool can wedge the agent for
// longer than the user's patience.
consteval bool other_tools_have_bounded_timeout() {
    using std::chrono::seconds;
    for (const auto& s : kCatalog) {
        // bash/diagnostics own their subprocess timeout; `task` owns its
        // budget via a bounded sub-agent turn count (no single syscall to
        // watchdog). All three set max_seconds=0 deliberately.
        if (s.name == "bash" || s.name == "diagnostics" || s.name == "task")
            continue;
        if (s.max_seconds < seconds{1} || s.max_seconds > seconds{300}) return false;
    }
    return true;
}
static_assert(other_tools_have_bounded_timeout(),
              "non-subprocess tools (except `task`) need a max_seconds in [1, 300]");

// `web_fetch` cannot wait longer than the underlying http total
// timeout, otherwise the watchdog fires while the client is still
// happily blocking on a slow server.
static_assert(lookup("web_fetch")->max_seconds <= std::chrono::seconds{30},
              "web_fetch overlay timeout must be ≤ http total (30s)");

// ── Truncation strategy correlates with the tool's effect shape ─────────
// Each TruncStrategy preserves a different slice of the output, and that
// choice should follow from what the tool DOES — not be set per-row from
// taste. Pinning the correlation as an invariant catches the "new Exec
// tool defaulted to Head and now `bash` output is being head-clipped
// instead of tail-clipped" class of bug at the build, not in a user
// report.
//
//   Effect::Exec        → Tail   (log streams: latest line wins)
//   Effect::Net         → Head or HeadTail (page body OR search results)
//   Effect::WriteFs only → Head   (echoes the chunk it wrote, in order)
//   Pure                → Head   (in-memory state has no "tail" semantics)
//   ReadFs              → Head or HeadTail  (depends on tool; see below)
//
// ReadFs has two legitimate shapes: linear file readers (read, list_dir,
// glob) prefer Head; search-like tools (grep, find_definition, git_*)
// prefer HeadTail to keep the "N more matches" / summary tail.
consteval bool truncation_matches_effect_shape() {
    for (const auto& s : kCatalog) {
        // 0 means no cap — strategy doesn't apply, skip.
        if (s.max_output_chars == 0) continue;

        if (s.effects.has(Effect::Exec)) {
            // `task` is Exec-class but its output is a structured subagent
            // summary, not a log stream — HeadTail keeps both the
            // opening framing and the final result. bash/diagnostics
            // stay Tail (latest log line wins).
            if (s.name == "task") {
                if (s.trunc_strategy == ToolSpec::TruncStrategy::Tail) return false;
                continue;
            }
            if (s.trunc_strategy != ToolSpec::TruncStrategy::Tail) return false;
            continue;
        }
        if (s.effects.has(Effect::Net)) {
            // Net tools split by use case: fetch-a-page wants Head (the
            // lede is the lead), search-results wants HeadTail ("N more
            // results" footer carries signal). Tail makes no sense —
            // network responses aren't log streams.
            if (s.trunc_strategy == ToolSpec::TruncStrategy::Tail) return false;
            continue;
        }
        if (s.effects.has(Effect::WriteFs) && !s.effects.has(Effect::ReadFs)) {
            if (s.trunc_strategy != ToolSpec::TruncStrategy::Head) return false;
            continue;
        }
        // Remaining: pure or read-only-ish tools — both Head and HeadTail
        // are defensible; just rule out Tail (no tool here is a log
        // stream).
        if (s.trunc_strategy == ToolSpec::TruncStrategy::Tail) return false;
    }
    return true;
}
static_assert(truncation_matches_effect_shape(),
              "a tool's trunc_strategy doesn't match its effect shape — see "
              "truncation_matches_effect_shape for the rules");

// Every tool with `max_output_chars > 0` must have a sane minimum so the
// elision marker (one line, ~30 chars) still leaves room for content.
consteval bool output_caps_are_sane() {
    for (const auto& s : kCatalog) {
        if (s.max_output_chars == 0) continue;
        if (s.max_output_chars < 1000)       return false;  // floor: leaves room for marker
        if (s.max_output_chars > 200'000)    return false;  // ceiling: ~50k tokens
    }
    return true;
}
static_assert(output_caps_are_sane(),
              "a tool's max_output_chars is outside the sane [1000, 200000] band");

// FGTS (eager_input_streaming) is only meaningful when the tool produces
// its arguments mid-stream — i.e. for tools whose JSON arg blob is large
// enough that streaming the open brace early matters. We don't enforce a
// strict whitelist here (the policy may change as Anthropic rolls models
// forward), but we DO require that tools tagged eager have at least one
// effect — a pure no-op tool gaining eager streaming is almost certainly
// a copy-paste mistake from a heavier neighbour.
//
// Exception: `todo` is Pure and intentionally eager because its arg blob
// (a list of items) is the only thing the model emits and the early-open
// behaviour lets the UI render the in-progress list while the model is
// still typing it.
consteval bool eager_streaming_is_justified() {
    for (const auto& s : kCatalog) {
        if (!s.eager_input_streaming) continue;
        if (s.name == "todo") continue;          // documented exception
        if (s.effects.empty()) return false;     // pure + eager is suspicious
    }
    return true;
}
static_assert(eager_streaming_is_justified(),
              "a pure (effect-less) tool other than `todo` opted into "
              "eager_input_streaming — review the rationale");

// ── Scheduling-effects divergence proof ─────────────────────────────────
// `sched_effects` answers the CONCURRENCY question and is allowed to
// differ from `effects` (the PERMISSION question) for exactly one tool:
// `task`. The divergence is load-bearing — it's what lets subagents fan
// out concurrently instead of serialising behind a coarse Exec lock — but
// until now nothing pinned it. A rename of `task`, a change to its
// catalog effects, or a stray edit to `sched_effects` could silently
// collapse the divergence (re-serialising every subagent) or spread it to
// another tool (letting an Exec tool run unscheduled). These proofs make
// any such drift a build error at the point it's introduced.

// For every tool EXCEPT `task`, scheduling effects == permission effects.
// No tool may quietly acquire a scheduling identity that differs from the
// capabilities the policy gates on.
consteval bool sched_effects_match_except_task() {
    for (const auto& s : kCatalog) {
        if (s.name == "task") continue;   // the one sanctioned divergence
        if (sched_effects(s) != s.effects) return false;
    }
    return true;
}
static_assert(sched_effects_match_except_task(),
              "a tool other than `task` has sched_effects != effects — "
              "scheduling identity must match the permission capability set "
              "for every tool but the sanctioned `task` divergence");

// `task`'s divergence is EXACT, not incidental: it gates as Exec (a
// subagent can run bash, so permission must be strict) but SCHEDULES as
// {ReadFs, Net} (composable with reads, other tasks, and disjoint-path
// writers). Pin both halves so neither can rot independently.
consteval bool task_divergence_is_exact() {
    auto* t = lookup("task");
    if (!t) return false;
    // Permission side: strict Exec, nothing weaker.
    if (!t->effects.has(Effect::Exec)) return false;
    // Scheduling side: exactly {ReadFs, Net} — Exec DROPPED so subagents
    // don't serialise behind a coarse exclusive lock.
    auto sched = sched_effects(*t);
    if (sched.has(Effect::Exec))     return false;
    if (!sched.has(Effect::ReadFs))  return false;
    if (!sched.has(Effect::Net))     return false;
    if (sched.has(Effect::WriteFs))  return false;
    return true;
}
static_assert(task_divergence_is_exact(),
              "`task` must gate as Exec but schedule as exactly {ReadFs, Net} — "
              "the divergence that lets subagents run concurrently");

#endif  // AGENTTY_SANITIZER_BUILD

} // namespace proofs

} // namespace agentty::tools::spec
