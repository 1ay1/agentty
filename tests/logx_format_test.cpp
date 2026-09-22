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

TEST_CASE("logx: a site name quoted inside a payload is not a site field") {
    require_logging();
    // `agentty diagnostics` reconstructs the session by scanning the log for
    // the `startup` and `provider.select` events. It used to do that with a
    // bare find() over the whole line — which matched a REQUEST BODY that
    // happened to quote the string, and printed a chunk of the user's
    // conversation as the active provider. Observed in a real log.
    //
    // The distinguishing fact is POSITION: a site field lives in the line
    // header, a payload hit lives deep in a JSON blob. Pin that the header
    // stays short enough for the scanner's column bound to separate them.
    logx::emit(logx::Channel::Wire, logx::Level::Trace, "anthropic.request.body",
               "raw={{\"text\":\"provider.select: provider=bogus\"}}");
    const auto line = last_line();

    const auto at = line.find("provider.select");
    REQUIRE(at != std::string::npos);      // the payload really is in there
    CHECK(at > 64);                        // but far past the header

    // A genuine event puts its site inside the header window.
    logx::emit(logx::Channel::Wire, logx::Level::Warn, "provider.select",
               "provider=anthropic kind=anthropic");
    const auto real = last_line();
    const auto site = real.find("provider.select");
    REQUIRE(site != std::string::npos);
    CHECK(site <= 64);
    CHECK(real.compare(site + std::string_view{"provider.select"}.size(), 1, ":") == 0);
}

TEST_CASE("logx: a message with newlines stays one grep-able record") {
    require_logging();
    // Raw wire bodies contain newlines (SSE frames are \n\n separated).
    // They must not split the record: the file is line-oriented, and every
    // consumer — `grep ' E '`, the diagnostics session scanner, the user
    // pasting a line into an issue — assumes one event is one line.
    //
    // This used to assert only that the tail fragment was "recoverable",
    // which a split record technically satisfies. It isn't enough: a body
    // that wraps hides the error's level from grep, and a payload that
    // quotes a site name at the start of its own line is indistinguishable
    // from a real event (that is how diagnostics came to report a chunk of
    // conversation as the active provider).
    logx::emit(logx::Channel::Wire, logx::Level::Error, "fmt.multiline",
               "data: {{\"a\":1}}\n\ndata: {{\"b\":2}}");
    const auto line = last_line();

    // BOTH frames are on the ONE line the reader gets back.
    CHECK(line.find("\"a\":1") != std::string::npos);
    CHECK(line.find("\"b\":2") != std::string::npos);
    // And it still carries its header, so the level is greppable.
    CHECK(line.find(" E ") != std::string::npos);
    CHECK(line.find("fmt.multiline:") != std::string::npos);

    // A carriage return must not survive either — a stray \r makes the
    // line look truncated in a pager.
    logx::emit(logx::Channel::Wire, logx::Level::Error, "fmt.crlf",
               "one\r\ntwo");
    const auto crlf = last_line();
    CHECK(crlf.find('\r') == std::string::npos);
    CHECK(crlf.find("one") != std::string::npos);
    CHECK(crlf.find("two") != std::string::npos);
}
