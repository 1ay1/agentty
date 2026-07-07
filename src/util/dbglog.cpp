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
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03lld",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<long long>(ms));
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
