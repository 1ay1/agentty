// theme_discipline_test — agentty spells no colours of its own.
//
// THE RULE
// ========
// agentty has no UI layer. Every colour decision lives in maya, reached
// through the theme: agentty names a semantic TOKEN (ui::accent,
// ui::status_ok) or a ThemeSlot, and maya decides what that looks like at
// paint time.
//
// A literal anywhere in agentty breaks that in a way nothing else catches.
// It is evaluated once, with no theme in scope, so it silently pins that
// one element to whatever palette it was compiled with — and it keeps
// rendering, keeps looking fine on the developer's terminal, and only
// shows up when a user picks a scheme and part of the screen ignores them.
// Every round of this bug has been that shape.
//
// So it is checked mechanically, against the source, on every build.
#include "agtest.hpp"

#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// The ONE file allowed to spell colours: the semantic palette itself.
//
// palette.hpp is where agentty's tokens are defined, and each one is a
// theme-slot read (AGENTTY_THEME_SLOT) rather than a literal — so it is
// exempt from the scan but not from the rule.
const std::vector<std::string> kAllowed = {
    "include/agentty/runtime/view/palette.hpp",
};

// The one file allowed to write RAW SGR escapes.
//
// The code-block runner emits colour while maya is SUSPENDED — cooked tty,
// TUI torn down, no canvas or style pool to paint into — so it genuinely
// cannot go through the theme. It still honours maya's tier detection for
// whether to emit colour at all, which is the part a user can observe
// (NO_COLOR / TERM=dumb).
const std::vector<std::string> kAllowedRawSgr = {
    "src/runtime/app/update/code_blocks.cpp",
};

[[nodiscard]] bool listed(const std::vector<std::string>& list,
                          const std::string& rel) {
    for (const auto& a : list)
        if (rel == a) return true;
    return false;
}

[[nodiscard]] bool allowed(const std::string& rel) {
    return listed(kAllowed, rel);
}

// ── Is this channel read guarded? ───────────────────────────────────────
//
// `has_channels()` establishes a fact about a colour that holds for the
// REST OF THE ENCLOSING BLOCK, not for one line. A line-scoped check forced
// the guard marker onto the arithmetic line itself, which is unreadable
// (`... + bg.b())  // has_channels`), teaches the wrong lesson about how the
// predicate works, and quietly fails open the moment someone writes the
// natural thing:
//
//     if (!bg.has_channels()) return std::nullopt;
//     return 0.2126 * bg.r() + ...;        // flagged, though it is correct
//
// So track brace depth instead. A guard seen at depth d covers every line
// until depth drops back below d — which is exactly the scope in which the
// compiler agrees the fact still holds.
//
// This is a heuristic, not a parser: braces inside string literals or
// comments would skew the depth. It is deliberately BIASED TOWARD FALSE
// POSITIVES (a skew makes a guard expire early and flags a safe line),
// because the failure mode of this test is a nuisance, while the failure
// mode of missing a real hit is #45 shipping again.
class GuardScope {
public:
    // Feed every line of the file in order, BEFORE testing it.
    void observe(const std::string& line) {
        // A guard is CODE. Honouring one found in a COMMENT is not a
        // detail: every function near this hazard carries a comment
        // explaining it, so reading comments disarmed the scanner in
        // exactly the files that needed it most. maya's anim::lerp sat
        // under a comment headed "Why this checks has_channels()" and was
        // waved through with its guard deleted.
        const auto first = line.find_first_not_of(" \t");
        const bool comment =
            first != std::string::npos
            && (line.compare(first, 2, "//") == 0
                || line.compare(first, 2, "/*") == 0
                || line.compare(first, 1, "*") == 0);

        // has_channels() is the name, but a proven Kind::Rgb is the same
        // fact stated the long way, and the SGR emission paths use it
        // because they must switch on the kind anyway. Inside such a
        // branch, r()/g()/b() genuinely ARE channels.
        const bool establishes =
            line.find("has_channels") != std::string::npos
            || line.find("ColorKind::Rgb") != std::string::npos
            || line.find("Kind::Rgb") != std::string::npos;

        if (!comment && establishes) guarded_depth_ = depth_;
        if (comment) return;   // braces in prose are not scope, either

        for (char c : line) {
            if (c == '{') ++depth_;
            else if (c == '}') {
                --depth_;
                // The guard's block closed: the fact no longer holds.
                if (guarded_depth_ >= 0 && depth_ < guarded_depth_)
                    guarded_depth_ = -1;
            }
        }
    }

