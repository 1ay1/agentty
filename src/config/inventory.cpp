// config/inventory.cpp — resolve and print the inventory.
//
// Everything here folds the SAME functions the real resolvers fold:
// scope::plan for the read ladder, dirs::resolve_dry for the write path.
// Nothing re-derives a location, which is what makes the output trustworthy
// rather than a second opinion. See inventory.hpp for the argument.

#include "agentty/config/inventory.hpp"

#include "agentty/util/logx.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>

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
            r.reads.push_back(std::move(row));
        }
        for (scope::Dialect d : kAllDialects) {
            if (reads_dialect(*e.read, d)) continue;
            r.skipped.push_back({d, std::string{scope::dir_name(d)}});
        }
    }

    if (e.write) {
        WriteRow w;
        w.env           = e.write->env;
        w.anchor        = anchor_env(e.write->root);
        w.env_is_anchor = is_anchor(*e.write);
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
        std::printf("\nread, in precedence order\n");
        int n = 0;
        for (const ReadRow& row : r.reads) {
            std::printf("  %d  %-8s %-8s %s%s\n", ++n,
                        std::string{scope::to_string(row.locus)}.c_str(),
                        std::string{scope::dir_name(row.dialect)}.c_str(),
                        row.path.c_str(),
                        row.exists ? "" : "   (missing)");
        }
        if (!r.explicit_env.empty())
            std::printf("     $%s overrides all of the above\n",
                        std::string{r.explicit_env}.c_str());
    }

    // The valuable half. "Not read, and why" is where someone finds out their
    // .claude/mcp.json is ignored on purpose — before they file an issue.
    if (!r.skipped.empty()) {
        std::printf("\nnot read\n");
        for (const SkippedDialect& s : r.skipped)
            std::printf("  %-8s not a portable format for %s\n",
                        s.dir.c_str(), std::string{r.name}.c_str());
    }

    if (r.write) {
        std::printf("\nwrite\n");
        if (!r.write->error.empty()) {
            std::printf("  unresolved: %s\n", r.write->error.c_str());
        } else {
            std::printf("  %s%s\n", r.write->path.c_str(),
                        origin_note(r.write->origin));
            // Always lead with the ANCHOR, because that is the model: one
            // variable per root. A leaf variable is mentioned second and
            // marked as narrower, so nobody reads the two as equals.
            std::printf("  $%s moves the whole root\n",
                        std::string{r.write->anchor}.c_str());
            if (!r.write->env.empty() && !r.write->env_is_anchor)
                std::printf("  $%s moves just this one (narrower)\n",
                            std::string{r.write->env}.c_str());
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

// The storage model on one screen. Two roots, one anchor each, and the leaf
// variables demoted to what they are: narrower conveniences that predate the
// anchors. Printing them in that shape is the point — seven flat names look
// like seven decisions, and they are not.
void print_env() {
    std::printf("storage model\n\n");
    std::printf("  ~/.agentty           follows the HUMAN   $AGENTTY_HOME%s\n",
                env_set("AGENTTY_HOME") ? "    (set)" : "");
    std::printf("  <project>/.agentty   follows the CODE    $AGENTTY_RAG_DIR%s\n",
                env_set("AGENTTY_RAG_DIR") ? " (set)" : "");

    std::printf("\nnarrower — each moves ONE leaf $AGENTTY_HOME already covers\n");
    for (const Entry& e : kEntries) {
        if (!e.write || is_anchor(*e.write) || e.write->env.empty()) continue;
        std::printf("  $%-22s %s%s\n", std::string{e.write->env}.c_str(),
                    std::string{e.name}.c_str(),
                    env_set(e.write->env) ? "   (set)" : "");
    }

    std::printf("\nnot storage — names one file to READ\n");
    std::printf("  $%-22s %s%s\n", "AGENTTY_MCP_CONFIG", "mcp",
                env_set("AGENTTY_MCP_CONFIG") ? "   (set)" : "");
    std::printf("\nnew categories get a leaf under an existing root, not a\n"
                "new variable — the anchor already relocates them.\n");
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
