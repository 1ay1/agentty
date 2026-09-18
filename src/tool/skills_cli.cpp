// agentty::tools::skills — the `agentty skill` CLI and the approvals store.
//
// WHY THIS EXISTS, AND WHY IT ISN'T A SETUP SCREEN
//
// A skill is instructions, not sandboxed code. Most are prose ("house
// style for docs") and cost nothing but catalog room. Some tell the agent
// to run programs, reach the network, and write credentials into the
// user's repo — and THAT is the same threat class as a project-local MCP
// server, which agentty already solved: scope::Trust, approval pinned to
// CONTENT, never to a name (the MCPoison lesson, CVE-2025-54136).
//
// So a capability enters agentty exactly one way: the user names it.
//
//     agentty skill add github.com/user/repo
//
// Same command for a vendor's skill, a local directory, and a company's
// internal skill. No first-run screen, no menu entry, no bundled default
// — because the moment one vendor gets placement there is no principled
// answer for the next one, and the setup flow becomes a billboard one
// good PR at a time.
//
// THE LOAD-BEARING PART: agentty writes the disclosure, not the skill.
//
// The consent prompt is RENDERED from the skill's declared `effects:` and
// `source:`. A skill author cannot write their own reassuring summary —
// they can only declare capability, and agentty turns declarations into
// plain sentences. A skill that under-declares gets a prompt that
// under-sells it, but it also doesn't get to LIE in the prompt, which is
// the failure mode worth designing against.
//
// Credit: the frontmatter-declared-capability idea comes from PR #47
// (Arag Agrawal, Cohesivity). That PR proposed a bundled effectful skill
// with a first-run screen; the placement is what agentty declined, but it
// was right that there was a real gap — there was no vocabulary for "this
// skill will run programs and reach the network", and no install path
// that treated every source the same.

#include "agentty/tool/skills.hpp"

