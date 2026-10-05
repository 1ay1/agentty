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
constexpr std::string_view kMcpApprovalsLeaf = kMcpApprovals;

// What trust_of says about this source, as a word.
//
// Calls the SAME function the spawn gate calls, with the same content hash
// and the same approvals store, so the word printed here is the word the
// connect loop acted on. Anything less is a second opinion, which is what
// this whole file exists to avoid.
[[nodiscard]] std::string trust_word(const scope::Source& src,
                                     const fs::path& file) {
    // is_regular_file before opening: a DIRECTORY opens fine and then throws
    // out of the first read, which an istreambuf_iterator slurp does not
    // catch. Reporting trust must never be the thing that aborts the
    // process.
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return {};
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

// Bytes under `dir`, recursively. Symlinks are NOT followed: a link into
// someone else's tree would make agentty report their disk as ours, and a
// cycle would hang the command.
//
// `out_partial` is set when some of the tree could not be examined. The
// count then UNDERSTATES, and saying so matters more than the number does:
// a silent 0 B next to 2 MB of unreadable files is the same class of
// confident lie this whole file exists to prevent.
[[nodiscard]] std::uintmax_t dir_bytes(const fs::path& dir, bool& out_partial) {
    out_partial = false;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        // A spec may name a FILE rather than a directory.
        const auto sz = fs::file_size(dir, ec);
        return ec ? 0 : sz;
    }
    std::uintmax_t total = 0;
    fs::recursive_directory_iterator it{
        dir, fs::directory_options::skip_permission_denied, ec};
    if (ec) { out_partial = true; return 0; }
    // Advance MANUALLY with the error_code increment. A range-for calls the
    // throwing operator++, and descending into a chmod-000 subdirectory
    // throws there -- skip_permission_denied governs the initial open, not
    // every step. That threw away the whole running total and reported 0 B
    // for a 1.9 MB tree.
    const fs::recursive_directory_iterator end{};
    while (it != end) {
        std::error_code sec;
        if (it->is_regular_file(sec) && !sec) {
            const auto sz = it->file_size(sec);
            if (!sec) total += sz;
            else      out_partial = true;
        } else if (sec) {
            out_partial = true;
        } else if (it->is_directory(sec) && !sec) {
            // skip_permission_denied makes an unreadable directory vanish
            // SILENTLY -- no error_code, nothing to notice, and everything
            // inside it simply never appears. A chmod-000 subdir holding
            // 2 MB reported as 0 B with no hint anything was missed.
            //
            // So probe it directly. Opening it is the only way to learn
            // that the recursion is about to pretend it is empty.
            std::error_code oec;
            fs::directory_iterator probe{it->path(), oec};
            if (oec) out_partial = true;
        }
        std::error_code iec;
        it.increment(iec);
        if (iec) {
            // Cannot descend here. Keep what we counted, note that the
            // answer is short, and stop -- resuming past an unreadable
            // branch is not something the iterator offers.
            out_partial = true;
            break;
        }
    }
    return total;
}

