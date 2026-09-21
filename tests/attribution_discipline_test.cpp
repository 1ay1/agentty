// attribution_discipline_test — a decoder may not invent a tool call's owner.
//
// ── THE RULE ─────────────────────────────────────────────────────────────
//
// When a streamed chunk does not say which tool call it belongs to, a
// decoder must FAIL rather than pick one.
//
// Picking produces valid JSON. Append one call's bytes to another and the
// result parses, passes schema validation, and dispatches — so `edit` runs
// with a path that belonged to `shell`, and nothing looks wrong until it has
// already written. Failing produces one visibly broken turn instead. That is
// strictly better, and it is the only outcome that stays honest when the
// decoder is wrong.
//
// ── WHY A SOURCE SCAN ────────────────────────────────────────────────────
//
// We shipped this bug four times across two decoders (see
// docs/TOOL_CALL_ATTRIBUTION.md). The last one survived review because the
// fallback was NAMED well: `latest_tool_item` reads as ordered, and was
// refreshed from `*open_tool_items.begin()` on an unordered_set — "latest"
// meant whichever item the hash happened to yield.
//
// A behavioural test cannot catch the next one, because the next one will be
// a new fallback with a new plausible name in a decoder that does not exist
// yet. What CAN be caught is the SHAPE: dereferencing the first element of
// an unordered container to stand in for an identity.
//
// So this scans the provider sources for that shape, the way
// theme_discipline_test scans for colour literals. It is a coarse net on
// purpose — the cost of a false positive is one `// attribution-ok:` comment
// explaining why a case is safe, and the cost of a miss is a tool running
// with another call's arguments.

#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

#include "agtest.hpp"

namespace fs = std::filesystem;

namespace {

// Reading the first element of an unordered container is order-undefined.
// Using it as an ANSWER ("which call is this?") is the bug; using it to
// drain a set is fine, so the scan looks for the dereference shape and lets
// a drain site annotate itself.
[[nodiscard]] bool looks_like_arbitrary_pick(const std::string& line) {
    // Cheap prefilter first: the regex below is the expensive part and the
    // overwhelming majority of lines contain none of these.
    if (line.find(".begin()") == std::string::npos) return false;
    // Skip comments. This file's own rule is discussed in prose in the very
    // decoders it guards, and flagging an explanation of the bug as the bug
    // trains people to ignore the scanner.
    const auto first = line.find_first_not_of(" \t");
    if (first != std::string::npos && line.compare(first, 2, "//") == 0)
        return false;
    static const std::regex pick{R"(\*\s*[A-Za-z_][A-Za-z0-9_.>-]*\.begin\(\))"};
    return std::regex_search(line, pick);
}

// An explicit, reviewed exemption. The comment must say WHY, so the next
// reader can judge it rather than trust it.
[[nodiscard]] bool exempted(const std::string& line) {
    return line.find("attribution-ok:") != std::string::npos;
}

}  // namespace

TEST_CASE("attribution: no decoder picks an arbitrary call as an owner") {
    const fs::path root{AGENTTY_SRC_ROOT};
    const fs::path dir = root / "src" / "provider";
    REQUIRE(fs::exists(dir));

    std::vector<std::string> offenders;
    int scanned = 0;

    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        const auto ext = e.path().extension();
        if (ext != ".cpp" && ext != ".hpp") continue;

        std::ifstream in(e.path());
        REQUIRE(in);
        ++scanned;

        std::string line;
        std::string prev;
        int lineno = 0;
        while (std::getline(in, line)) {
            ++lineno;
            if (!looks_like_arbitrary_pick(line)) { prev = line; continue; }
            // The marker may sit on the line itself or on the line above:
            // a real justification is usually a sentence, and a trailing
            // comment that long is worse to read than a preceding one.
            if (exempted(line) || exempted(prev)) { prev = line; continue; }
            offenders.push_back(
                fs::relative(e.path(), root).generic_string() + ":"
                + std::to_string(lineno) + "  " + line);
            prev = line;
        }
    }

    // Scanned something, or the test is vacuously green.
    CHECK(scanned > 5);

    for (const auto& o : offenders) MESSAGE(o);
    CHECK_MESSAGE(offenders.empty(),
        "a provider decoder dereferences the first element of an unordered "
        "container. If that value answers \"which tool call do these bytes "
        "belong to?\", it is a coin flip that corrupts one call with "
        "another's arguments — and the result still parses, so nothing "
        "notices until a tool runs. Return an explicit failure instead (see "
        "wire::ToolCallTracker / AttributionError::Ambiguous). If the site "
        "is genuinely safe — draining a set, or a single-element container "
        "checked above — append `// attribution-ok: <why>`.");
}

TEST_CASE("attribution: the scanner recognises the shape it exists for") {
    // A discipline test that cannot fail is decoration. These are the exact
    // lines from the two bugs, so a future rewrite of the matcher has to keep
    // catching them.
    CHECK(looks_like_arbitrary_pick(
        "        ctx.latest_tool_item = *ctx.open_tool_items.begin();"));
    CHECK(looks_like_arbitrary_pick(
        "    return *calls.begin();"));
    CHECK(looks_like_arbitrary_pick(
        "  auto id = * open_items.begin() ;"));

    // And does not fire on ordinary iteration, which is most `.begin()` uses.
    CHECK(!looks_like_arbitrary_pick(
        "    for (auto it = m.begin(); it != m.end(); ++it) {"));
    CHECK(!looks_like_arbitrary_pick(
        "    std::vector<std::string> ids(s.begin(), s.end());"));
    CHECK(!looks_like_arbitrary_pick(
        "    std::sort(v.begin(), v.end());"));
    // Nor on prose describing the bug — the decoders explain this rule in
    // comments, and flagging the explanation trains people to ignore the
    // scanner.
    CHECK(!looks_like_arbitrary_pick(
        "// it was refreshed from `*open_tool_items.begin()` on a set,"));

    // The exemption is honoured, and only with the marker.
    CHECK(exempted("  auto id = *s.begin();  // attribution-ok: size()==1 above"));
    CHECK(!exempted("  auto id = *s.begin();  // fine, probably"));
}
