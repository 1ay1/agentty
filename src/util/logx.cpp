// logx.cpp — structured diagnostic log: filter parsing, atomic file
// appends, and the async-signal-safe flight recorder. See logx.hpp for
// the design contract.

#include "agentty/util/logx.hpp"

#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <thread>

#if defined(_WIN32)
  #include <io.h>
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <direct.h>     // _mkdir
  #define AGT_WRITE  ::_write
  #define AGT_OPEN   ::_open
  #define AGT_LSEEK  ::_lseeki64
#else
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <unistd.h>
  #define AGT_WRITE  ::write
  #define AGT_OPEN   ::open
  #define AGT_LSEEK  ::lseek
#endif

namespace agentty::logx {

namespace detail {
std::atomic<std::uint8_t> g_min_level[kChannels];   // zero-init → Trace; init() raises
std::atomic<bool>         g_ring_all{false};
} // namespace detail

namespace {

// ── File sink state (written once by init, read-only after) ───────────
int         g_fd = -1;               // O_APPEND fd, -1 = file logging off
std::string g_path;                  // resolved path for log_file()
constexpr std::int64_t kRotateBytes = 32ll * 1024 * 1024;

// ── Flight recorder ───────────────────────────────────────────────────
// Fixed ring of preformatted lines. Writers claim a slot with a relaxed
// fetch_add, memcpy the line, then publish the length with a release
// store; the dumper acquires. A torn slot (claimed, not yet published)
// reads len==0 and is skipped — the recorder is best-effort by design,
// never blocking, never allocating.
constexpr std::size_t kRingSlots = 256;
constexpr std::size_t kSlotBytes = 384;
struct Slot {
    std::atomic<std::uint16_t> len{0};
    char bytes[kSlotBytes];
};
Slot                     g_ring[kRingSlots];
std::atomic<std::uint64_t> g_ring_head{0};

// ── Level / filter parsing ────────────────────────────────────────────
Level parse_level(std::string_view s) noexcept {
    if (s == "trace") return Level::Trace;
    if (s == "debug") return Level::Debug;
    if (s == "info")  return Level::Info;
    if (s == "warn" || s == "warning") return Level::Warn;
    if (s == "error") return Level::Error;
    if (s == "off" || s == "none") return Level::Off;
    return Level::Warn;   // unknown token → conservative default
}

int channel_index(std::string_view s) noexcept {
    for (std::size_t i = 0; i < kChannels; ++i)
        if (s == kChannelNames[i]) return static_cast<int>(i);
    return -1;
}

// Parse "$AGENTTY_LOG" — "level" and "chan=level" atoms, comma-separated.
// A bare level sets the default for every channel; chan=level overrides.
// Unknown channels/levels are skipped (never fatal — this parses user
// input at startup, a typo must not take the log system down).
void parse_filter(std::string_view spec) noexcept {
    Level def = Level::Off;
    std::array<Level, kChannels> per{};
    per.fill(Level::Off);
    std::array<bool, kChannels> set{};
    set.fill(false);

    std::size_t pos = 0;
    bool any_atom = false;
    while (pos <= spec.size()) {
        auto comma = spec.find(',', pos);
        if (comma == std::string_view::npos) comma = spec.size();
        std::string_view atom = spec.substr(pos, comma - pos);
        pos = comma + 1;
        // trim spaces
        while (!atom.empty() && atom.front() == ' ') atom.remove_prefix(1);
        while (!atom.empty() && atom.back() == ' ')  atom.remove_suffix(1);
        if (atom.empty()) continue;
        any_atom = true;
        if (auto eq = atom.find('='); eq != std::string_view::npos) {
            const int ci = channel_index(atom.substr(0, eq));
            if (ci >= 0) {
                per[static_cast<std::size_t>(ci)] =
                    parse_level(atom.substr(eq + 1));
                set[static_cast<std::size_t>(ci)] = true;
            }
        } else {
            def = parse_level(atom);
        }
        if (comma == spec.size()) break;
    }
    // A spec of only chan=level atoms leaves the default Off (silence the
    // rest); an empty spec means logging stays off entirely.
    if (!any_atom) def = Level::Off;
    for (std::size_t i = 0; i < kChannels; ++i)
        detail::g_min_level[i].store(
            static_cast<std::uint8_t>(set[i] ? per[i] : def),
            std::memory_order_relaxed);
    // If ANY channel accepts below Warn, mirror those events into the
    // flight ring too (a trace-enabled run wants its ring to match).
    bool ring_all = false;
    for (std::size_t i = 0; i < kChannels; ++i)
        if (detail::g_min_level[i].load(std::memory_order_relaxed)
            < static_cast<std::uint8_t>(Level::Warn))
            ring_all = true;
    detail::g_ring_all.store(ring_all, std::memory_order_relaxed);
}

void open_sink(const char* path) noexcept {
    if (!path || !*path) return;
    // Rotate once at startup if oversized — rename to .old (best-effort).
    struct stat st{};
    if (::stat(path, &st) == 0 && st.st_size > kRotateBytes) {
        std::string old = std::string{path} + ".old";
        std::remove(old.c_str());
        std::rename(path, old.c_str());
    }
#if defined(_WIN32)
    g_fd = AGT_OPEN(path, _O_WRONLY | _O_APPEND | _O_CREAT | _O_BINARY,
                    _S_IREAD | _S_IWRITE);
#else
    g_fd = AGT_OPEN(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
#endif
    if (g_fd >= 0) g_path = path;
}

} // namespace

namespace detail {

bool init() {
    // Levels default to Off unless configured.
    for (auto& lv : g_min_level)
        lv.store(static_cast<std::uint8_t>(Level::Off),
                 std::memory_order_relaxed);

    const char* spec = std::getenv("AGENTTY_LOG");
    const char* file = std::getenv("AGENTTY_LOG_FILE");
    // Legacy shim: AGENTTY_DEBUG_LOG=<path> = file + debug default.
    const char* legacy = std::getenv("AGENTTY_DEBUG_LOG");
    if ((!file || !*file) && legacy && *legacy) file = legacy;
    if ((!spec || !*spec) && legacy && *legacy)  spec = "debug";

    if (spec && *spec) parse_filter(spec);
    if (file && *file) open_sink(file);
    // A filter without a file logs to $XDG_STATE_HOME/agentty/agentty.log
    // (or ~/.agentty/agentty.log) so AGENTTY_LOG=debug alone just works.
    if (g_fd < 0 && spec && *spec) {
        std::string fallback;
        if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg && *xdg)
            fallback = std::string{xdg} + "/agentty";
        else if (const char* home = std::getenv("HOME"); home && *home)
            fallback = std::string{home} + "/.agentty";
#if defined(_WIN32)
        if (fallback.empty())
            if (const char* prof = std::getenv("USERPROFILE"); prof && *prof)
                fallback = std::string{prof} + "\\.agentty";
#endif
        if (!fallback.empty()) {
#if defined(_WIN32)
            ::_mkdir(fallback.c_str());
            open_sink((fallback + "\\agentty.log").c_str());
#else
            ::mkdir(fallback.c_str(), 0755);
            open_sink((fallback + "/agentty.log").c_str());
#endif
        }
    }
    return true;
}

} // namespace detail

std::string_view log_file() noexcept {
    (void)detail::inited();
    return g_path;
}

void emit(Channel ch, Level lv, std::string_view site,
          std::string_view msg) noexcept {
    (void)detail::inited();

    // ── Format the full line once, on the stack ───────────────────────
    // 2026-08-28T01:23:45.678 +0012345ms tid4 W wire    site: msg
    char line[kSlotBytes];
    std::size_t n = 0;
    {
        using namespace std::chrono;
        const auto now  = system_clock::now();
        const auto secs = system_clock::to_time_t(now);
        const int  ms   = static_cast<int>(
            duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &secs);
#else
        localtime_r(&secs, &tm);
#endif
        static const auto t0 = steady_clock::now();
        const long long mono_ms = duration_cast<milliseconds>(
            steady_clock::now() - t0).count();
        // Small stable thread tag: hash of thread::id, 4 hex chars.
        const auto tid_hash = static_cast<unsigned>(
            std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xffff);

        const std::size_t ci = static_cast<std::size_t>(ch);
        const auto& cn = kChannelNames[ci];   // literal-backed, %.*s — no alloc
        int w = std::snprintf(
            line, sizeof(line),
            "%04d-%02d-%02dT%02d:%02d:%02d.%03d +%07lldms %04x %c %-7.*s %.*s: ",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec, ms,
            mono_ms, tid_hash,
            kLevelChar[static_cast<std::size_t>(lv)],
            static_cast<int>(cn.size()), cn.data(),
            static_cast<int>(std::min(site.size(), std::size_t{48})),
            site.data());
        if (w < 0) return;
        n = std::min(static_cast<std::size_t>(w), sizeof(line) - 2);
        const std::size_t room = sizeof(line) - 2 - n;   // keep \n + NUL
        const std::size_t take = std::min(msg.size(), room);
        std::memcpy(line + n, msg.data(), take);
        n += take;
        line[n++] = '\n';
    }

    // ── Flight recorder (always for Warn+, or when ring_all) ──────────
    if (lv >= Level::Warn
        || detail::g_ring_all.load(std::memory_order_relaxed)) {
        const auto slot_idx = g_ring_head.fetch_add(1, std::memory_order_relaxed)
                              % kRingSlots;
        Slot& s = g_ring[slot_idx];
        s.len.store(0, std::memory_order_release);        // invalidate
        const auto take = std::min(n, sizeof(s.bytes));
        std::memcpy(s.bytes, line, take);
        s.len.store(static_cast<std::uint16_t>(take),
                    std::memory_order_release);            // publish
    }

    // ── File sink: one write(2) on an O_APPEND fd — atomic append ─────
    if (g_fd >= 0 && enabled(ch, lv)) {
        // Best-effort; short writes / EINTR are dropped rather than looped —
        // diagnostics must never stall the caller. The `!` (not a bare
        // (void)) marks the result used: glibc declares write() __wur under
        // _FORTIFY_SOURCE, and GCC ignores a plain void-cast for
        // warn_unused_result functions.
        (void)!AGT_WRITE(g_fd, line, static_cast<unsigned>(n));
    }
}

std::size_t dump_flight_recorder(int fd) noexcept {
    // Oldest → newest. Only async-signal-safe operations: atomic loads,
    // memcpy-free raw writes of the preformatted slots.
    if (fd < 0) return 0;
    static const char hdr[] = "=== agentty flight recorder (last events, oldest first) ===\n";
    std::size_t total = 0;
    auto wr = [&](const char* p, std::size_t len) {
        const auto k = AGT_WRITE(fd, p, static_cast<unsigned>(len));
        if (k > 0) total += static_cast<std::size_t>(k);
    };
    wr(hdr, sizeof(hdr) - 1);
    const auto head = g_ring_head.load(std::memory_order_relaxed);
    const auto count = head < kRingSlots ? head : kRingSlots;
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto idx = (head - count + i) % kRingSlots;
        const Slot& s = g_ring[idx];
        const auto len = s.len.load(std::memory_order_acquire);
        if (len == 0 || len > kSlotBytes) continue;   // torn/empty slot
        wr(s.bytes, len);
    }
    return total;
}

bool dump_flight_recorder_to(const char* path) noexcept {
    if (!path || !*path) return false;
#if defined(_WIN32)
    int fd = AGT_OPEN(path, _O_WRONLY | _O_APPEND | _O_CREAT | _O_BINARY,
                      _S_IREAD | _S_IWRITE);
#else
    int fd = AGT_OPEN(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
#endif
    if (fd < 0) return false;
    const auto n = dump_flight_recorder(fd);
#if defined(_WIN32)
    ::_close(fd);
#else
    ::close(fd);
#endif
    return n > 0;
}

} // namespace agentty::logx