    [[nodiscard]] bool guarded() const noexcept { return guarded_depth_ >= 0; }

    void reset() noexcept { depth_ = 0; guarded_depth_ = -1; }

private:
    int depth_ = 0;
    int guarded_depth_ = -1;
};

}  // namespace

TEST_CASE("theme discipline: the guard tracker is block-scoped") {
    // The scanner is itself code, and a guard that silently stops guarding
    // is worse than none — it reads green forever. The line-scoped version
    // failed exactly that way: it accepted only a marker ON the arithmetic
    // line, so every correctly-written guard was a false positive, and the
    // obvious fix ("append // has_channels") taught the wrong model of how
    // the predicate works. Pin the scope rules so they cannot rot.
    auto scan = [](std::initializer_list<const char*> lines) {
        GuardScope s;
        std::vector<bool> guarded;
        for (const char* l : lines) {
            const std::string line{l};
            s.observe(line);
            guarded.push_back(s.guarded());
        }
        return guarded;
    };

    // The case that forced the ugly inline marker in ui_theme.hpp: a guard
    // must cover the REST OF ITS BLOCK, not just its own line.
    {
        const auto g = scan({
            "bool f(LitColor c) {",
            "    if (!c.has_channels()) return false;",
            "    return c.r() + c.g() > 10;",
            "}",
        });
        CHECK(!g[0], "no guard before has_channels is seen");
        CHECK(g[1],  "the guard line itself counts");
        CHECK(g[2],  "the guard must survive to the next line");
        CHECK(!g[3], "the guard expires when its block closes");
    }

    // A nested guard does not leak to its siblings.
    {
        const auto g = scan({
            "void f() {",
            "    if (x) {",
            "        if (c.has_channels()) {}",
            "    }",
            "    return c.r() * 2;",
            "}",
        });
        CHECK(g[2],  "guarded inside the nested block");
        CHECK(!g[4], "a sibling statement is NOT covered by it");
    }

    // The fail-open case: one function's guard must not cover the next.
    {
        const auto g = scan({
            "int a(LitColor c) {",
            "    if (!c.has_channels()) return 0;",
            "    return c.r() / 2;",
            "}",
            "int b(LitColor c) {",
            "    return c.r() / 2;",
            "}",
        });
        CHECK(g[2],  "the first function is guarded");
        CHECK(!g[5], "the next function must NOT inherit the guard");
    }

    // A guard in a COMMENT is not a guard. This one is load-bearing: every
    // function near this hazard documents it, so honouring comments
    // disarmed the scanner in precisely the files that needed it. maya's
    // anim::lerp sat under "// Why this checks has_channels()" and was
    // waved through with the guard itself deleted.
    {
        const auto g = scan({
            "// Why this checks has_channels() ...",
            "LitColor lerp(LitColor a, LitColor b, double t) {",
            "    return LitColor::rgb(mix(a.r(), b.r()));",
            "}",
        });
        CHECK(!g[0], "a comment mentioning has_channels establishes nothing");
        CHECK(!g[2], "so the body is UNGUARDED and must be flagged");
    }

    // A proven Kind::Rgb branch is the same fact stated the long way — the
    // SGR emission paths switch on the kind anyway, and inside such a
    // branch the bytes genuinely are channels.
    {
        const auto g = scan({
            "switch (c.kind()) {",
            "    case ColorKind::Rgb:",
            "        return write(p, c.r(), c.g(), c.b());",
            "}",
        });
        CHECK(g[1], "case ColorKind::Rgb establishes channels");
        CHECK(g[2], "and covers the emission inside it");
    }
}

