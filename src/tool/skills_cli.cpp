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

#if defined(_WIN32)
#  include <io.h>
#else
#  include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace agentty::tools::skills {
namespace {

// ── Approvals store ────────────────────────────────────────────────────────

// Is stdin a terminal? agentty ships on Windows too, so this can't just be
// isatty() from <unistd.h>.
[[nodiscard]] bool stdin_is_tty() noexcept {
#if defined(_WIN32)
    return ::_isatty(::_fileno(stdin)) != 0;
#else
    return ::isatty(STDIN_FILENO) != 0;
#endif
}

// One size limit, used by BOTH the pre-install check and the copy. Two
// different limits is how add/list ended up disagreeing about whether a
// skill existed.
constexpr std::uintmax_t kMaxSkillFile = 2u * 1024 * 1024;   // 2 MB
constexpr std::uintmax_t kMaxSkillTotal = 8u * 1024 * 1024;  // 8 MB
constexpr int            kMaxSkillFiles = 128;

// A skill's `name:` is author-controlled, and `skill add` builds an
// INSTALL PATH out of it. That combination is a filesystem-write
// primitive unless the name is constrained, and it was: a skill declaring
//
//   name: ../../../../tmp/PWNED
//
// installed to ~/.agentty/skills/../../../../tmp/PWNED — outside the skills
// root, from a prose skill that never even prompts. An absolute name
// (`name: /etc/cron.d/x`) ignored the root entirely.
//
// So: one path component, no separators, no dots-only, conservative
// charset. Anything else is refused BY NAME rather than sanitised into
// something else — silently installing "foo" when the file said
// "../../foo" would be its own surprise.
[[nodiscard]] bool safe_component_impl(std::string_view n) {
    if (n.empty() || n.size() > 64) return false;
    if (n == "." || n == "..") return false;
    if (n.front() == '.' || n.front() == '-') return false;  // no dotfiles, no flag-lookalikes
    for (const char c : n) {
        const auto u = static_cast<unsigned char>(c);
        const bool ok = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z')
                     || (u >= '0' && u <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok) return false;   // rejects '/', '\\', NUL, spaces, UTF-8
    }
    return true;
}

// Belt and braces: even with a safe component, verify the resolved
// destination is genuinely inside the skills root before writing. Catches
// anything the charset check misses (symlinked root, odd platform
// semantics) — the same containment check the workspace boundary uses.
[[nodiscard]] bool inside_impl(const fs::path& root, const fs::path& child) {
    std::error_code ec;
    const auto r = fs::weakly_canonical(root, ec);
    if (ec) return false;
    const auto c = fs::weakly_canonical(child, ec);
    if (ec) return false;
    auto ri = r.begin(), rend = r.end();
    auto ci = c.begin(), cend = c.end();
    for (; ri != rend; ++ri, ++ci) {
        if (ci == cend || *ci != *ri) return false;
    }
    return true;
}

// Describe a SKILL.md before installing it, using THE parser — not a
// second one. This briefly had its own mini-parser and it diverged
// immediately: block scalars (`description: |`) rendered as a bare "|" in
// the consent prompt while loading fine afterwards, so the user approved
// a description they never actually saw.
std::optional<Skill> peek_skill(const fs::path& md) {
    // Size-check BEFORE reading. A 10 MB SKILL.md used to be parsed and
    // screened (~2s of work the user waits on with no output) and then
    // installed — while the LOADER silently drops oversized files, so
    // `skill list` then said "no skills installed" and add/list
    // contradicted each other. Refuse early, at the same limit the copy
    // enforces, so there is one answer.
    std::error_code ec;
    const auto sz = fs::file_size(md, ec);
    if (!ec && sz > kMaxSkillFile) return std::nullopt;

    std::ifstream in(md, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    auto s = parse_skill_text(ss.str(),
                             md.parent_path().filename().string(),
                             "user");
    if (s.name.empty()) return std::nullopt;
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
// its name, description, source and declared effects — never a sentence,
// and its description is SANITISED before it is shown (91% of confirmed
// malicious skills carry injection; a description that can open a new line
// can impersonate agentty's own framing).
//
// The screen is deliberately NOT uniform. Anthropic measured 93% approval
// on Claude Code permission prompts; a screen that looks the same every
// time is one people learn to dismiss. So a clean prose skill gets a quiet
// two-line note, a clean effectful skill gets the routine prompt, and a
// skill with findings gets a visually different screen with the default
// answer flipped. Differentiation is the only intervention the habituation
// literature found durable.
void print_consent(const Skill& s, const std::vector<Finding>& findings) {
    const bool critical = std::ranges::any_of(findings, [](const Finding& f) {
        return f.severity == Finding::Severity::Critical;
    });

    std::printf("\n");
    if (critical) {
        // Different shape, different words, different default. A user who
        // has approved twenty skills should FEEL that this one is not those.
        std::printf("  ━━━ REVIEW THIS ONE ━━━\n\n");
    }

    std::printf("  install skill \"%s\"",
                sanitize_author_text(s.name, 64).c_str());
    if (!s.origin.empty())
        std::printf(" from %s", sanitize_author_text(s.origin, 96).c_str());
    std::printf("?\n\n");

    if (!s.description.empty())
        std::printf("  %s\n\n", sanitize_author_text(s.description).c_str());

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

    // Findings, if any. These are agentty's words about what it saw in the
    // body — the skill gets no say in this section.
    if (!findings.empty()) {
        std::printf("  reading the file, agentty noticed:\n");
        for (const auto& f : findings) {
            const char* mark = f.severity == Finding::Severity::Critical ? "!!"
                             : f.severity == Finding::Severity::Warn     ? " !"
                                                                         : "  ";
            if (f.line > 0)
                std::printf("   %s line %-4d %s\n", mark, f.line, f.detail.c_str());
            else
                std::printf("   %s           %s\n", mark, f.detail.c_str());
        }
        std::printf("\n");

        // The honest caveat. Snyk's own scanners miss things; saying
        // "scanned, looks fine" would be the harmful sentence here.
        std::printf("  this is pattern matching over text, not proof of intent —\n");
        std::printf("  and a skill with no findings has not been proven safe.\n\n");
    }

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

// A skill directory is data, not a tree to traverse. Copying it with
// `recursive` followed symlinks: a `dir-escape -> /tmp` symlink made the
// copy walk into /tmp, and a `leak.txt -> /etc/passwd` symlink would have
// copied the target's CONTENT into the user's skills directory. Neither is
// something a skill install should be able to do.
//
// Copy plain files only, one level of real subdirectory, skip symlinks
// entirely, and cap the total — a skill is text plus a few resources.
struct CopyResult {
    bool ok = false;
    std::string error;
    int skipped_links = 0;
};

CopyResult copy_skill_tree(const fs::path& from, const fs::path& to) {
    CopyResult out;
    std::error_code ec;
    fs::create_directories(to, ec);
    if (ec) { out.error = ec.message(); return out; }

    std::uintmax_t total = 0;
    int files = 0;

    // follow_directory_symlink is OFF, and every entry is re-checked with
    // is_symlink() because the recursive iterator can still hand back link
    // entries themselves.
    fs::recursive_directory_iterator it(
        from, fs::directory_options::skip_permission_denied, ec);
    if (ec) { out.error = ec.message(); return out; }

    for (const auto& e : it) {
        const auto rel = fs::relative(e.path(), from, ec);
        if (ec) continue;

        if (fs::is_symlink(e.symlink_status())) { ++out.skipped_links; continue; }

        if (e.is_directory()) {
            fs::create_directories(to / rel, ec);
            continue;
        }
        if (!e.is_regular_file()) continue;     // fifo, socket, device: not data

        const auto sz = e.file_size(ec);
        if (ec) continue;
        if (sz > kMaxSkillFile) {
            out.error = rel.string() + " is " + std::to_string(sz / 1024)
                      + " KB — over the 2 MB per-file limit";
            return out;
        }
        total += sz;
        if (++files > kMaxSkillFiles || total > kMaxSkillTotal) {
            out.error = "skill is too large (limit: 128 files, 8 MB total)";
            return out;
        }

        fs::create_directories((to / rel).parent_path(), ec);
        fs::copy_file(e.path(), to / rel,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            out.error = "copying " + rel.string() + ": " + ec.message();
            return out;
        }
    }

    out.ok = true;
    return out;
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
        std::error_code sz_ec;
        const auto sz = fs::file_size(from / "SKILL.md", sz_ec);
        if (!sz_ec && sz > kMaxSkillFile) {
            std::fprintf(stderr,
                "refusing to install: SKILL.md is %ju KB, over the 2 MB limit.\n"
                "a skill is instructions — if it needs more than that, it "
                "probably wants to be a plugin.\n",
                static_cast<std::uintmax_t>(sz / 1024));
            return 1;
        }
        std::fprintf(stderr, "could not parse %s\n",
                     (from / "SKILL.md").string().c_str());
        return 1;
    }
    Skill s = *parsed;

    // An empty or bodyless SKILL.md installs cleanly and then does nothing
    // — it occupies a catalog slot and tokens on every turn to say nothing
    // at all. Almost always a mistake (wrong path, truncated download).
    if (s.body.empty() && s.description.empty()) {
        std::fprintf(stderr,
            "refusing to install: %s has no body and no description.\n"
            "an empty skill costs catalog space and tokens for nothing — "
            "check the path?\n",
            (from / "SKILL.md").string().c_str());
        return 1;
    }

    if (s.origin.empty() && (src.starts_with("github.com/")
                          || src.starts_with("https://")))
        s.origin = src;

    // The name becomes a PATH. Refuse anything that isn't one plain
    // component before it can be used for one — see safe_component().
    if (!safe_skill_name(s.name)) {
        std::fprintf(stderr,
            "refusing to install: the skill's name is not a plain directory\n"
            "name (letters, digits, - and _). it said:\n  %s\n",
            sanitize_author_text(s.name, 120).c_str());
        return 1;
    }

    const auto dest = install_dir_for(s.name);

    // Second, independent check: the resolved destination must actually be
    // under the skills root. Cheap, and it doesn't rely on the charset
    // rule above being exhaustive.
    const auto skills_root = util::user_root() / "skills";
    if (!path_inside(skills_root, dest)) {
        std::fprintf(stderr,
            "refusing to install: destination escapes %s\n",
            skills_root.string().c_str());
        return 1;
    }

    if (fs::exists(dest / "SKILL.md") && !force) {
        // Never clobber a skill the user may have edited. (This rule came
        // from PR #47, which got it right and tested it.)
        std::fprintf(stderr,
            "skill \"%s\" already installed at %s\n"
            "re-run with --force to overwrite\n",
            s.name.c_str(), dest.string().c_str());
        return 1;
    }

    // Screen the BODY, not the frontmatter. The arXiv corpus found shadow
    // features (capability present in code, absent from the docs) in 100%
    // of advanced attacks — so a skill's own account of itself is the least
    // reliable input available, and a declares-nothing/does-everything
    // mismatch is itself the signal.
    const auto findings = screen_body(s.body);
    const bool critical = std::ranges::any_of(findings, [](const Finding& f) {
        return f.severity == Finding::Severity::Critical;
    });

    print_consent(s, findings);

    // --yes is for scripts, and it stops at critical findings. An
    // unattended flag that silently installs something carrying an
    // instruction-override or a persistence payload is the flag doing harm
    // on the user's behalf; make automation say so explicitly.
    if (critical && yes) {
        std::fprintf(stderr,
            "refusing --yes: this skill has critical findings. install it\n"
            "interactively, or re-run with --i-have-read-this if you have.\n");
        const bool read_it = std::ranges::find(
            argv, std::string{"--i-have-read-this"}) != argv.end();
        if (!read_it) return 1;
    }

    if ((needs_trust_gate(s.effects) || critical) && !yes) {
        // No TTY means nobody can answer. Printing a prompt into a pipe
        // and reading EOF used to exit 0 having installed NOTHING — a
        // silent no-op that reported success, which in CI reads as "the
        // skill is there" right up until it isn't.
        if (!stdin_is_tty()) {
            std::fprintf(stderr,
                "this skill needs approval and there is no terminal to ask.\n"
                "run it interactively, or pass --yes if you have already "
                "reviewed it.\n");
            return 1;
        }

        // Findings flip the default. Routine installs lead with [1] install;
        // a flagged one leads with reading the file, and the safe answer is
        // the first thing your eye lands on.
        if (critical)
            std::printf("  [2] print it first   [3] cancel   [1] install anyway\n\n> ");
        else
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

    // Copy plain files only — no symlinks, no device nodes, size-capped.
    // This used to be fs::copy(recursive), which followed a
    // `dir-escape -> /tmp` symlink into /tmp and, when it errored, left a
    // half-populated directory while still printing success and exiting 0.
    const auto copied = copy_skill_tree(from, dest);
    if (!copied.ok) {
        std::fprintf(stderr, "install failed: %s\n", copied.error.c_str());
        std::error_code rm;
        fs::remove_all(dest, rm);   // don't leave a half-skill behind
        return 1;
    }

    // A skill whose SKILL.md didn't make it is not installed, whatever the
    // copy said. Verifying the one file that matters turns a silent
    // half-failure into an error the user can act on.
    if (!fs::exists(dest / "SKILL.md")) {
        std::fprintf(stderr, "install failed: SKILL.md did not copy\n");
        std::error_code rm;
        fs::remove_all(dest, rm);
        return 1;
    }

    if (copied.skipped_links > 0)
        std::printf("skipped %d symlink%s — skills are copied as plain files\n",
                    copied.skipped_links,
                    copied.skipped_links == 1 ? "" : "s");

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
    // Same rule as install: a name is one plain component. `remove` was
    // saved from traversal only by an existence check, which is luck
    // rather than design — `remove ../../something-that-exists` would have
    // been a recursive delete outside the skills root.
    if (!safe_skill_name(argv[0])) {
        std::fprintf(stderr, "not a skill name: %s\n",
                     sanitize_author_text(argv[0], 120).c_str());
        return 1;
    }
    const auto dest = install_dir_for(argv[0]);
    const auto skills_root = util::user_root() / "skills";
    if (!path_inside(skills_root, dest)) {
        std::fprintf(stderr, "refusing: path escapes %s\n",
                     skills_root.string().c_str());
        return 1;
    }
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
    print_consent(*s, screen_body(s->body));
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

bool safe_skill_name(std::string_view name) noexcept {
    return safe_component_impl(name);
}

bool path_inside(const std::filesystem::path& root,
                 const std::filesystem::path& child) noexcept {
    return inside_impl(root, child);
}


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
