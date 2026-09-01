// logx_redaction_test — the log must be SAFE TO SEND and COMPLETE.
//
// Two properties, both learned the hard way:
//
//  1. NOT TRUNCATED. The formatter used to share the flight recorder's fixed
//     384-byte slot, so `wire=trace` — documented as "verbatim and
//     untruncated" — silently dropped ~90% of every request body. A trace
//     that looks complete but isn't is worse than no trace: it sends you
//     hunting for a bug in the wrong place.
//
//  2. REDACTED. The whole point of the log is that users send it. Bodies
//     carry Authorization headers, api_key fields and OAuth tokens; "remember
//     to scrub it first" is not a control that works. Redaction happens at
//     the single emit() seam so no call site can forget it.

#include <cstdlib>
#include <fstream>
#include <string>

#include "agtest.hpp"
#include "agentty/util/logx.hpp"

using namespace agentty;

namespace {

// Emit one line and return what actually landed in the file.
std::string emit_and_read(const std::string& msg) {
    const std::string path = std::string{std::getenv("TMPDIR") ?
        std::getenv("TMPDIR") : "/tmp"} + "/agentty-redact-probe.log";
    std::remove(path.c_str());
    // The sink is resolved once per process, so drive the real emit path and
    // read back through the public accessor rather than re-opening a sink.
    logx::emit(logx::Channel::Wire, logx::Level::Error, "test.probe", msg);
    const auto lf = logx::log_file();
    if (lf.empty()) return {};
    std::ifstream in{std::string{lf}, std::ios::binary};
    std::string all{std::istreambuf_iterator<char>(in),
                    std::istreambuf_iterator<char>()};
    // Return only the last line — earlier tests share the file.
    if (all.empty()) return {};
    auto end = all.find_last_not_of('\n');
    if (end == std::string::npos) return {};
    auto start = all.rfind('\n', end);
    return all.substr(start == std::string::npos ? 0 : start + 1);
}

// These run as a STANDALONE binary whose ctest ENVIRONMENT sets AGENTTY_LOG
// + AGENTTY_LOG_FILE (see cmake/AgenttyTests.cmake). If logging is off the
// harness is misconfigured — REQUIRE rather than skip, because a test that
// silently no-ops is worse than one that fails: it reports green while
// asserting nothing, which is exactly what happened when these lived in the
// consolidated binary.
void require_logging() {
    REQUIRE_MESSAGE(!logx::log_file().empty(),
                    "AGENTTY_LOG/_FILE must be set for this binary "
                    "(ctest sets them; see cmake/AgenttyTests.cmake)");
}

} // namespace

TEST_CASE("logx: a long body is not truncated") {
    require_logging();
    const std::string body(8000, 'x');
    const auto line = emit_and_read("raw=" + body);
    // The old fixed buffer capped the whole line at 384 bytes.
    CHECK(line.size() > 8000);
    CHECK(line.find(body) != std::string::npos);
}

TEST_CASE("logx: bearer tokens and api keys are redacted") {
    require_logging();
    struct Case { const char* msg; const char* leak; };
    const Case cases[] = {
        {R"(authorization: Bearer sk-proj-AbCdEf0123456789XYZ)", "sk-proj-AbCdEf"},
        {R"({"api_key":"sk-ant-api03-SECRETVALUE12345"})",       "SECRETVALUE"},
        {R"(github token ghu_16CharsOfTokenHere123456)",          "16CharsOfToken"},
        {R"({"access_token":"eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.aaa"})", "eyJhbGciOi"},
        {R"({"refresh_token":"rt_abcdefghijklmnop"})",            "abcdefghijklmnop"},
        {R"(client_secret=abcdef1234567890abcdef)",               "abcdef1234567890"},
    };
    for (const auto& c : cases) {
        INFO("input = " << c.msg);
        const auto line = emit_and_read(c.msg);
        CHECK(line.find(c.leak) == std::string::npos);   // secret is gone
        CHECK(line.find("<redacted>") != std::string::npos);
    }
}

TEST_CASE("logx: redaction keeps the line diagnosable") {
    require_logging();
    // Structure must survive — a redactor that eats the surrounding JSON
    // makes the log useless, which is a worse failure than a missed token.
    const auto line = emit_and_read(
        R"({"model":"gpt-5.6-luna","api_key":"sk-abcdefghijklmnop","stream":true})");
    CHECK(line.find("gpt-5.6-luna") != std::string::npos);
    CHECK(line.find("\"stream\":true") != std::string::npos);
    CHECK(line.find("abcdefghijklmnop") == std::string::npos);
}