TEST_CASE("theme discipline: agentty names tokens, never colours") {
    const fs::path root{AGENTTY_SRC_ROOT};
    REQUIRE(fs::exists(root));

    // A bare literal colour. Color::slot() is the point, and a computed
    // Color::rgb(expr) is arithmetic — both are fine. Hardcoded channel
    // values are not.
    const std::regex lit{
        R"(Color::(black|red|green|yellow|blue|magenta|cyan|white|bright_\w+)\(\))"
        R"(|Color::hex\(0x)"
        R"(|Color::rgb\(\s*[0-9])"
        R"(|Color::rgb\(\s*0x)"
        R"(|Color::hsl\(\s*[0-9])"
        R"(|Color::indexed\(\s*[0-9])"
        R"(|AnsiColor::)"
        // maya's literal Style presets are the same bet by another name.
        R"(|\b(fg|bg)_(black|red|green|yellow|blue|magenta|cyan|white)\b)"};

    // A hand-written SGR escape sidesteps Color entirely, so the regex above
    // cannot see it. This is the evasion that matters most: it is invisible
    // to every type-level guard AND to the theme, and it is how colour
    // creeps back in once the obvious route is closed.
    const std::regex raw_sgr{R"((\\x1b|\\033|\\e)\[[0-9;]*m)"};

    // Arithmetic on a colour's channel bytes.
    //
    // Resolving a slot yields a LitColor, which is PAINTABLE but not
    // necessarily NUMERIC: only Kind::Rgb carries channels. Named and
    // Indexed keep a PALETTE INDEX in the r byte with g/b zero, and Default
    // has nothing at all — and theme::native states every slot as Named or
    // Default deliberately, so the user's own palette reaches the screen.
    //
    // Multiplying or differencing those bytes therefore reads a palette
    // index as a colour channel. That is issue #45: bright_black (Named 8)
    // blended into rgb(8,0,0) and painted over every line of every
    // reasoning block — invisible on a dark terminal, and invisible to any
    // developer, because they all run a scheme rather than native.
    //
    // ANY channel read — not just one adjacent to an operator.
    //
    // The first version required a [-+*/] next to the read. That felt
    // precise and was useless: the original #45 bug passes its channels to
    // a FUNCTION — `LitColor::rgb(mix(a.r(), b.r()), ...)` — so no operator
    // is in sight and the scanner walked past the very line it was written
    // for. Flag every read; the two legitimate shapes (index() for palette
    // emission, a has_channels/Kind::Rgb branch for real arithmetic) are
    // already explicit and say so.
    const std::regex channel_read{R"(\.[rgb]\(\))"};

    std::vector<std::string> offenders;
    int scanned = 0;

    for (const fs::path& sub : {fs::path{"src"}, fs::path{"include"}}) {
        const fs::path dir = root / sub;
        if (!fs::exists(dir)) continue;
        for (const auto& e : fs::recursive_directory_iterator(dir)) {
            if (!e.is_regular_file()) continue;
            const auto ext = e.path().extension();
            if (ext != ".cpp" && ext != ".hpp") continue;

            const std::string rel =
                fs::relative(e.path(), root).generic_string();
            // NOTE: kAllowed exempts a file from the COLOUR LITERAL rule
            // only. It must not skip the whole file — an exemption earned
            // for one rule silently granting another is how a guard rots.
            // The channel check below applies everywhere, no exceptions:
            // palette.hpp reads theme slots, so it has colours in hand and
            // is exactly the kind of place a blend could appear.
            const bool lit_exempt = allowed(rel);
            ++scanned;

            std::ifstream in{e.path()};
            std::string line;
            int n = 0;
            GuardScope scope;
            while (std::getline(in, line)) {
                ++n;
                scope.observe(line);
                const auto first = line.find_first_not_of(" \t");
                if (first != std::string::npos
                    && (line.compare(first, 2, "//") == 0
                        || line.compare(first, 1, "*") == 0)) continue;
                if (!lit_exempt && std::regex_search(line, lit))
                    offenders.push_back(rel + ":" + std::to_string(n)
                                        + "  " + line);
                if (!listed(kAllowedRawSgr, rel)
                    && std::regex_search(line, raw_sgr))
                    offenders.push_back(rel + ":" + std::to_string(n)
                                        + "  (raw SGR)  " + line);
                if (std::regex_search(line, channel_read) && !scope.guarded())
                    offenders.push_back(rel + ":" + std::to_string(n)
                                        + "  (channel read on a colour that "
                                          "may have none)  " + line);
            }
        }
    }

    // Guard the guard: a walk that found nothing to scan would pass for
    // entirely the wrong reason.
    CHECK(scanned > 100);

    if (!offenders.empty()) {
        std::string msg = std::to_string(offenders.size())
            + " colour literal(s) in agentty. Colour belongs in maya:\n";
        for (std::size_t i = 0; i < offenders.size() && i < 25; ++i)
            msg += "   " + offenders[i] + "\n";
        msg += "\nUse a ui:: token (palette.hpp) or Color::slot(ThemeSlot::X)\n"
               "so the colour follows the user's theme.";
        FAIL_CHECK(msg);
    }
    CHECK(offenders.empty());
}
