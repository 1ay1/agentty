// commands.cpp — user-authored slash commands. See commands.hpp for the
// format, discovery roots, and substitution contract. Structure mirrors
// skills.cpp deliberately (same lenient frontmatter subset, same mtime-
// signature cache) so the two loaders stay conceptually one thing.

#include "agentty/util/user_root.hpp"
#include "agentty/tool/commands.hpp"
#include "agentty/util/home_dir.hpp"

#include "agentty/scope/scope.hpp"
#include "agentty/config/inventory.hpp"   // kCommandsLayout — the one declaration
#include "agentty/util/capped_read.hpp"   // the one capped-read primitive

#include <maya/runtime.hpp>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <ranges>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace agentty::tools::commands {

namespace {

constexpr std::size_t kMaxCommands  = 128;
constexpr std::size_t kMaxBodyBytes = 64 * 1024;
constexpr int         kMaxDepth     = 3;   // namespace nesting (a:b:c)

[[nodiscard]] fs::path home_dir() noexcept {
    return agentty::util::home_dir_or_empty();
}

[[nodiscard]] std::string trim(std::string_view v) {
    std::size_t b = 0, e = v.size();
    while (b < e && std::isspace(static_cast<unsigned char>(v[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(v[e - 1]))) --e;
    return std::string{v.substr(b, e - b)};
}

// NOTE: the local read_capped() is gone — see util/capped_read.hpp for why
// three copies of it all shipped the same bug.

[[nodiscard]] bool parse_kv(const std::string& line,
                            std::string& key, std::string& val) {
    auto colon = line.find(':');
    if (colon == std::string::npos) return false;
    key = trim(line.substr(0, colon));
    val = trim(line.substr(colon + 1));
    if (val.size() >= 2 &&
        ((val.front() == '"' && val.back() == '"') ||
         (val.front() == '\'' && val.back() == '\''))) {
        val = val.substr(1, val.size() - 2);
    }
    return !key.empty();
}

// Parse one <name>.md into a Command. Frontmatter is optional; only the
// scalar keys `description` / `argument-hint` are recognised (commands
// are simpler than skills — no block scalars needed; a block-scalar
// description in the wild still parses, it just keeps the `|` marker
// out and takes the first continuation line via the body fallback).
[[nodiscard]] Command parse_command(const std::string& raw,
                                    std::string name,
                                    const std::string& source) {
    Command c;
    c.name   = std::move(name);
    c.source = source;

    std::istringstream in(raw);
    std::string line;
    std::streampos body_start = 0;
    bool fm_done = false;
    std::string first;
    if (std::getline(in, first) && trim(first) == "---") {
        while (std::getline(in, line)) {
            if (trim(line) == "---") {
                fm_done = true;
                body_start = in.tellg();
                break;
            }
            std::string k, v;
            if (!parse_kv(line, k, v)) continue;
            if      (k == "description")   c.description   = v;
            else if (k == "argument-hint") c.argument_hint = v;
            // unknown keys tolerated (Claude has allowed-tools, model —
            // not enforced here; the expanded prompt goes through the
            // normal turn pipeline with the normal tool gates).
        }
    }
    if (fm_done && body_start != std::streampos(-1)) {
        c.body = trim(raw.substr(static_cast<std::size_t>(body_start)));
    } else {
        c.body = trim(raw);
    }
    if (c.description.empty()) {
        std::istringstream b(c.body);
        std::string l;
        while (std::getline(b, l)) {
            auto t = trim(l);
            if (!t.empty()) { c.description = t; break; }
        }
    }
    return c;
}

// Recursively scan `dir` for *.md command files, joining subdirectory
// names into the command name with ':' (git/fixup.md → git:fixup).
void scan_dir(const fs::path& dir, const std::string& prefix,
              const std::string& source, int depth,
              std::vector<Command>& out, std::string& sig) {
    if (depth > kMaxDepth) return;
    std::error_code ec;
    std::vector<fs::path> entries;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec))
        entries.push_back(it->path());
    std::sort(entries.begin(), entries.end());

    for (const auto& p : entries) {
        if (out.size() >= kMaxCommands) return;
        std::error_code sec;
        if (fs::is_directory(p, sec)) {
            const std::string sub = p.filename().string();
            if (!sub.empty() && sub[0] != '.')
                scan_dir(p, prefix.empty() ? sub : prefix + ":" + sub,
                         source, depth + 1, out, sig);
            continue;
        }
        if (p.extension() != ".md") continue;
        const std::string stem = p.stem().string();
        if (stem.empty() || stem[0] == '.') continue;

        auto fmt = fs::last_write_time(p, sec);
        if (!sec)
            sig += std::to_string(static_cast<long long>(
                       fmt.time_since_epoch().count())) + ";";
        // Commands have no `agentty commands` CLI to report into, so an
        // oversized file is skipped -- but the TooBig case is now named
        // rather than hidden inside an empty string, so if that CLI ever
        // ships the branch to fill in is already here.
        const auto got = ::agentty::util::capped_read(p, kMaxBodyBytes);
        if (!::agentty::util::has_content(got)) continue;
        const std::string& raw = ::agentty::util::bytes_or_empty(got);
        if (raw.empty()) continue;

        std::string name = prefix.empty() ? stem : prefix + ":" + stem;
        // Shadow: earlier roots win on a name collision.
        if (std::ranges::any_of(out,
                [&](const Command& e) { return e.name == name; }))
            continue;
        Command c = parse_command(raw, std::move(name), source);
        if (c.body.empty()) continue;
        std::error_code cec;
        auto abs = fs::weakly_canonical(p, cec);
        c.file = (cec ? fs::absolute(p, cec) : abs).string();
        out.push_back(std::move(c));
    }
}

void scan_root(const fs::path& root, const std::string& source,
               std::vector<Command>& out, std::string& sig) {
    std::error_code ec;
    if (!fs::is_directory(root, ec) || ec) return;
    auto mt = fs::last_write_time(root, ec);
    if (!ec)
        sig += source + ":" +
               std::to_string(static_cast<long long>(
                   mt.time_since_epoch().count())) + ";";
    scan_dir(root, /*prefix=*/{}, source, /*depth=*/0, out, sig);
}

// The scanned commands and the mtime signature they were scanned at.
struct Cache {
    std::vector<Command> commands;
    std::string          sig = "\x01uninit";
};
maya::guarded<Cache>& cache() {
    static maya::guarded<Cache> c;
    return c;
}

} // namespace

