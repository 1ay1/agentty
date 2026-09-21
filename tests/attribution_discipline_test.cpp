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
    const bool candidate = line.find(".begin()") != std::string::npos
                        || line.find(".back()")  != std::string::npos
                        || line.find(".front()") != std::string::npos;
    if (!candidate) return false;
    // Skip comments. This file's own rule is discussed in prose in the very
    // decoders it guards, and flagging an explanation of the bug as the bug
    // trains people to ignore the scanner.
    const auto first = line.find_first_not_of(" \t");
    if (first != std::string::npos && line.compare(first, 2, "//") == 0)
        return false;

    // `*thing.begin()` — an arbitrary element of an unordered container.
    static const std::regex deref_begin{
        R"(\*\s*[A-Za-z_][A-Za-z0-9_.>-]*\.begin\(\))"};
    if (std::regex_search(line, deref_begin)) return true;

    // `calls.back()` / `tool_slots.front()` — "the most recent call".
    //
    // Matched on COLLECTION-OF-CALLS names, not on anything containing
    // "tool". back()/front() are ordinary on strings, buffers and JSON
    // arrays — `tools_j.back()["cache_control"]` is building a request body,
    // not choosing an owner — and a scanner that fires on those gets
    // switched off, which is worse than not having one.
    //
    // The type layer already removes the INDEXING spellings
    // (ToolCallTracker::all() is not random-access), so this only has to
    // cover the residue: the plausible-sounding "latest" pick that would
    // otherwise walk past both layers.
    static const std::regex ends_pick{
        R"(\b(calls|tool_calls|tool_slots|slots|open_tools|open_tool_items)\s*\.(back|front)\(\))",
        std::regex::icase};
    return std::regex_search(line, ends_pick);
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

