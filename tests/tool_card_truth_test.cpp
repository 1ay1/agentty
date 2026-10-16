// tool_card_truth_test — every number a tool card shows must match what the
// tool actually returned.
//
// A card has a header ("Grep  pat  in  path · 1 match") and a body rendered by
// maya::ToolBodyPreview. Both are derived from the tool's text output, and
// several of them used to count the wrong thing: rows of context counted as
// matches ("… 23 more matches" under a 1-match grep), the file's total length
// shown for a 5-line read ("349 lines"), and so on. These cases feed the real
// output each tool produces through the real card code and check every count
// and line number that reaches the screen.

#include <string>

#include "agtest.hpp"

#include "agentty/domain/conversation.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_body_preview.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_helpers.hpp"
#include "maya/print.hpp"
#include "maya/widget/tool_body_preview.hpp"

namespace A = agentty;
namespace U = agentty::ui;
using Kind = maya::ToolBodyPreview::Kind;

namespace {

A::ToolUse done(std::string name, nlohmann::json args, std::string output) {
    static int seq = 0;
    A::ToolUse tc;
    tc.id = A::ToolCallId{"truth-" + std::to_string(++seq)};
    tc.name = A::ToolName{std::move(name)};
    tc.args = std::move(args);
    tc.status = A::ToolUse::Done{{}, {}, std::move(output)};
    return tc;
}

std::string body_text(const A::ToolUse& tc) {
    return maya::render_to_string(
        maya::ToolBodyPreview{U::tool_body_preview_config(tc)}.build(), 120);
}

bool has(const std::string& s, std::string_view needle) {
    return s.find(needle) != std::string::npos;
}

// grep with context: ONE match, shown with 15 lines of context either side.
const std::string kGrepOneMatchWithContext = [] {
    std::string s = "Found 1 match across 1 file.\n\n"
                    "## Matches in /w/process.cpp\n\n"
                    "### const std::string id = call.with(...) { \xe2\x80\xba L170-200\n"
                    "```\n";
    for (int i = 170; i <= 200; ++i) s += "    line " + std::to_string(i) + "\n";
    s += "```\n\nShowing all 1 matches.";
    return s;
}();

}  // namespace

TEST_CASE("card truth: grep header counts matches, not rows") {
    auto tc = done("grep", {{"pattern", "proc-"}, {"path", "/w/process.cpp"}},
                   kGrepOneMatchWithContext);
    const auto header = U::tool_timeline_detail(tc);
    INFO("header: " << header);
    CHECK(has(header, "1 match"));
    CHECK_FALSE(has(header, "31 match"));
}

TEST_CASE("card truth: a 1-match grep body never claims more matches") {
    auto tc = done("grep", {{"pattern", "proc-"}, {"path", "/w/process.cpp"}},
                   kGrepOneMatchWithContext);
    const auto body = body_text(tc);
    INFO("body:\n" << body);
    CHECK_FALSE(has(body, "more matches"));
}

TEST_CASE("card truth: a grep body's elision counts lines, and says so") {
    auto tc = done("grep", {{"pattern", "proc-"}, {"path", "/w/process.cpp"}},
                   kGrepOneMatchWithContext);
    const auto body = body_text(tc);
    INFO("body:\n" << body);
    // 31 rows of the block were returned; whatever is hidden is lines.
    if (has(body, "more"))
        CHECK(has(body, "more line"));
}

TEST_CASE("card truth: a ranged read's header names the range, not the file length") {
    std::string out = "    return a;\n}\n\nExecResult run_start(const Call& call) {\n"
                      "    // Make room by dropping sessions\n\n"
                      "[showing lines 131-135 of 349; 214 more \xe2\x80\x94 pass "
                      "offset=136 (or start_line=136) for the next chunk]";
    auto tc = done("read", {{"path", "/w/process.cpp"}, {"start_line", 131}, {"end_line", 135}},
                   out);
    const auto header = U::tool_timeline_detail(tc);
    INFO("header: " << header);
    // Showing "349 lines" for a 5-line read reads as "this card is 349 lines".
    CHECK(has(header, "131"));
    CHECK(has(header, "135"));
}

TEST_CASE("card truth: a ranged read's body gutter starts at the first line read") {
    std::string out = "    return a;\n}\n\nExecResult run_start(const Call& call) {\n"
                      "    // Make room by dropping sessions\n\n"
                      "[showing lines 131-135 of 349; 214 more \xe2\x80\x94 pass "
                      "offset=136 (or start_line=136) for the next chunk]";
    auto tc = done("read", {{"path", "/w/process.cpp"}, {"start_line", "131"}, {"end_line", 135}},
                   out);
    const auto body = body_text(tc);
    INFO("body:\n" << body);
    CHECK(has(body, "131"));
    CHECK(has(body, "return a;"));
    // The paging footer is the tool talking to the model, not file content.
    CHECK_FALSE(has(body, "offset=136"));
}

TEST_CASE("card truth: a cached read is labelled cached in the body, not shown as file text") {
    std::string out = "[cached \xe2\x80\x94 unchanged since your last read of this file]\n"
                      "line one\nline two\n";
    auto tc = done("read", {{"path", "/w/f.txt"}}, out);
    const auto body = body_text(tc);
    INFO("body:\n" << body);
    CHECK_FALSE(has(body, "1 \xe2\x94\x82 [cached"));
}

