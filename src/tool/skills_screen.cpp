// agentty::tools::skills — untrusted-text sanitising and injection screening.
//
// WHAT THE RESEARCH SAID, AND WHAT IT CHANGED HERE
//
// Three findings from the 2026 literature drove this file:
//
// 1. Snyk (Feb 2026, 3,984 skills from ClawHub/skills.sh): 13.4% carried a
//    CRITICAL issue; of the confirmed-malicious set, 100% contained
//    malicious code and 91% ALSO used prompt injection. The combination is
//    the point — the injection primes the agent to accept the payload that
//    its own safety training would otherwise refuse.
//
// 2. arXiv 2602.06547 (98,380 skills, 157 behaviourally confirmed): attacks
//    average 4.03 vulnerabilities across a median of 3 kill-chain phases.
//    "Shadow features" — capability absent from the documentation — appear
//    in 0% of basic attacks and 100% of advanced ones. So a skill's own
//    description of itself is the LEAST reliable signal available.
//
// 3. Anthropic (Mar 2026): Claude Code users approve 93% of permission
//    prompts. Akhawe & Felt measured 70% clickthrough on Chrome SSL
//    warnings. A uniform prompt trains the user to dismiss it.
//
// (3) is why this file exists at all. If every install shows the same
// screen, the screen is decoration. Screening lets the common case stay
// quiet and the rare dangerous case look DIFFERENT — polymorphic warnings
// are the one intervention the habituation literature found to durably
// hold attention (Anderson et al., BYU, fMRI + field study).
//
// (2) is why screening runs on the BODY and never trusts frontmatter: a
// skill that declares `effects: []` and then pipes curl into sh is exactly
// the shadow-feature case, and the mismatch is itself a finding.
//
// LIMITS, STATED HONESTLY
// This is a review aid. It is regex-and-heuristics over text, so it is
// evadable by aliases, wrapper scripts, encoded payloads, and any pattern
// nobody has published yet. Datadog's own guidance on the equivalent greps:
// treat matches as review leads and a clean result as ONE piece of
// evidence. agentty's wording follows that and never says "safe".

#include "agentty/tool/skills.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace agentty::tools::skills {
namespace {

bool contains_ci(std::string_view hay, std::string_view needle) {
    if (needle.size() > hay.size()) return false;
    const auto it = std::search(
        hay.begin(), hay.end(), needle.begin(), needle.end(),
        [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        });
    return it != hay.end();
}

// Unicode smuggling: bidi overrides and zero-width characters let a skill
// render one thing to a human and another to the parser. Documented in the
// arXiv study as pattern P2 (hidden instructions).
struct Smuggle { std::string_view utf8; std::string_view what; };
constexpr std::array<Smuggle, 9> kSmuggle{{
    {"\xE2\x80\xAE", "right-to-left override (U+202E)"},
    {"\xE2\x80\xAD", "left-to-right override (U+202D)"},
    {"\xE2\x80\xAA", "left-to-right embedding (U+202A)"},
    {"\xE2\x80\xAB", "right-to-left embedding (U+202B)"},
    {"\xE2\x81\xA6", "first-strong isolate (U+2066)"},
    {"\xE2\x80\x8B", "zero-width space (U+200B)"},
    {"\xE2\x80\x8C", "zero-width non-joiner (U+200C)"},
    {"\xE2\x80\x8D", "zero-width joiner (U+200D)"},
    {"\xEF\xBB\xBF", "zero-width no-break space (U+FEFF)"},
}};

void add(std::vector<Finding>& out, Finding::Severity sev,
         std::string code, std::string detail, int line) {
    // One finding per code — a skill with twelve curl|sh lines is one
    // problem, not twelve. Keeps the prompt readable, which is the whole
    // reason screening exists.
    for (const auto& f : out)
        if (f.code == code) return;
    out.push_back(Finding{sev, std::move(code), std::move(detail), line});
}

} // namespace