TEST_CASE("attribution: every transport states how it attributes") {
    // The scan above catches a decoder that picks arbitrarily. It cannot
    // catch a decoder that never faces the question because it keeps ONE
    // slot and routes everything there — which is what the Anthropic SSE
    // parser did for years (`current_tool_id`, every input_json_delta sent
    // to whichever block opened last).
    //
    // That code was safe, but safe BY LUCK: Claude emits content blocks one
    // at a time, so "the last one opened" happened to be right. A bet on a
    // server's emission order is not a property, and the losing side is one
    // call's arguments appended to another's — still valid JSON, dispatched
    // silently, wrong tool.
    //
    // So each transport must NAME its strategy, and the name has to be a
    // real mechanism. Three are legitimate:
    //
    //   keyed      — a per-call slot keyed on the wire's own identity
    //                (Chat's wire::ToolCallTracker, Responses' output_index
    //                + OpenCalls, Anthropic's content-block index)
    //   atomic     — the protocol delivers a whole call in one frame, so
    //                Start/Delta/End are emitted back-to-back and two calls
    //                are never in flight (Ollama's NDJSON message.tool_calls
    //                and its salvage paths)
    //
    // A transport with neither is the shape that keeps coming back.
    struct Transport {
        const char* path;
        const char* evidence;   // the token that proves the strategy
        const char* why;
    };
    const Transport kTransports[] = {
        {"openai/transport.cpp",   "ctx.tools.attribute",
         "Chat Completions interleaves parallel calls; keyed on (id, index) "
         "jointly via wire::ToolCallTracker, which returns Ambiguous rather "
         "than guessing."},
        {"responses/codec.cpp",    "addressed_item",
         "Responses addresses frames by output_index (a proxy cannot rewrite "
         "it without renumbering output[]), then a validated item_id, then "
         "OpenCalls::sole() which answers only when there is exactly one."},
        {"anthropic/sse.cpp",     "tool_at",
         "Anthropic puts an index on every content_block frame; that index "
         "is the block's identity for start, every delta, and stop."},
        {"ollama/transport.cpp",  "StreamToolUseEnd",
         "Ollama's NDJSON carries whole calls in one frame, so each is "
         "emitted Start->Delta->End atomically and nothing is ever in "
         "flight to misattribute."},
    };

    const fs::path root{AGENTTY_SRC_ROOT};
    for (const auto& t : kTransports) {
        const auto path = root / "src" / "provider" / t.path;
        INFO("transport = " << t.path);
        REQUIRE(fs::exists(path));

        std::ifstream in(path);
        REQUIRE(in);
        const std::string src((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());

        CHECK_MESSAGE(src.find(t.evidence) != std::string::npos,
            t.path << " no longer contains `" << t.evidence
                   << "`, the mechanism its attribution rests on. " << t.why
                   << " If the strategy changed, update this table with the "
                      "new mechanism — do not delete the row. A transport "
                      "with no named strategy is one that routes bytes "
                      "somewhere plausible, and plausible corruption still "
                      "parses.");
    }
}

TEST_CASE("attribution: no transport keeps a single current-call slot") {
    // The specific spelling of the Anthropic bug, banned by name.
    //
    // `current_tool_id` / `active_tool` / `last_tool` read as bookkeeping
    // and are in fact an attribution decision: every delta goes to that
    // one call. The scanner in this file would not flag it — there is no
    // `*begin()`, no `.back()`, nothing that looks like a pick. It looks
    // like a variable.
    static constexpr std::string_view kBanned[] = {
        "current_tool_id", "current_tool_name",
        "active_tool_id",  "last_tool_id",
        "latest_tool_item",
    };

    const fs::path root{AGENTTY_SRC_ROOT};
    const fs::path dir = root / "src" / "provider";
    REQUIRE(fs::exists(dir));

    std::vector<std::string> offenders;
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        const auto ext = e.path().extension();
        if (ext != ".cpp" && ext != ".hpp") continue;

        std::ifstream in(e.path());
        REQUIRE(in);
        std::string line;
        int lineno = 0;
        while (std::getline(in, line)) {
            ++lineno;
            // Comments explaining the ban are not violations of it — the
            // decoders document this rule, and flagging the explanation
            // trains people to ignore the scanner.
            const auto first = line.find_first_not_of(" \t");
            if (first != std::string::npos
                && line.compare(first, 2, "//") == 0) continue;
            if (exempted(line)) continue;
            for (auto banned : kBanned) {
                if (line.find(banned) == std::string::npos) continue;
                offenders.push_back(
                    fs::relative(e.path(), root).generic_string() + ":"
                    + std::to_string(lineno) + "  " + line);
            }
        }
    }

    for (const auto& o : offenders) MESSAGE(o);
    CHECK_MESSAGE(offenders.empty(),
        "a provider decoder keeps a single 'current tool' slot. That is an "
        "attribution decision wearing the clothes of bookkeeping: every "
        "delta goes to that one call, so the moment the server interleaves "
        "two, one call's arguments land on the other. It parses, it "
        "dispatches, and nothing notices. Key on the identity the wire "
        "already provides (content_block index, tool_calls[].index, "
        "output_index) and resolve through ONE named lookup.");
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

    // ── The residue the TYPE cannot remove ──
    //
    // ToolCallTracker::all() is deliberately not random-access, so `[i]` is
    // a compile error. back() is still legal on a bidirectional range, and
    // "the most recent call" is exactly the plausible-sounding spelling that
    // would otherwise slip through both layers.
    CHECK(looks_like_arbitrary_pick("    auto& c = tracker.calls.back();"));
    CHECK(looks_like_arbitrary_pick("    return calls.back().id;"));
    CHECK(looks_like_arbitrary_pick("    auto& s = tool_slots.front();"));

    // But NOT on the ordinary uses of back()/front(), which are everywhere
    // on strings and buffers. A scanner that fires on those gets switched
    // off, which is worse than not having one.
    CHECK(!looks_like_arbitrary_pick("    if (buf.back() == '\\n') buf.pop_back();"));
    CHECK(!looks_like_arbitrary_pick("    if (sv.front() != '{') return false;"));
    CHECK(!looks_like_arbitrary_pick("    tools_j.back()[\"cache_control\"] = x;"));

    // The exemption is honoured, and only with the marker.
    CHECK(exempted("  auto id = *s.begin();  // attribution-ok: size()==1 above"));
    CHECK(!exempted("  auto id = *s.begin();  // fine, probably"));
}