// Three significant-ish figures, which is all anyone needs to answer "is
// this the big one". Exact bytes would be noise.
[[nodiscard]] std::string human_bytes(std::uintmax_t n) {
    const char* unit[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(n);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    char buf[32];
    std::snprintf(buf, sizeof buf, u == 0 ? "%.0f %s" : "%.1f %s", v, unit[u]);
    return buf;
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
            std::error_code sec;
            if (fs::exists(got->path, sec)) {
                // Specs that share a resolved directory (rag and feedback
                // both land on <project>/.agentty) must not each claim the
                // whole tree, or the column sums to double the real disk.
                // The FIRST row for a path owns the measurement; later ones
                // say so rather than repeating a number that isn't theirs.
                bool owned_by_earlier = false;
                for (const Entry& other : kEntries) {
                    if (&other == &e) break;   // only rows BEFORE this one
                    if (!other.write) continue;
                    if (auto o = dirs::resolve_dry(*other.write);
                        o && o->path == got->path) {
                        owned_by_earlier = true;
                        break;
                    }
                }
                w.shared_dir = owned_by_earlier;
                if (!owned_by_earlier) {
                    bool partial = false;
                    w.bytes    = dir_bytes(got->path, partial);
                    w.partial  = partial;
                    w.measured = true;
                }
            }
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
            if (r.write->measured)
                std::printf("  %s on disk%s\n",
                            human_bytes(r.write->bytes).c_str(),
                            r.write->partial
                                ? "  (at least — part of it is unreadable)" : "");
            else if (r.write->shared_dir)
                std::printf("  shares this directory with another concern"
                            " (counted there)\n");
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

// The storage model on one screen. Two roots, one anchor each; everything
// else is a leaf an anchor already moves.
void print_env() {
    std::printf("storage model\n\n");
    std::printf("  ~/.agentty           follows the HUMAN   $AGENTTY_HOME%s\n",
                env_set("AGENTTY_HOME") ? "         (set)" : "");
    std::printf("  <project>/.agentty   follows the CODE    $AGENTTY_PROJECT_DIR%s\n",
                env_set("AGENTTY_PROJECT_DIR") ? "  (set)" : "");
    std::printf("\nthat is the whole model. every category is a leaf under one\n"
                "of those two, so the anchor moves it.\n");

    // The leftovers. Shipped in 0.9.19 before the anchors existed, kept
    // because removing a released variable breaks a setup with no error.
    bool any = false;
    for (const Entry& e : kEntries) {
        if (!e.write || e.write->env.empty()) continue;
        if (!any) {
            std::printf("\nalso accepted, from before the anchors — each moves one leaf\n");
            any = true;
        }
        std::printf("  $%-22s %s%s\n", std::string{e.write->env}.c_str(),
                    std::string{e.name}.c_str(),
                    env_set(e.write->env) ? "   (set)" : "");
    }

    std::printf("\n$AGENTTY_MCP_CONFIG is not storage — it names one file to read.%s\n",
                env_set("AGENTTY_MCP_CONFIG") ? "   (set)" : "");
}

void print_table() {
    std::printf("where things live\n\n");
    // Columns are padded by hand rather than with printf's %-Ns because the
    // placeholder is an em dash: three BYTES, one COLUMN. %-6s pads to a byte
    // count and would short the cell by two, which is exactly how a table
    // with non-ASCII cells goes ragged.
    auto pad = [](std::string s, std::size_t cols) {
        std::size_t width = 0;
        for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++width;
        if (width < cols) s.append(cols - width, ' ');
        return s;
    };
    std::printf("  %s%s%s%s%s\n",
                pad("concern", 11).c_str(), pad("what", 23).c_str(),
                pad("reads", 7).c_str(), pad("on disk", 10).c_str(), "writes");
    std::uintmax_t total = 0;
    for (const Entry& e : kEntries) {
        const Report r = describe(e);
        std::string reads = "—";
        if (e.read) reads = std::to_string(r.reads.size());
        // The size column is the one #58 needed: "move my non-config data"
        // is unanswerable until you can see which categories are big.
        std::string size = "—";
        if (r.write && r.write->measured) {
            size = human_bytes(r.write->bytes);
            if (r.write->partial) size += "+";   // undercounts; see detail view
            total += r.write->bytes;
        } else if (r.write && r.write->shared_dir) {
            size = "(shared)";
        }
        const std::string where =
            r.write ? (r.write->error.empty() ? r.write->path : "unresolved")
                    : "—";
        std::printf("  %s%s%s%s%s\n",
                    pad(std::string{e.name}, 11).c_str(),
                    pad(std::string{e.what}, 23).c_str(),
                    pad(reads, 7).c_str(), pad(size, 10).c_str(),
                    where.c_str());
    }
    std::printf("  %s%s%s%s\n", pad("", 11).c_str(), pad("total", 23).c_str(),
                pad("", 7).c_str(), human_bytes(total).c_str());
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