std::string sanitize_author_text(std::string_view raw, std::size_t max_len) {
    std::string out;
    out.reserve(std::min(raw.size(), max_len + 1));

    std::size_t i = 0;
    bool last_was_space = false;
    while (i < raw.size() && out.size() < max_len) {
        // Drop smuggling codepoints outright rather than rendering them.
        bool skipped = false;
        for (const auto& s : kSmuggle) {
            if (raw.compare(i, s.utf8.size(), s.utf8) == 0) {
                i += s.utf8.size();
                skipped = true;
                break;
            }
        }
        if (skipped) continue;

        const unsigned char c = static_cast<unsigned char>(raw[i]);

        // Newlines and control characters collapse to a single space: a
        // description must never be able to open a new "line" in the
        // prompt and impersonate agentty's own framing.
        if (c < 0x20 || c == 0x7F) {
            if (!last_was_space && !out.empty()) { out += ' '; last_was_space = true; }
            ++i;
            continue;
        }

        // Copy a WHOLE UTF-8 sequence or none of it. The byte-at-a-time
        // version split multibyte characters at the cap — a description of
        // emoji or CJK truncated to invalid bytes, which then went into
        // the model's system prompt and the TUI. Verified: a run of U+1F680
        // capped at 10 produced a dangling lead byte.
        std::size_t len = 1;
        if      ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else if (c >= 0x80)          { ++i; continue; }  // stray continuation

        if (i + len > raw.size()) break;                 // truncated input
        // Continuation bytes must actually be continuations; if not, the
        // input is malformed and the byte is dropped rather than copied.
        bool well_formed = true;
        for (std::size_t k = 1; k < len; ++k)
            if ((static_cast<unsigned char>(raw[i + k]) & 0xC0) != 0x80)
                well_formed = false;
        if (!well_formed) { ++i; continue; }

        // Stop BEFORE the cap rather than straddling it.
        if (out.size() + len > max_len) break;

        last_was_space = (len == 1 && c == ' ');
        out.append(raw, i, len);
        i += len;
    }

    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (i < raw.size()) out += "\xE2\x80\xA6";     // … truncated, and says so
    return out;
}