std::vector<Command> all() {
    // Scan outside the lock (directory walk + file reads), then keep the
    // fresh list only if the signature moved.
    std::string sig;
    std::vector<Command> fresh;
    // Root ladder from scope::plan (Locus-major, Dialect-minor): project
    // .agentty ▷ .agents ▷ .claude ▷ the same three under ~. scan_root does
    // the first-name-wins shadow + mtime-sig per root. Project stays cwd-
    // relative (env.project_root = ".") — commands has always resolved
    // ".agentty/commands" against cwd.
    scope::Env env;
    env.home             = home_dir();
    env.user_native_base = ::agentty::util::user_root();
    env.project_root     = fs::path{"."};
    env.project_writable = true;
    const scope::Layout& layout = ::agentty::config::kCommandsLayout;
    for (const scope::Source& src : scope::plan(layout, env)) {
        scan_root(src.base / layout.leaf,
                  std::string{scope::to_string(src.locus)}, fresh, sig);
    }
    return cache().with(
        [](Cache& c, std::string s, std::vector<Command> f) {
            if (s != c.sig) {
                c.commands = std::move(f);
                c.sig      = std::move(s);
            }
            return c.commands;
        },
        std::move(sig), std::move(fresh));
}

std::optional<Command> find(std::string_view name) {
    for (auto& c : all())
        if (c.name == name) return std::move(c);
    return std::nullopt;
}

void invalidate_cache() {
    cache().with([](Cache& c) { c.sig = "\x01invalidated"; });
}

std::string expand(std::string_view body, std::string_view args) {
    // Whitespace-split positionals once.
    std::vector<std::string> pos;
    {
        std::size_t i = 0;
        while (i < args.size()) {
            while (i < args.size() &&
                   std::isspace(static_cast<unsigned char>(args[i]))) ++i;
            std::size_t b = i;
            while (i < args.size() &&
                   !std::isspace(static_cast<unsigned char>(args[i]))) ++i;
            if (i > b) pos.emplace_back(args.substr(b, i - b));
        }
    }
    const std::string all_args = trim(args);

    std::string out;
    out.reserve(body.size() + all_args.size());
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] != '$') { out += body[i]; continue; }
        // `$` at end of body: literal.
        if (i + 1 >= body.size()) { out += '$'; continue; }
        const char c = body[i + 1];
        if (c == '$') { out += '$'; ++i; continue; }          // $$ → $
        if (c >= '1' && c <= '9') {                           // $1..$9
            const std::size_t idx = static_cast<std::size_t>(c - '1');
            if (idx < pos.size()) out += pos[idx];
            ++i;
            continue;
        }
        constexpr std::string_view kArgs = "ARGUMENTS";
        if (body.compare(i + 1, kArgs.size(), kArgs) == 0) {  // $ARGUMENTS
            out += all_args;
            i += kArgs.size();
            continue;
        }
        out += '$';                                           // unknown: literal
    }
    return out;
}

std::optional<std::string> try_expand(std::string_view text) {
    if (text.empty() || text[0] != '/') return std::nullopt;
    return try_expand(text, all());
}

std::optional<std::string> try_expand(std::string_view text, const std::vector<Command>& known) {
    // Shape: starts with '/', then a command name (no whitespace), then
    // optional whitespace + arguments. Leading whitespace before '/'
    // means it's prose, not a command.
    if (text.empty() || text[0] != '/') return std::nullopt;
    std::size_t name_end = 1;
    while (name_end < text.size() &&
           !std::isspace(static_cast<unsigned char>(text[name_end])))
        ++name_end;
    if (name_end == 1) return std::nullopt;   // bare "/"
    const std::string_view name = text.substr(1, name_end - 1);

    const auto cmd = std::ranges::find(known, name, &Command::name);
    if (cmd == known.end()) return std::nullopt;   // /etc/hosts, /unknown → plain text

    std::string_view args =
        name_end < text.size() ? text.substr(name_end + 1) : std::string_view{};
    return expand(cmd->body, args);
}

} // namespace agentty::tools::commands