#include "agentty/scope/scope.hpp"
#include "agentty/util/home_dir.hpp"
#include "agentty/util/user_root.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace agentty::tools::skills {
namespace {

// ── Approvals store ────────────────────────────────────────────────────────

// Parse one SKILL.md off disk WITHOUT going through discovery — `add` must
// describe the file it is about to copy, not a skill that isn't installed
// yet. Reuses the shared parser via the install-then-reload path below is
// not possible (chicken/egg), so this reads the frontmatter it needs:
// name, description, effects, source. Deliberately minimal — the full
// parse happens on load, and lint() reports anything odd afterwards.
std::optional<Skill> peek_skill(const fs::path& md) {
    std::ifstream in(md);
    if (!in) return std::nullopt;
    std::string line;
    if (!std::getline(in, line) || line.find("---") == std::string::npos)
        return std::nullopt;

    Skill s;
    auto trim = [](std::string v) {
        const auto b = v.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return std::string{};
        const auto e = v.find_last_not_of(" \t\r\n");
        return v.substr(b, e - b + 1);
    };
    while (std::getline(in, line)) {
        if (trim(line) == "---") break;
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const auto k = trim(line.substr(0, colon));
        const auto v = trim(line.substr(colon + 1));
        if      (k == "name")        s.name = v;
        else if (k == "description") s.description = v;
        else if (k == "source")      s.origin = v;
        else if (k == "effects")     s.effects = parse_effects(v);
    }
    // Body: everything after the closing fence. Needed for the content hash.
    std::ostringstream body;
    body << in.rdbuf();
    s.body = body.str();
    if (s.name.empty()) s.name = md.parent_path().filename().string();
    return s;
}

// ── Effects → plain sentences ──────────────────────────────────────────────
// One line per declared bit, in bit order so the prompt never reorders
// between runs. The wording says what the AGENT will be told to do, not
// what the skill "can" do — a skill can't do anything; it persuades.
struct EffectLine {
    std::string_view label;
    std::string_view gloss;
};

std::vector<EffectLine> effect_lines(EffectSet e) {
    std::vector<EffectLine> out;
    if (e.has(Effect::ReadFs))
        out.push_back({"read files",        "open and scan paths on this machine"});
    if (e.has(Effect::WriteFs))
        out.push_back({"write files",       "create or modify files that outlive the run"});
    if (e.has(Effect::Net))
        out.push_back({"reach the network", "send and receive data over the internet"});
    if (e.has(Effect::Exec))
        out.push_back({"execute programs",  "run commands, including fetched packages"});
    return out;
}

// The consent screen. Everything here is derived: the skill contributes
// its name, description, source and declared effects — never a sentence.
void print_consent(const Skill& s) {
    std::printf("\n");
    std::printf("  install skill \"%s\"", s.name.c_str());
    if (!s.origin.empty()) std::printf(" from %s", s.origin.c_str());
    std::printf("?\n\n");

    if (!s.description.empty())
        std::printf("  %s\n\n", s.description.c_str());

    const auto lines = effect_lines(s.effects);
    if (lines.empty()) {
        std::printf("  declares no effects — prose only.\n");
    } else {
        std::printf("  the agent will be told it may:\n");
        for (const auto& l : lines)
            std::printf("    %-18s %s\n",
                        std::string{l.label}.c_str(),
                        std::string{l.gloss}.c_str());
    }
    std::printf("\n");

    // The sentence people most need and least expect.
    std::printf("  skills are instructions, not sandboxed code. agentty cannot\n");
    std::printf("  enforce what is written above — it is what the skill declared.\n");
    if (!s.origin.empty())
        std::printf("  you did not write these instructions.\n");
    std::printf("\n");
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Where `skill add` installs to: the user root, so it is available in
// every workspace and a cloned repo can't overwrite it.
fs::path install_dir_for(std::string_view name) {
    return util::user_root() / "skills" / std::string{name};
}

// ── add ────────────────────────────────────────────────────────────────────

int verb_add(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        std::fprintf(stderr, "usage: agentty skill add <github.com/user/repo | ./dir>\n");
        return 2;
    }
    const std::string src = argv[0];
    const bool force = std::ranges::find(argv, std::string{"--force"}) != argv.end();
    const bool yes   = std::ranges::find(argv, std::string{"--yes"})   != argv.end();

    // Local directory: copy it. Remote: not fetched here — agentty has no
    // business shelling out to git for this, and a network fetch inside a
    // CLI verb wants its own design (checkout pinning, cache, offline).
    // Declared here so the shape is right; the remote arm is the next
    // commit's job.
    fs::path from = fs::path{src};
    if (!fs::exists(from / "SKILL.md")) {
        if (src.starts_with("github.com/") || src.starts_with("https://")) {
            std::fprintf(stderr,
                "remote fetch isn't wired yet. clone it and add the path:\n"
                "  git clone https://%s /tmp/skill && agentty skill add /tmp/skill\n",
                src.starts_with("github.com/") ? src.c_str() : src.c_str());
            return 1;
        }
        std::fprintf(stderr, "no SKILL.md under %s\n", from.string().c_str());
        return 1;
    }

    // Parse BEFORE installing, so the consent prompt describes the real
    // file rather than a promise about it.
    const auto parsed = peek_skill(from / "SKILL.md");
    if (!parsed) {
        std::fprintf(stderr, "could not parse %s\n",
                     (from / "SKILL.md").string().c_str());
        return 1;
    }
    Skill s = *parsed;
    if (s.origin.empty() && (src.starts_with("github.com/")
                          || src.starts_with("https://")))
        s.origin = src;

    const auto dest = install_dir_for(s.name);
    if (fs::exists(dest / "SKILL.md") && !force) {
        // Never clobber a skill the user may have edited. (This rule came
        // from PR #47, which got it right and tested it.)
        std::fprintf(stderr,
            "skill \"%s\" already installed at %s\n"
            "re-run with --force to overwrite\n",
            s.name.c_str(), dest.string().c_str());
        return 1;
    }

    print_consent(s);

    if (needs_trust_gate(s.effects) && !yes) {
        std::printf("  [1] install   [2] print it first   [3] cancel\n\n> ");
        std::fflush(stdout);
        std::string answer;
        if (!std::getline(std::cin, answer)) return 1;
        if (answer == "2") {
            std::printf("\n%s\n", read_file(from / "SKILL.md").c_str());
            std::printf("  [1] install   [3] cancel\n\n> ");
            std::fflush(stdout);
            if (!std::getline(std::cin, answer)) return 1;
        }
        if (answer != "1") {
            std::printf("cancelled — nothing written\n");
            return 1;
        }
    }

    std::error_code ec;
    fs::create_directories(dest, ec);
    fs::copy(from, dest,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::fprintf(stderr, "install failed: %s\n", ec.message().c_str());
        return 1;
    }

    // Approve what we just showed — but key the approval off the skill as
    // the LOADER parses it, not off peek_skill's cheaper read. The two
    // trim the body differently, and an approval that doesn't match the
    // hash computed at load time is worse than none: the user answers a
    // prompt and the skill still shows PENDING forever.
    if (needs_trust_gate(s.effects)) {
        // all() re-scans when a root's mtime signature changes, which the
        // copy above just did — so find() sees the freshly installed file.
        const auto* installed = find(s.name);
        if (!installed) {
            std::fprintf(stderr,
                "installed, but %s did not load — run `agentty skills` to see why\n",
                s.name.c_str());
            return 1;
        }
        auto store = load_approvals();
        store.approve(content_sha_of(*installed));
        save_approvals(store);
    }

    std::printf("installed %s → %s\n", s.name.c_str(), dest.string().c_str());
    if (needs_trust_gate(s.effects))
        std::printf("approved this version; editing it will ask again\n");
    return 0;
}

// ── list ───────────────────────────────────────────────────────────────────

int verb_list() {
    const auto& all_skills = all();
    if (all_skills.empty()) {
        std::printf("no skills installed\n");
        return 0;
    }
    const auto store = load_approvals();
    for (const auto& s : all_skills) {
        const char* trust = "-";
        if (needs_trust_gate(s.effects)) {
            const auto t = trust_of(s, store);
            trust = std::holds_alternative<scope::Trusted>(t) ? "approved"
                  : std::holds_alternative<scope::Pending>(t) ? "PENDING"
                                                              : "blocked";
        }
        std::printf("%-24s %-8s %-10s %s\n",
                    s.name.c_str(), s.source.c_str(), trust,
                    effects_to_frontmatter(s.effects).c_str());
        if (!s.description.empty())
            std::printf("    %s\n", s.description.c_str());
        if (!s.origin.empty())
            std::printf("    from %s\n", s.origin.c_str());
    }
    return 0;
}

// ── remove ─────────────────────────────────────────────────────────────────

int verb_remove(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        std::fprintf(stderr, "usage: agentty skill remove <name>\n");
        return 2;
    }
    const auto dest = install_dir_for(argv[0]);
    if (!fs::exists(dest)) {
        std::fprintf(stderr, "not installed under %s: %s\n",
                     util::user_root().string().c_str(), argv[0].c_str());
        return 1;
    }
    std::error_code ec;
    fs::remove_all(dest, ec);
    if (ec) {
        std::fprintf(stderr, "remove failed: %s\n", ec.message().c_str());
        return 1;
    }
    std::printf("removed %s\n", argv[0].c_str());
    return 0;
}

