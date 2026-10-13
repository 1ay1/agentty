// view() is a pure function of the Model. This asserts the one way that
// has actually been broken.
//
// A panel read `app::deps().load_settings()` to build its rows. The symptom
// is not a crash: the same Model paints two different frames depending on
// what the store happens to hold at call time. That also makes the frame
// cache unsound — visual_hash summarises the MODEL, so maya is entitled to
// skip a repaint whose hidden input changed.
//
// A grep is the right shape of test here: the property is "no view TU
// reaches the seam", which no amount of calling view() can demonstrate.
#include "agtest.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Strip // and /* */ so a mention in prose doesn't fail the test.
[[nodiscard]] std::string without_comments(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    bool line_c = false, block_c = false;
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (line_c) { if (src[i] == '\n') { line_c = false; out += '\n'; } continue; }
        if (block_c) { if (src.compare(i, 2, "*/") == 0) { block_c = false; ++i; } continue; }
        if (src.compare(i, 2, "//") == 0) { line_c = true; continue; }
        if (src.compare(i, 2, "/*") == 0) { block_c = true; ++i; continue; }
        out += src[i];
    }
    return out;
}

} // namespace

TEST_CASE("view: no view TU reads through the Deps seam") {
    const fs::path root = fs::path{AGENTTY_SRC_ROOT} / "src" / "runtime" / "view";
    REQUIRE(fs::exists(root));

    std::vector<std::string> offenders;
    for (const auto& e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file() || e.path().extension() != ".cpp") continue;
        std::ifstream f(e.path());
        const std::string src{std::istreambuf_iterator<char>(f),
                              std::istreambuf_iterator<char>()};
        const std::string code = without_comments(src);
        if (code.find("deps()") != std::string::npos)
            offenders.push_back(e.path().filename().string());
    }

    std::string msg = "view TUs must read the Model, not deps(): ";
    for (const auto& o : offenders) { msg += o; msg += ' '; }
    CHECK_MESSAGE(offenders.empty(), msg);
}