TEST_CASE("card truth: grep files/count modes report files and totals truthfully") {
    auto files = done("grep", {{"pattern", "x"}, {"path", "/w"}, {"output", "files"}},
                      "Found 8 matches across 4 files.\n\n"
                      "/w/a.hpp  (3 matches)\n/w/b.cpp  (2 matches)\n"
                      "/w/c.cpp  (2 matches)\n/w/d.cpp  (1 match)");
    const auto h = U::tool_timeline_detail(files);
    INFO("header: " << h);
    CHECK(has(h, "8 match"));
}

TEST_CASE("card truth: grep body shows the matched row, not leading context") {
    std::string out = "Found 1 match across 1 file.\n\n## Matches in /w/p.cpp\n\n### L170-200\n```\n";
    for (int i = 170; i <= 200; ++i)
        out += (i == 190 ? std::string{"    auto id = next_proc_id();"}
                         : "    line " + std::to_string(i)) + "\n";
    out += "```\n";
    auto tc = done("grep", {{"pattern", "next_proc_id"}, {"path", "/w"}}, out);
    const auto body = body_text(tc);
    INFO("body:\n" << body);
    CHECK(has(body, "190"));
    CHECK(has(body, "next_proc_id"));
    CHECK_FALSE(has(body, "line 170"));
}

// Most tools echo display_description as line 1 of their output. The card
// already shows it as the title; it must not break counts or become body text.

TEST_CASE("card truth: read_filter counts matching lines, not context rows") {
    auto tc = done("read_filter", {{"path", "src/a.cpp"}, {"pattern", "f1"}},
                   "src/a.cpp \xe2\x80\x94 2 matching lines of 60 (\xc2\xb12 context; gaps collapsed):\n\n"
                   "1\tint f0();\n2\tint f1();\n3\tint f2();\n\xe2\x8b\xaf 6 lines \xe2\x8b\xaf\n"
                   "10\tint f9();\n11\tint f10();\n12\tint f11();\n");
    const auto h = U::tool_timeline_detail(tc);
    INFO("header: " << h);
    CHECK(has(h, "2 matches"));
    CHECK_FALSE(has(h, "6"));
}

TEST_CASE("card truth: extract shows how many values it pulled") {
    auto tc = done("extract", {{"pattern", "f(\\d+)"}},
                   "Extracted 60 values.\n\n0\n1\n2\n");
    const auto h = U::tool_timeline_detail(tc);
    INFO("header: " << h);
    CHECK(has(h, "60 values"));
}

TEST_CASE("card truth: a capped glob says 500+, not a fake exact count") {
    auto tc = done("glob", {{"pattern", "*"}},
                   "Found more than 500 matches; showing the first 500 found (not sorted "
                   "by recency across all matches). Narrow the pattern or `path` to see the rest.\n"
                   "a.cpp  1K\n");
    const auto h = U::tool_timeline_detail(tc);
    INFO("header: " << h);
    CHECK(has(h, "500+"));
}
const nlohmann::json kDesc = {{"display_description", "Find the widgets"}};

A::ToolUse with_desc(std::string name, nlohmann::json args, std::string out) {
    for (auto& [k, v] : kDesc.items()) args[k] = v;
    return done(std::move(name), std::move(args), "Find the widgets\n" + out);
}

TEST_CASE("card truth: glob count survives a description echo") {
    auto tc = with_desc("glob", {{"pattern", "*.cpp"}},
                        "Found 3 file(s):\na.cpp  1K\nb.cpp  2K\nc.cpp  3K\n");
    const auto h = U::tool_timeline_detail(tc);
    INFO("header: " << h);
    CHECK(has(h, "3"));
    CHECK_FALSE(has(h, "no "));
    CHECK_FALSE(has(body_text(tc), "Find the widgets"));
}

TEST_CASE("card truth: grep count survives a description echo") {
    auto tc = with_desc("grep", {{"pattern", "proc-"}, {"path", "/w/process.cpp"}},
                        kGrepOneMatchWithContext);
    const auto h = U::tool_timeline_detail(tc);
    INFO("header: " << h);
    CHECK(has(h, "1 match"));
}

TEST_CASE("card truth: a read body never shows the description as line 1") {
    auto tc = with_desc("read", {{"path", "/w/f.txt"}}, "line one\nline two\n");
    const auto body = body_text(tc);
    INFO("body:\n" << body);
    CHECK_FALSE(has(body, "Find the widgets"));
    CHECK(has(body, "line one"));
}

TEST_CASE("card truth: shell exit code survives a description echo") {
    auto tc = with_desc("shell", {{"command", "make"}},
                        "Command \"make\" failed with exit code 2.\n```\nboom\n```");
    const auto h = U::tool_timeline_detail(tc);
    INFO("header: " << h);
    CHECK(has(h, "2"));
    CHECK_FALSE(has(body_text(tc), "Find the widgets"));
}

TEST_CASE("card truth: git_status on a repo with no commits names the branch") {
    auto tc = with_desc("git_status", nlohmann::json::object(),
                        "On branch master \xc2\xb7 2 staged\n"
                        "## No commits yet on master\nA  d.json\nA  src/a.cpp\n");
    const auto h = U::tool_timeline_detail(tc);
    INFO("header: " << h);
    CHECK(has(h, "master"));
    CHECK_FALSE(has(h, "No"));
    CHECK(has(h, "2"));
}
