// agentty::tools::skills — implementation. See skills.hpp for the
// progressive-disclosure rationale and the discovery-root table.

#include "agentty/tool/skills.hpp"

#include "agentty/tool/util/fs_helpers.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>

namespace agentty::tools::skills {

namespace fs = std::filesystem;

namespace {

[[nodiscard]] fs::path home_dir() noexcept {
    if (auto* h = std::getenv("HOME"); h && *h) return fs::path{h};
#if defined(_WIN32)
    if (auto* h = std::getenv("USERPROFILE"); h && *h) return fs::path{h};
#endif
    return {};
}

// Trim ASCII whitespace from both ends.
[[nodiscard]] std::string trim(std::string s) {
    auto issp = [](char c) { return c==' '||c=='\t'||c=='\r'||c=='\n'; };
    while (!s.empty() && issp(s.front())) s.erase(s.begin());
    while (!s.empty() && issp(s.back()))  s.pop_back();
    return s;
}

// Read a file with a hard byte cap. Empty on missing / unreadable / oversize.
[[nodiscard]] std::string read_capped(const fs::path& p, std::size_t cap) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec) || ec) return {};
    auto sz = fs::file_size(p, ec);
    if (ec || sz == 0 || sz > cap) return {};
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string out(static_cast<std::size_t>(sz), '\0');
    f.read(out.data(), static_cast<std::streamsize>(sz));
    out.resize(static_cast<std::size_t>(f.gcount()));
    return out;
}

// Parse `key: value` from a YAML frontmatter line. LENIENT by design
// (spec guidance): only the FIRST colon splits, so a description like
// "Use when: handling PDFs" — technically invalid YAML that other
// clients' parsers accept — parses fine here. Returns false if the
// line isn't a simple scalar mapping.
[[nodiscard]] bool parse_kv(const std::string& line,
                            std::string& key, std::string& val) {
    auto colon = line.find(':');
    if (colon == std::string::npos) return false;
    key = trim(line.substr(0, colon));
    val = trim(line.substr(colon + 1));
    // Strip matching surrounding quotes on the value.
    if (val.size() >= 2 &&
        ((val.front() == '"' && val.back() == '"') ||
         (val.front() == '\'' && val.back() == '\''))) {
        val = val.substr(1, val.size() - 2);
    }
    return !key.empty();
}

[[nodiscard]] bool is_truthy(const std::string& v) noexcept {
    return v == "true" || v == "1" || v == "yes";
}

// Split a SKILL.md into frontmatter fields + body. Frontmatter is the
// block between the first two `---` lines. `slug` is the directory
// name, used as the name fallback (lenient: a name/dir mismatch loads
// anyway, with the frontmatter name winning).
[[nodiscard]] Skill parse_skill(const std::string& raw, const std::string& slug,
                                const std::string& source) {
    Skill s;
    s.name = slug;
    s.source = source;

    std::istringstream in(raw);
    std::string line;
    std::streampos body_start = 0;
    bool fm_done = false;
    std::string first;
    if (std::getline(in, first) && trim(first) == "---") {
        while (std::getline(in, line)) {
            if (trim(line) == "---") { fm_done = true; body_start = in.tellg(); break; }
            std::string k, v;
            if (parse_kv(line, k, v)) {
                if (k == "name" && !v.empty())             s.name = v;
                else if (k == "description")               s.description = v;
                else if (k == "compatibility")             s.compatibility = v;
                else if (k == "allowed-tools")             s.allowed_tools = v;
                else if (k == "disable-model-invocation")  s.user_only = is_truthy(v);
                // `license` / `metadata` / unknown keys: tolerated, unused.
            }
        }
    }

    if (fm_done && body_start != std::streampos(-1)) {
        s.body = trim(raw.substr(static_cast<std::size_t>(body_start)));
    } else {
        // No frontmatter — treat the whole file as the body.
        s.body = trim(raw);
    }
    // Lenient fallback (spec: a description is essential for disclosure,
    // but we can synthesise one): first non-blank body line.
    if (s.description.empty()) {
        std::istringstream b(s.body);
        std::string l;
        while (std::getline(b, l)) {
            auto t = trim(l);
            if (!t.empty()) { s.description = t; break; }
        }
    }
    return s;
}

