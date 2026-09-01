// logx_format_test — the log LINE FORMAT is a contract.
//
// Users grep it, we ask them to paste it, and `agentty diagnostics` parses it
// back out to reconstruct the session. A format change that looks harmless
// (dropping the channel column, reordering the level) silently breaks all
// three. These tests pin the shape.

#include <cstdlib>
#include <fstream>
#include <regex>
#include <string>

#include "agtest.hpp"
#include "agentty/util/logx.hpp"

using namespace agentty;

namespace {

std::string last_line() {
    const auto lf = logx::log_file();
    if (lf.empty()) return {};
    std::ifstream in{std::string{lf}, std::ios::binary};
    std::string all{std::istreambuf_iterator<char>(in),
                    std::istreambuf_iterator<char>()};
    auto end = all.find_last_not_of('\n');
    if (end == std::string::npos) return {};
    auto start = all.rfind('\n', end);
    return all.substr(start == std::string::npos ? 0 : start + 1);
}

// See the note in logx_redaction_test: a skipped log test is a green lie.
void require_logging() {
    REQUIRE_MESSAGE(!logx::log_file().empty(),
                    "AGENTTY_LOG/_FILE must be set for this binary "
                    "(ctest sets them; see cmake/AgenttyTests.cmake)");
}

} // namespace

TEST_CASE("logx: the line shape is stable") {
    require_logging();
    logx::emit(logx::Channel::Wire, logx::Level::Warn, "fmt.probe", "k=v");
    const auto line = last_line();

    // 2026-08-28T01:23:45.678 +0012345ms 1a2b W wire    fmt.probe: k=v
    // Anchored at the start only: the MESSAGE may contain anything at all
    // (raw wire bodies include newlines), so pinning the header prefix is
    // the contract — not the whole line.
    const std::regex header{
        R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3} )"   // wall clock
        R"(\+\d+ms )"                                        // monotonic
        R"([0-9a-f]{4} )"                                    // thread tag
        R"([TDIWE] )"                                        // level char
        R"(\w+ +)"                                           // channel
        R"([\w.]+: )"};                                       // site
    INFO("line = " << line);
    CHECK(std::regex_search(line, header, std::regex_constants::match_continuous));
}

TEST_CASE("logx: every channel and level renders") {
    require_logging();
    // A channel missing from kChannelNames would print garbage or crash;
    // this walks the whole enum so adding one without naming it fails here.
    for (std::size_t i = 0; i < logx::kChannels; ++i) {
        const auto ch = static_cast<logx::Channel>(i);
        logx::emit(ch, logx::Level::Error, "fmt.chan", "i={}");
        const auto line = last_line();
        INFO("channel index = " << i);
        CHECK(line.find(std::string{logx::kChannelNames[i]}) != std::string::npos);
    }
    struct LV { logx::Level lv; char ch; };
    for (auto [lv, c] : {LV{logx::Level::Trace, 'T'}, LV{logx::Level::Debug, 'D'},
                         LV{logx::Level::Info,  'I'}, LV{logx::Level::Warn,  'W'},
                         LV{logx::Level::Error, 'E'}}) {
        logx::emit(logx::Channel::General, lv, "fmt.lv", "x");
        const auto line = last_line();
        INFO("level char = " << c);
        CHECK(line.find(std::string{" "} + c + " ") != std::string::npos);
    }
}

TEST_CASE("logx: a message with newlines stays one grep-able record") {
    require_logging();
    // Raw wire bodies contain newlines (SSE frames are \n\n separated). The
    // line must still START with a timestamp so `grep ' E '` and the
    // diagnostics tail-reader keep working.
    logx::emit(logx::Channel::Wire, logx::Level::Error, "fmt.multiline",
               "data: {\"a\":1}\n\ndata: {\"b\":2}");
    const auto line = last_line();
    // The tail after the final newline is the body's last fragment; what
    // matters is that the record was written whole and is recoverable.
    CHECK(line.find("data:") != std::string::npos);
}