std::vector<Finding> screen_body(std::string_view body) {
    std::vector<Finding> out;
    using S = Finding::Severity;

    // Whole-file checks first (line 0).
    for (const auto& s : kSmuggle) {
        if (body.find(s.utf8) != std::string_view::npos) {
            add(out, S::Critical, "unicode-smuggling",
                std::string{"contains "} + std::string{s.what} +
                " — text that renders differently than it parses", 0);
            break;
        }
    }

    // Line-wise checks. Cheap, and gives the user a line number to jump to.
    int line = 0;
    std::size_t pos = 0;
    while (pos <= body.size()) {
        const auto nl = body.find('\n', pos);
        const auto l = body.substr(pos, (nl == std::string_view::npos ? body.size() : nl) - pos);
        ++line;

        // ── Remote code execution (Snyk SC2, Datadog grep #1) ──
        if ((contains_ci(l, "curl") || contains_ci(l, "wget")) &&
            (contains_ci(l, "| sh") || contains_ci(l, "|sh") ||
             contains_ci(l, "| bash") || contains_ci(l, "|bash") ||
             contains_ci(l, "| source") || contains_ci(l, "iex"))) {
            add(out, S::Critical, "curl-pipe-shell",
                "downloads a script and pipes it straight into a shell — "
                "the code that runs is whatever the server sends today", line);
        }

        // ── Obfuscated execution (Snyk SC3) ──
        if ((contains_ci(l, "base64 -d") || contains_ci(l, "base64 --decode") ||
             contains_ci(l, "atob(") || contains_ci(l, "from_base64")) &&
            (contains_ci(l, "eval") || contains_ci(l, "exec") ||
             contains_ci(l, "| sh") || contains_ci(l, "| bash"))) {
            add(out, S::Critical, "obfuscated-exec",
                "decodes base64 and executes the result — the real command "
                "is hidden from anyone reading this file", line);
        }

        // ── Credential access (Snyk E2, arXiv Credential Access phase) ──
        if (contains_ci(l, "gh auth token") || contains_ci(l, "~/.aws/credentials") ||
            contains_ci(l, ".ssh/id_") || contains_ci(l, "~/.netrc") ||
            contains_ci(l, "GITHUB_TOKEN") || contains_ci(l, "AWS_SECRET") ||
            contains_ci(l, "printenv") || contains_ci(l, "process.env") ||
            contains_ci(l, "os.environ")) {
            add(out, S::Warn, "reads-credentials",
                "reads credentials or environment secrets", line);
        }

        // ── Exfiltration: credential access + outbound in one skill ──
        if ((contains_ci(l, "curl") || contains_ci(l, "wget") ||
             contains_ci(l, "fetch(") || contains_ci(l, "requests.post")) &&
            (contains_ci(l, "-d ") || contains_ci(l, "--data") ||
             contains_ci(l, "POST"))) {
            add(out, S::Warn, "posts-data-outbound",
                "sends data to a remote endpoint", line);
        }

        // ── Instruction override (arXiv P1, Snyk prompt-injection) ──
        if (contains_ci(l, "ignore previous instruction") ||
            contains_ci(l, "ignore all previous") ||
            contains_ci(l, "disregard the above") ||
            contains_ci(l, "developer mode") ||
            contains_ci(l, "you are now") ||
            contains_ci(l, "system:") ||
            contains_ci(l, "security warnings are")) {
            add(out, S::Critical, "instruction-override",
                "tries to override the agent's own instructions — a skill "
                "should describe a task, not reprogram the agent", line);
        }

        // ── Hidden directives in HTML comments (arXiv P2) ──
        if (contains_ci(l, "<!--") &&
            (contains_ci(l, "instruction") || contains_ci(l, "ignore") ||
             contains_ci(l, "secret") || contains_ci(l, "token") ||
             contains_ci(l, "do not tell") || contains_ci(l, "don't tell"))) {
            add(out, S::Critical, "hidden-directive",
                "puts instructions inside an HTML comment — invisible when "
                "the file is rendered, visible to the agent", line);
        }

        // ── Concealment from the user (arXiv Defense Evasion) ──
        if (contains_ci(l, "do not tell the user") ||
            contains_ci(l, "don't tell the user") ||
            contains_ci(l, "without informing") ||
            contains_ci(l, "silently") ||
            contains_ci(l, "without mentioning")) {
            add(out, S::Critical, "conceal-from-user",
                "instructs the agent to hide what it is doing from you", line);
        }

        // ── Persistence (arXiv Impact phase) ──
        if (contains_ci(l, "crontab") || contains_ci(l, "systemctl enable") ||
            contains_ci(l, "launchctl load") ||
            contains_ci(l, "authorized_keys") ||
            contains_ci(l, ">> ~/.bashrc") || contains_ci(l, ">> ~/.zshrc") ||
            contains_ci(l, ">> ~/.profile")) {
            add(out, S::Critical, "persistence",
                "installs something that keeps running after this session — "
                "cron, a service, an SSH key, or a shell profile edit", line);
        }

        // ── Destructive ──
        if (contains_ci(l, "rm -rf /") || contains_ci(l, "rm -rf ~") ||
            contains_ci(l, "git push --force") || contains_ci(l, "git push -f") ||
            contains_ci(l, "DROP TABLE") || contains_ci(l, "mkfs")) {
            add(out, S::Warn, "destructive",
                "contains an irreversible or destructive command", line);
        }

        // ── Password-protected archives (Snyk: AV/scanner evasion) ──
        if ((contains_ci(l, "unzip") || contains_ci(l, "7z")) &&
            (contains_ci(l, "-P ") || contains_ci(l, "-p"))) {
            add(out, S::Warn, "encrypted-archive",
                "extracts a password-protected archive — a known way to get "
                "a payload past scanners", line);
        }

        // ── Broad pre-approval (Datadog grep #2) ──
        if (contains_ci(l, "allowed-tools") &&
            (contains_ci(l, "Bash(*)") || contains_ci(l, "bash(*)"))) {
            add(out, S::Critical, "blanket-shell",
                "asks to pre-approve unrestricted shell access", line);
        }

        // ── agentty does NOT implement `!`-dynamic context. If a skill was
        // written expecting it (ported from Claude Code), say so: the user
        // should know those lines are inert here, and that the skill was
        // authored for a tool where they would have run BEFORE any review.
        if (l.find("!`") != std::string_view::npos) {
            add(out, S::Note, "dynamic-context",
                "uses Claude Code's !`cmd` dynamic context, which agentty "
                "does not execute — these lines are inert here", line);
        }

        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }

    std::ranges::stable_sort(out, [](const Finding& a, const Finding& b) {
        if (a.severity != b.severity) return a.severity > b.severity;
        return a.line < b.line;
    });
    return out;
}

} // namespace agentty::tools::skills