// Enumerate bundled resource files under the skill dir (tier 3 of
// progressive disclosure). Recursive but bounded: depth ≤ 3, at most
// kMaxResources entries, skipping dotfiles and SKILL.md itself. Paths
// recorded relative to the skill dir with forward slashes so the model
// can splice them after the absolute dir we hand it.
void enumerate_resources(const fs::path& dir, std::vector<std::string>& out) {
    std::error_code ec;
    auto opts = fs::directory_options::skip_permission_denied;
    fs::recursive_directory_iterator it(dir, opts, ec), end;
    for (; !ec && it != end && out.size() < kMaxResources; it.increment(ec)) {
        if (it.depth() > 2) { it.disable_recursion_pending(); continue; }
        const auto& p = it->path();
        auto fname = p.filename().string();
        if (!fname.empty() && fname.front() == '.') {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        if (fname == "SKILL.md" && it.depth() == 0) continue;
        auto rel = fs::relative(p, dir, ec);
        if (ec) continue;
        std::string r = rel.generic_string();   // forward slashes everywhere
        out.push_back(std::move(r));
    }
    std::sort(out.begin(), out.end());
}

// Scan one root for <slug>/SKILL.md entries, appending to `out` (skipping
// names already present — earlier roots win, see header precedence
// table). Appends each found SKILL.md's mtime into `sig` so an in-place
// edit (same dir mtime) still invalidates the cache.
void scan_root(const fs::path& root, const std::string& source,
               std::vector<Skill>& out, std::string& sig) {
    std::error_code ec;
    if (!fs::is_directory(root, ec) || ec) return;
    auto mt = fs::last_write_time(root, ec);
    if (!ec) sig += source + ":" + std::to_string(mt.time_since_epoch().count()) + ";";

    std::vector<fs::path> dirs;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec)) dirs.push_back(it->path());
    }
    std::sort(dirs.begin(), dirs.end());
    for (const auto& d : dirs) {
        if (out.size() >= kMaxSkills) break;
        fs::path md = d / "SKILL.md";
        std::error_code mec;
        auto fmt = fs::last_write_time(md, mec);
        if (!mec) sig += std::to_string(fmt.time_since_epoch().count()) + ";";
        std::string raw = read_capped(md, kMaxBodyBytes);
        if (raw.empty()) continue;
        Skill s = parse_skill(raw, d.filename().string(), source);
        if (s.name.empty()) continue;
        // Shadow: earlier roots (project before user, native before
        // interop) win on name collision.
        if (std::ranges::any_of(out, [&](const Skill& e){ return e.name == s.name; }))
            continue;
        std::error_code cec;
        auto abs = fs::weakly_canonical(d, cec);
        s.dir = cec ? fs::absolute(d, cec) : abs;
        enumerate_resources(s.dir, s.resources);
        // Tier-3 access: allowlist the skill dir for READS so the model
        // can fetch bundled scripts/references that live outside the
        // workspace (~/.agentty/skills/...) without tripping the
        // boundary. Read-only — the write/edit gate never consults it.
        util::allow_read_root(s.dir);
        out.push_back(std::move(s));
    }
}

std::vector<Skill>& cache() {
    static std::vector<Skill> c;
    return c;
}

} // namespace

const std::vector<Skill>& all() {
    static std::mutex mu;
    static std::string cached_sig = "\x01uninit";
    std::lock_guard lk(mu);

    // Build the current signature from every root's + SKILL.md's mtime;
    // rescan only when it changed (cheap stat vs full parse per turn).
    std::string sig;
    std::vector<Skill> fresh;
    // Precedence order — see header. Project shadows user; within a
    // scope the native dir shadows the interop conventions.
    const fs::path home = home_dir();
    const fs::path project_roots[] = {
        fs::path{".agentty"} / "skills",
        fs::path{".agents"}  / "skills",
        fs::path{".claude"}  / "skills",
    };
    for (const auto& r : project_roots) scan_root(r, "project", fresh, sig);
    if (!home.empty()) {
        const fs::path user_roots[] = {
            home / ".agentty" / "skills",
            home / ".agents"  / "skills",
            home / ".claude"  / "skills",
        };
        for (const auto& r : user_roots) scan_root(r, "user", fresh, sig);
    }

    if (sig != cached_sig) {
        cache() = std::move(fresh);
        cached_sig = sig;
    }
    return cache();
}

const Skill* find(std::string_view name) {
    for (const auto& s : all())
        if (s.name == name) return &s;
    return nullptr;
}

std::string catalog_block() {
    const auto& skills = all();
    // Hidden beats listed-but-blocked: user_only skills never appear, so
    // the model can't waste a turn trying to activate one.
    std::size_t eligible = 0;
    for (const auto& s : skills) if (!s.user_only) ++eligible;
    if (eligible == 0) return {};

    std::ostringstream m;
    m << "\n\n<skills>\n"
      << "On-demand skills are available. Each is a focused instruction "
         "doc you can load IN FULL with the `skill` tool when its task "
         "comes up \u2014 don't guess the contents, load it. Skills may "
         "bundle resource files (scripts/, references/, assets/); the "
         "activation result lists them \u2014 `read` the specific file "
         "when the instructions reference it, resolving relative paths "
         "against the skill directory the result names. Listed: name "
         "\u2014 description.\n";
    for (const auto& s : skills) {
        if (s.user_only) continue;
        m << "- " << s.name;
        if (!s.description.empty()) m << " \u2014 " << s.description;
        m << "\n";
    }
    m << "</skills>";
    return m.str();
}

std::string activation_payload(const Skill& s) {
    std::ostringstream out;
    out << "<skill_content name=\"" << s.name << "\">\n";
    if (!s.description.empty()) out << s.description << "\n\n";
    if (!s.compatibility.empty())
        out << "Compatibility: " << s.compatibility << "\n\n";
    out << s.body << "\n";
    if (!s.dir.empty()) {
        out << "\nSkill directory: " << s.dir.string() << "\n"
            << "Relative paths in this skill resolve against the skill "
               "directory \u2014 use absolute paths in tool calls.\n";
    }
    if (!s.resources.empty()) {
        out << "\n<skill_resources>\n";
        for (const auto& r : s.resources) out << "  " << r << "\n";
        if (s.resources.size() >= kMaxResources)
            out << "  (listing capped \u2014 there may be more files)\n";
        out << "</skill_resources>\n"
            << "Resources are NOT loaded \u2014 `read` the specific file "
               "when the instructions call for it.\n";
    }
    out << "</skill_content>";
    return out.str();
}

namespace {
struct Activations {
    std::mutex mu;
    std::vector<std::string> names;
};
[[nodiscard]] Activations& activations() {
    static Activations a;
    return a;
}
} // namespace

bool note_activated(std::string_view name) {
    auto& a = activations();
    std::lock_guard lk(a.mu);
    for (const auto& n : a.names) if (n == name) return false;
    a.names.emplace_back(name);
    return true;
}

void reset_activations() {
    auto& a = activations();
    std::lock_guard lk(a.mu);
    a.names.clear();
}

} // namespace agentty::tools::skills
