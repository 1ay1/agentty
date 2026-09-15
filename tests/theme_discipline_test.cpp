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

[[nodiscard]] bool allowed(const std::string& rel) {
    for (const auto& a : kAllowed)
        if (rel == a) return true;
    return false;
}

}  // namespace

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
        R"(|Color::indexed\(\s*[0-9])"};

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
            if (allowed(rel)) continue;
            ++scanned;

            std::ifstream in{e.path()};
            std::string line;
            int n = 0;
            while (std::getline(in, line)) {
                ++n;
                const auto first = line.find_first_not_of(" \t");
                if (first != std::string::npos
                    && (line.compare(first, 2, "//") == 0
                        || line.compare(first, 1, "*") == 0)) continue;
                if (std::regex_search(line, lit))
                    offenders.push_back(rel + ":" + std::to_string(n)
                                        + "  " + line);
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
