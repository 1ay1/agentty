// logx_test — the structured diagnostic log (agentty::logx).
//
// Covers: filter parsing (bare level, chan=level, mixed, garbage), gate
// semantics (enabled/recorded, ring-always-on-Warn+), file sink (atomic
// appends land, format shape), the flight recorder (ring capture, dump
// ordering, wraparound), spans, and the dbglog compat shim.
//
// Env-sensitive: sets AGENTTY_LOG/_FILE before first use. logx latches
// its config on first call (magic static), so this test relies on being
// its own process (standalone binary) and configures BEFORE any logging.

#include "agentty/util/logx.hpp"
#include "agentty/util/dbglog.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

static int g_fail = 0;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) { std::printf("  FAIL: %s\n", msg); ++g_fail; }         \
        else         { std::printf("  ok:   %s\n", msg); }                   \
    } while (0)

using namespace agentty;

static std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main() {
    // Configure BEFORE the first logx call: default warn, wire wide open,
    // rag silenced. File in a temp path.
    const std::string log_path = "/tmp/agentty-logx-test.log";
    std::remove(log_path.c_str());
    setenv("AGENTTY_LOG", "warn,wire=trace,rag=off", 1);
    setenv("AGENTTY_LOG_FILE", log_path.c_str(), 1);

    // ── Gate semantics ────────────────────────────────────────────────
    CHECK(logx::enabled(logx::Channel::Wire, logx::Level::Trace),
          "filter: wire=trace opens the wire channel at trace");
    CHECK(!logx::enabled(logx::Channel::General, logx::Level::Debug),
          "filter: default warn masks general/debug");
    CHECK(logx::enabled(logx::Channel::General, logx::Level::Warn),
          "filter: default warn passes general/warn");
    CHECK(!logx::enabled(logx::Channel::Rag, logx::Level::Error),
          "filter: rag=off silences even error ON THE FILE");
    CHECK(logx::recorded(logx::Channel::Rag, logx::Level::Error),
          "recorded: Warn+ always reaches the flight ring, even when the "
          "file filter silences the channel");

    // ── File sink + format ────────────────────────────────────────────
    AGT_LOG(Wire, Trace, "test.site", "hello {} {}", 42, "world");
    AGT_LOG(General, Debug, "test.site", "MUST NOT APPEAR {}", 1);
    AGT_LOG(General, Error, "test.err", "an error line");
    {
        const std::string got = slurp(log_path);
        CHECK(got.find("hello 42 world") != std::string::npos,
              "file: enabled trace line lands");
        CHECK(got.find("MUST NOT APPEAR") == std::string::npos,
              "file: masked debug line does NOT land");
        CHECK(got.find("an error line") != std::string::npos,
              "file: default-warn error line lands");
        CHECK(got.find(" T wire") != std::string::npos,
              "format: level char + channel name present");
        CHECK(got.find("test.site: ") != std::string::npos,
              "format: site prefix present");
        // Wall-clock + monotonic stamps.
        CHECK(got.find("ms ") != std::string::npos,
              "format: monotonic ms stamp present");
    }

    // ── logx::log_file resolution ─────────────────────────────────────
    CHECK(logx::log_file() == log_path, "log_file() reports the sink path");

    // ── Flight recorder: capture + dump + ordering ────────────────────
    AGT_LOG(General, Warn, "ring.a", "first");
    AGT_LOG(General, Warn, "ring.b", "second");
    {
        const std::string dump_path = "/tmp/agentty-logx-ring.txt";
        std::remove(dump_path.c_str());
        CHECK(logx::dump_flight_recorder_to(dump_path.c_str()),
              "ring: dump writes bytes");
        const std::string got = slurp(dump_path);
        CHECK(got.find("flight recorder") != std::string::npos,
              "ring: dump carries the header");
        const auto a = got.find("ring.a: first");
        const auto b = got.find("ring.b: second");
        CHECK(a != std::string::npos && b != std::string::npos,
              "ring: both warn events captured");
        CHECK(a < b, "ring: dump is oldest-first");
        // rag=off event still reaches the ring (recorded() contract).
        AGT_LOG(Rag, Error, "ring.rag", "silenced-on-file, ringed");
        std::remove(dump_path.c_str());
        (void)logx::dump_flight_recorder_to(dump_path.c_str());
        CHECK(slurp(dump_path).find("ring.rag") != std::string::npos,
              "ring: file-silenced error still recorded");
        std::remove(dump_path.c_str());
    }

    // ── Ring wraparound: >kRingSlots events keep only the newest ──────
    {
        for (int i = 0; i < 300; ++i)
            AGT_LOG(General, Warn, "wrap", "event {}", i);
        const std::string dump_path = "/tmp/agentty-logx-wrap.txt";
        std::remove(dump_path.c_str());
        (void)logx::dump_flight_recorder_to(dump_path.c_str());
        const std::string got = slurp(dump_path);
        CHECK(got.find("event 299") != std::string::npos,
              "wrap: newest event survives");
        CHECK(got.find("event 0\n") == std::string::npos,
              "wrap: oldest event evicted");
        std::remove(dump_path.c_str());
    }

    // ── Spans ─────────────────────────────────────────────────────────
    {
        { AGT_SPAN(Wire, "test.span"); }   // enters + exits at trace
        const std::string got = slurp(log_path);
        CHECK(got.find("test.span: begin") != std::string::npos,
              "span: begin logged at trace");
        CHECK(got.find("test.span: end (") != std::string::npos,
              "span: end logged with duration");
        // A span on a masked channel emits nothing.
        const auto size_before = slurp(log_path).size();
        { AGT_SPAN(Rag, "rag.span"); }
        CHECK(slurp(log_path).size() == size_before,
              "span: masked channel emits nothing");
    }

    // ── dbglog compat shim ────────────────────────────────────────────
    {
        util::dbglog("legacy.site", "legacy message");
        const std::string got = slurp(log_path);
        CHECK(got.find("legacy.site: legacy message") != std::string::npos,
              "shim: util::dbglog lands as general/error");
        CHECK(util::dbglog_enabled(),
              "shim: dbglog_enabled true (warn default passes error)");
    }

    // ── Long message truncation is safe (no overflow, still terminated) ─
    {
        std::string big(2000, 'x');
        AGT_LOG(General, Warn, "trunc", "{}", big);
        const std::string dump_path = "/tmp/agentty-logx-trunc.txt";
        std::remove(dump_path.c_str());
        (void)logx::dump_flight_recorder_to(dump_path.c_str());
        CHECK(slurp(dump_path).find("trunc: xxx") != std::string::npos,
              "trunc: oversized message captured (clipped, not dropped)");
        std::remove(dump_path.c_str());
    }

    std::remove(log_path.c_str());
    if (g_fail == 0) std::printf("All logx tests passed\n");
    return g_fail == 0 ? 0 : 1;
}
