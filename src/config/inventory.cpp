// config/inventory.cpp — resolve and print the inventory.
//
// Everything here folds the SAME functions the real resolvers fold:
// scope::plan for the read ladder, dirs::resolve_dry for the write path.
// Nothing re-derives a location, which is what makes the output trustworthy
// rather than a second opinion. See inventory.hpp for the argument.

#include "agentty/config/inventory.hpp"

#include "agentty/util/home_dir.hpp"
#include "agentty/util/logx.hpp"
#if AGENTTY_MCP
#include "agentty/mcp/import.hpp"   // foreign configs, named with their fix
#endif

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <variant>

namespace agentty::config {

namespace fs = std::filesystem;

namespace {

// The inventory. Each row points AT the constants in the header; it never
// copies one, so there is exactly one definition per concern.
constexpr std::array<Entry, 10> kEntries{{
    {"mcp",      "MCP servers",          &kMcpLayout,      nullptr},
    {"skills",   "skills",               &kSkillsLayout,   nullptr},
    {"agents",   "subagent definitions", &kAgentsLayout,   nullptr},
    {"commands", "slash commands",       &kCommandsLayout, nullptr},
    {"memory",   "learned memory",       &kMemoryLayout,   nullptr},
    {"threads",  "conversation history", nullptr,          &kThreadsSpec},
    {"cache",    "refetchable caches",   nullptr,          &kCacheSpec},
    {"logs",     "diagnostics",          nullptr,          &kLogsSpec},
    {"rag",      "retrieval indexes",    nullptr,          &kRagSpec},
    {"feedback", "retrieval learning",   nullptr,          &kFeedbackSpec},
}};

// The full dialect product, for computing what a feature SKIPS. The only
// place the complete set is spelled; the skipped list is a set difference
// against the layout's own `dialects`, so it can never disagree with plan().
constexpr scope::Dialect kAllDialects[] = {
    scope::Dialect::Agentty, scope::Dialect::Agents, scope::Dialect::Claude};

[[nodiscard]] bool reads_dialect(const scope::Layout& l, scope::Dialect d) noexcept {
    // An empty span means "defaulted", and plan() treats that as native-only
    // — mirror that rather than reporting a feature reads nothing.
    const auto ds = l.dialects.empty()
        ? std::span<const scope::Dialect>{scope::kNativeOnly}
        : l.dialects;
    for (scope::Dialect have : ds)
        if (have == d) return true;
    return false;
}

// A source's concrete target: the leaf joined onto the base, except at the
// Explicit locus where the variable already named the exact file.
[[nodiscard]] fs::path target_of(const scope::Source& src,
                                 const scope::Layout& layout,
                                 const scope::Env& env) {
    if (src.locus == scope::Locus::Explicit && env.explicit_config)
        return *env.explicit_config;
    return src.base / fs::path{layout.leaf};
}

[[nodiscard]] bool env_set(std::string_view name) noexcept {
    if (name.empty()) return false;
    const char* v = std::getenv(std::string{name}.c_str());
    return v && v[0];
}

// The approvals leaf MCP vouches against. Same constant bridge.cpp uses;
// named here so the report reads the same store the spawn gate reads.
constexpr char kMcpApprovalsLeaf[] = "mcp_approvals.json";

// What trust_of says about this source, as a word.
//
// Calls the SAME function the spawn gate calls, with the same content hash
// and the same approvals store, so the word printed here is the word the
// connect loop acted on. Anything less is a second opinion, which is what
// this whole file exists to avoid.
[[nodiscard]] std::string trust_word(const scope::Source& src,
                                     const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    const std::string bytes((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
    const scope::Trust t = scope::trust_of(
        src, scope::content_hash(bytes),
        scope::load_approvals(kMcpApprovalsLeaf));
    if (std::holds_alternative<scope::Trusted>(t)) return "trusted";
    if (const auto* b = std::get_if<scope::Blocked>(&t))
        return "blocked: " + b->reason;
    return "needs approval";
}

}  // namespace

std::span<const Entry> inventory() noexcept { return kEntries; }

const Entry* find(std::string_view name) noexcept {
    for (const Entry& e : kEntries)
        if (e.name == name) return &e;
    return nullptr;
}

Report describe(const Entry& e) {
    Report r;
    r.name = e.name;
    r.what = e.what;

    if (e.read) {
        r.explicit_env = e.read->explicit_env;
        const scope::Env env = scope::current_env(*e.read);
        // plan() IS the ladder. No filtering here — that is the invariant the
        // dialect change bought: what plan emits is what gets read.
        for (const scope::Source& src : scope::plan(*e.read, env)) {
            ReadRow row;
            row.locus   = src.locus;
            row.dialect = src.dialect;
            const fs::path p = target_of(src, *e.read, env);
            row.path = p.string();
            std::error_code ec;
            row.exists = fs::exists(p, ec);
            // Only executable content needs vouching, and only a file that
            // exists has bytes to bind an approval to.
            if (row.exists && executable(e.name))
                row.trust = trust_word(src, p);
            r.reads.push_back(std::move(row));
        }
        for (scope::Dialect d : kAllDialects) {
            if (reads_dialect(*e.read, d)) continue;
            r.skipped.push_back({d, std::string{scope::dir_name(d)}});
        }
#if AGENTTY_MCP
        // Foreign configs that EXIST on this machine. Only listed when
        // present: naming a path nobody has is noise, while naming one they
        // do have is the answer to "why is agentty ignoring my servers".
        if (executable(e.name)) {
            const fs::path home = util::home_dir_or_empty();
            std::error_code fec;
            const fs::path proj = fs::current_path(fec);
            for (auto t : mcp::import_::all_tools()) {
                const auto loc = mcp::import_::locations_of(t, home, proj);
                for (const fs::path& p : {loc.user, loc.project}) {
                    if (p.empty() || !fs::is_regular_file(p, fec)) continue;
                    r.foreign.push_back(
                        {std::string{mcp::import_::name_of(t)}, p.string(),
                         "agentty mcp import --from "
                             + std::string{mcp::import_::name_of(t)}});
                }
            }
        }
#endif
    }

    if (e.write) {
        WriteRow w;
        w.env            = e.write->env;
        w.anchor         = anchor_env(e.write->root);
        w.env_moves_root = moves_whole_root(*e.write);
        w.rebuildable   = e.write->life.rebuildable;
        w.keep_last     = e.write->life.keep_last;
        // resolve_dry: report a location without creating it or warning.
        // This command is an observer; asking where the logs go must not
        // make a logs directory.
        if (auto got = dirs::resolve_dry(*e.write)) {
            w.path   = got->path.string();
            w.origin = got->origin;
        } else {
            w.error = got.error().detail.empty() ? "unresolved"
                                                 : got.error().detail;
        }
        r.write = std::move(w);
    }

    AGT_LOG(Persist, Debug, "config", "describe {}: {} read, {} skipped, write={}",
            e.name, r.reads.size(), r.skipped.size(), r.write.has_value());
    return r;
}

namespace {

[[nodiscard]] const char* origin_note(dirs::Origin o) noexcept {
    switch (o) {
        case dirs::Origin::Default:          return "";
        case dirs::Origin::Override:         return "  (overridden)";
        case dirs::Origin::OverrideFellBack: return "  (override unusable — fell back)";
    }
    return "";
}

void print_one(const Report& r) {
    std::printf("%s — %s\n", std::string{r.name}.c_str(),
                std::string{r.what}.c_str());

    if (!r.reads.empty()) {
        // Name the gate in the header when there is one. A reader who sees
        // "needs approval" on a row deserves to know WHY this concern is the
        // one that asks.
        std::printf("\nread, in precedence order%s\n",
                    executable(r.name) ? "  (executable — project configs are"
                                         " vouched by content hash)" : "");
        int n = 0;
        for (const ReadRow& row : r.reads) {
            std::printf("  %d  %-8s %-8s %s%s%s%s\n", ++n,
                        std::string{scope::to_string(row.locus)}.c_str(),
                        std::string{scope::dir_name(row.dialect)}.c_str(),
                        row.path.c_str(),
                        row.exists ? "" : "   (missing)",
                        row.trust.empty() ? "" : "   ",
                        row.trust.c_str());
        }
        if (!r.explicit_env.empty())
            std::printf("     $%s overrides all of the above\n",
                        std::string{r.explicit_env}.c_str());
    }

    // The valuable half. "Not read, and why" is where someone finds out their
    // .claude/mcp.json is ignored on purpose — before they file an issue.
    if (!r.skipped.empty() || !r.foreign.empty()) {
        std::printf("\nnot read\n");
        for (const SkippedDialect& s : r.skipped)
            std::printf("  %-8s not a portable format for %s\n",
                        s.dir.c_str(), std::string{r.name}.c_str());
        // A foreign config gets the command that adopts it. Reading it live
        // would put the user's servers in two places with a precedence rule;
        // importing puts them in one, which is what they asked for.
        for (const ForeignSource& f : r.foreign)
            std::printf("  %-8s %s\n           → %s\n",
                        f.tool.c_str(), f.path.c_str(), f.adopt_with.c_str());
    }

    if (r.write) {
        std::printf("\nwrite\n");
        if (!r.write->error.empty()) {
            std::printf("  unresolved: %s\n", r.write->error.c_str());
        } else {
            std::printf("  %s%s\n", r.write->path.c_str(),
                        origin_note(r.write->origin));
            // Say exactly what each variable moves. The anchor (when the
            // root has one) first, because that is the model; then this
            // spec's own variable, marked as narrower so the two never read
            // as equals.
            if (!r.write->anchor.empty())
                std::printf("  $%s moves the whole root\n",
                            std::string{r.write->anchor}.c_str());
            if (!r.write->env.empty() && !r.write->env_moves_root)
                std::printf("  $%s moves just this%s\n",
                            std::string{r.write->env}.c_str(),
                            r.write->anchor.empty() ? "" : " (narrower)");
            // Lifecycle is on the Spec precisely so it cannot be forgotten;
            // printing it turns an internal discipline into something a user
            // can check.
            if (r.write->rebuildable && r.write->keep_last > 0)
                std::printf("  rebuildable, keeps the newest %u\n",
                            r.write->keep_last);
            else if (r.write->rebuildable)
                std::printf("  rebuildable — safe to delete\n");
            else
                std::printf("  kept indefinitely — nothing reclaims this\n");
        }
    }
    std::printf("\n");
}

// The storage model on one screen. Two roots; the user root has an anchor,
// the project root does not yet. Saying so plainly is the point -- seven flat
// names look like seven decisions, and the one real gap (a project root with
// no anchor, which is why RAG_DIR had to be a one-off) is worth naming rather
// than papering over.
void print_env() {
    std::printf("storage model\n\n");
    std::printf("  ~/.agentty           follows the HUMAN   $AGENTTY_HOME%s\n",
                env_set("AGENTTY_HOME") ? "   (set)" : "");
    std::printf("  <project>/.agentty   follows the CODE    — no anchor yet\n");

    std::printf("\nnarrower — each moves ONE leaf, not a root\n");
    for (const Entry& e : kEntries) {
        if (!e.write || e.write->env.empty()) continue;
        std::printf("  $%-22s %s%s\n", std::string{e.write->env}.c_str(),
                    std::string{e.name}.c_str(),
                    env_set(e.write->env) ? "   (set)" : "");
    }

    std::printf("\nnot storage — names one file to READ\n");
    std::printf("  $%-22s %s%s\n", "AGENTTY_MCP_CONFIG", "mcp",
                env_set("AGENTTY_MCP_CONFIG") ? "   (set)" : "");

    std::printf("\nthreads/cache/logs predate $AGENTTY_HOME and each move one\n"
                "leaf it already moves. nothing new goes on that axis — a new\n"
                "category is a leaf under an existing root.\n");
}

void print_table() {
    std::printf("where things live\n\n");
    std::printf("  %-10s %-22s %-6s %s\n", "concern", "what", "reads", "writes");
    for (const Entry& e : kEntries) {
        const Report r = describe(e);
        char reads[16] = "—";
        if (e.read) std::snprintf(reads, sizeof reads, "%zu", r.reads.size());
        std::printf("  %-10s %-22s %-6s %s\n",
                    std::string{e.name}.c_str(),
                    std::string{e.what}.c_str(),
                    reads,
                    r.write ? (r.write->error.empty() ? r.write->path.c_str()
                                                      : "unresolved")
                            : "—");
    }
    std::printf("\n`agentty config <concern>` for the full ladder,"
                " `agentty config env` for the model.\n");
}

}  // namespace

int cmd_config(std::span<const std::string> argv) {
    if (argv.empty()) {
        print_table();
        return 0;
    }
    const std::string& want = argv.front();
    if (want == "env") {
        print_env();
        return 0;
    }
    if (const Entry* e = find(want)) {
        print_one(describe(*e));
        return 0;
    }
    std::fprintf(stderr, "unknown concern: %s\n\nknown: env", want.c_str());
    for (const Entry& e : kEntries)
        std::fprintf(stderr, " %s", std::string{e.name}.c_str());
    std::fprintf(stderr, "\n");
    return 1;
}

}  // namespace agentty::config