// ── approve ────────────────────────────────────────────────────────────────

int verb_approve(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        std::fprintf(stderr, "usage: agentty skill approve <name>\n");
        return 2;
    }
    const auto* s = find(argv[0]);
    if (!s) {
        std::fprintf(stderr, "no such skill: %s\n", argv[0].c_str());
        return 1;
    }
    if (!needs_trust_gate(s->effects)) {
        std::printf("%s declares no effects — nothing to approve\n", argv[0].c_str());
        return 0;
    }
    print_consent(*s);
    auto store = load_approvals();
    store.approve(content_sha_of(*s));
    save_approvals(store);
    std::printf("approved %s\n", argv[0].c_str());
    return 0;
}

void usage() {
    std::printf(
        "usage: agentty skill <verb>\n\n"
        "  add <path> [--force] [--yes]   install a skill (shows what it declares)\n"
        "  list                           installed skills, effects, trust\n"
        "  remove <name>                  uninstall\n"
        "  approve <name>                 approve a pending skill after reading it\n\n"
        "a skill with no declared effects is prose and is never gated.\n");
}

} // namespace

// ── Public ─────────────────────────────────────────────────────────────────

scope::Approvals load_approvals() {
    return scope::load_approvals(kApprovalsLeaf);
}

void save_approvals(const scope::Approvals& a) {
    (void)scope::save_approvals(kApprovalsLeaf, a);
}

std::string content_sha_of(const Skill& s) {
    return scope::content_hash(s.body + "\n#effects:" + effects_to_frontmatter(s.effects));
}

scope::Trust trust_of(const Skill& s) noexcept {
    return trust_of(s, load_approvals());
}

int cli(const std::vector<std::string>& argv) {
    if (argv.empty()) { usage(); return 2; }
    const std::string verb = argv[0];
    const std::vector<std::string> tail(argv.begin() + 1, argv.end());

    if (verb == "add")     return verb_add(tail);
    if (verb == "list")    return verb_list();
    if (verb == "remove")  return verb_remove(tail);
    if (verb == "approve") return verb_approve(tail);

    usage();
    return 2;
}

} // namespace agentty::tools::skills