TEST_CASE("logx: ordinary payload is never mangled") {
    require_logging();
    // False positives are the real risk: over-eager redaction silently
    // corrupts the payload you are trying to read.
    const auto line = emit_and_read(
        R"({"pattern":"foo-bar_baz","path":"src/task-runner.cpp","sk":"short"})");
    CHECK(line.find("foo-bar_baz") != std::string::npos);
    CHECK(line.find("src/task-runner.cpp") != std::string::npos);
    CHECK(line.find("<redacted>") == std::string::npos);
}

// The realistic leak path: a provider that echoes the request back in an
// error body, or a config/tool payload that embeds a key. Headers are never
// logged, so THIS is where a real key would surface.
TEST_CASE("logx: a key embedded in a body is redacted") {
    require_logging();
    const auto line = emit_and_read(
        R"({"error":{"message":"Incorrect API key provided: sk-proj-Abc123Def456Ghi789. )"
        R"(You can find your API key at https://platform.openai.com/account/api-keys."}})");
    CHECK(line.find("Abc123Def456Ghi789") == std::string::npos);
    CHECK(line.find("<redacted>") != std::string::npos);
    // The actionable part of the message must survive.
    CHECK(line.find("Incorrect API key provided") != std::string::npos);
    CHECK(line.find("platform.openai.com") != std::string::npos);
}

// ── Vendor key shapes ────────────────────────────────────────────────────
//
// Found by probing the shapes providers ACTUALLY emit rather than the ones
// I had imagined. Groq's `gsk_` and `AWS_SECRET_ACCESS_KEY=` both leaked
// through the first version of the redactor.
TEST_CASE("logx: real vendor key shapes are redacted") {
    require_logging();
    struct Case { const char* msg; const char* leak; };
    const Case cases[] = {
        {R"(x-api-key: sk-ant-api03-REALLOOKINGKEY0123456789)", "REALLOOKINGKEY"},
        {R"(Authorization: Basic dXNlcjpwYXNzd29yZDEyMzQ1Ng==)", "dXNlcjpwYXNz"},
        {R"(AWS_SECRET_ACCESS_KEY=wJalrXUtnFEMIbKxKEY0123456789EXAMPLEKEY)", "wJalrXUtnFEM"},
        {R"({"anthropic_api_key":"sk-ant-VALUE9876543210abc"})", "VALUE9876543210"},
        {R"(gsk_GroqKeyLooksLikeThis0123456789abcd)", "GroqKeyLooksLike"},
        {R"(export ANTHROPIC_API_KEY=sk-ant-api03-abcdefghij0123456789)", "abcdefghij0123"},
        // A key pasted into the CONVERSATION, not a header — the path that
        // actually reaches the log, since headers are never logged.
        {R"({"messages":[{"role":"user","content":"my key is sk-proj-LEAK123456789abc"}]})",
         "LEAK123456789"},
    };
    for (const auto& c : cases) {
        INFO("input = " << c.msg);
        const auto line = emit_and_read(c.msg);
        CHECK(line.find(c.leak) == std::string::npos);
        CHECK(line.find("<redacted>") != std::string::npos);
    }
}

// The inverse, and the more important one: an over-eager redactor silently
// CORRUPTS the payload you are reading. A first attempt at covering AWS keys
// added a bare "secret" key-word, which ate `/etc/secrets/config.yaml` and
// `{"pattern":"secret_sauce"}` — and, because the marker is written in
// place, produced repeated garbage rather than a clean replacement.
TEST_CASE("logx: ordinary prose that merely mentions secrets is untouched") {
    require_logging();
    const char* clean[] = {
        R"({"pattern":"secret_sauce","path":"src/secrets_test.cpp"})",
        R"({"tool":"grep","args":{"pattern":"password","glob":"*.env"}})",
        R"(error: file not found: /etc/secrets/config.yaml)",
        R"({"model":"gpt-5.6-luna","messages":[{"content":"explain sk-learn"}]})",
        R"({"display_description":"Read the private_key docs section"})",
    };
    for (const auto* c : clean) {
        INFO("input = " << c);
        const auto line = emit_and_read(c);
        CHECK(line.find("<redacted>") == std::string::npos);
        // And the payload survives byte-for-byte.
        CHECK(line.find(c) != std::string::npos);
    }
}
