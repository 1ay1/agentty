#include "agentty/util/dbglog.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>

namespace agentty::util {

namespace {

// Resolved once: the log path from $AGENTTY_DEBUG_LOG, or empty if unset.
// std::call_once so the getenv read + copy happens exactly once even under
// concurrent worker threads hitting a catch site simultaneously.
const std::string& log_path() {
    static std::string path;
    static std::once_flag once;
    std::call_once(once, [] {
        if (const char* p = std::getenv("AGENTTY_DEBUG_LOG"); p && *p)
            path = p;
    });
    return path;
}

std::mutex& log_mutex() {
    static std::mutex m;
    return m;
}

std::string timestamp() {
    using namespace std::chrono;
    auto now  = system_clock::now();
    auto secs = system_clock::to_time_t(now);
    auto ms   = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &secs);
#else
    localtime_r(&secs, &tm);
#endif
    // Clamp every field to a bounded range before formatting. This is both a
    // correctness guard (a corrupt std::tm from a bad clock can't produce a
    // wild value) AND what lets GCC prove the output fits: without bounds it
    // assumes %04d could emit an arbitrarily wide int and warns the 32-byte
    // buffer might truncate (-Wformat-truncation). Bounded, the worst case is
    // "9999-12-31 23:59:59.999" = 23 bytes, comfortably under 40.
    auto clamp = [](int v, int lo, int hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    };
    const int year = clamp(tm.tm_year + 1900, 0, 9999);
    const int mon  = clamp(tm.tm_mon + 1, 1, 12);
    const int mday = clamp(tm.tm_mday, 1, 31);
    const int hour = clamp(tm.tm_hour, 0, 23);
    const int min  = clamp(tm.tm_min, 0, 59);
    const int sec  = clamp(tm.tm_sec, 0, 60);        // 60 = leap second
    const int msec = clamp(static_cast<int>(ms), 0, 999);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  year, mon, mday, hour, min, sec, msec);
    return buf;
}

} // namespace

bool dbglog_enabled() noexcept {
    return !log_path().empty();
}

void dbglog(std::string_view where, std::string_view msg) noexcept {
    const std::string& path = log_path();
    if (path.empty()) return;   // disabled — the common case, cheap exit

    // Never let logging throw into a catch block that's already handling an
    // exception: swallow any I/O failure here (best-effort diagnostics).
    try {
        std::string line = timestamp();
        line += " [";
        line.append(where.data(), where.size());
        line += "] ";
        line.append(msg.data(), msg.size());
        line += '\n';

        std::scoped_lock lock(log_mutex());
        std::ofstream ofs(path, std::ios::app | std::ios::binary);
        if (ofs) ofs.write(line.data(), static_cast<std::streamsize>(line.size()));
    } catch (...) {
        // Intentionally silent: the whole point of dbglog is to surface the
        // ORIGINAL error, not to introduce a new failure path. If the log
        // file can't be written, we simply have no trace — same as before.
    }
}

} // namespace agentty::util
