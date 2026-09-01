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
#include <mutex>
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
// Mid-run rotation: bytes written since open. Startup-only rotation is not
// enough for the dev workflow (trace-by-default + long-lived sessions):
// a week of wire chunks would grow one file unbounded. Checked with a
// relaxed add per write; the actual rotation is serialized by g_rotate_mu
// and re-checked under the lock, so concurrent writers rotate exactly once.
std::atomic<std::int64_t> g_bytes{0};
std::mutex g_rotate_mu;
constexpr std::int64_t kRotateBytes = 32ll * 1024 * 1024;

// ── Secret redaction ──────────────────────────────────────────────────
//
// Applied at the ONE seam every event passes through, so no call site can
// forget it and no future channel can bypass it.
//
// This exists because the whole point of the log is that users SEND it.
// `wire=trace` carries request bodies and provider replies; an Authorization
// header or an `"api_key"` field pasted into a Discord thread is a real
// incident, and "remember to scrub it first" is not a control that works.
//
// Strategy: find high-signal secret SHAPES and replace the secret VALUE with
// a marker, keeping the surrounding structure so the line stays readable and
// the JSON stays diagnosable. We deliberately do not try to be exhaustive —
// an over-eager redactor that eats real payload makes the log useless, which
// is a worse failure than a missed exotic token. The shapes below cover the
// credentials agentty actually handles.
//
// COST: this runs only on a line that is already going to be written, i.e.
// only when a channel is enabled. A disabled log statement never reaches
// emit() at all (the macro gates first), so redaction is free in the
// default configuration.
std::atomic<unsigned long> g_redactions{0};

// Case-insensitive find of `needle` in [p, p+n).
[[nodiscard]] const char* find_ci(const char* p, std::size_t n,
                                  std::string_view needle) noexcept {
    if (needle.empty() || n < needle.size()) return nullptr;
    const auto lower = [](char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    };
    for (std::size_t i = 0; i + needle.size() <= n; ++i) {
        std::size_t j = 0;
        while (j < needle.size() && lower(p[i + j]) == lower(needle[j])) ++j;
        if (j == needle.size()) return p + i;
    }
    return nullptr;
}

[[nodiscard]] bool token_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'
        || c == '+' || c == '/' || c == '=' || c == '~';
}

// Redact in place. Returns the new length (never grows: the marker is
// shorter than any secret we replace, so this is a safe in-buffer rewrite).
[[nodiscard]] std::size_t redact(char* buf, std::size_t n) noexcept {
    static constexpr std::string_view kMark = "<redacted>";
    // Prefix shapes: the token itself starts at the match.
    static constexpr std::string_view kPrefixes[] = {
        "sk-", "ghu_", "ghp_", "gho_", "ghs_", "github_pat_",
        "xai-", "csk-", "tvly-", "AIza", "eyJ",   // eyJ = a JWT header
        // Vendor prefixes found by probing the shapes providers actually
        // emit: Groq (gsk_), Mistral/Fireworks-style (r8_), Anthropic admin
        // (sk-ant- is covered by sk-), OpenRouter (or-), Together (tgp_).
        "gsk_", "r8_", "or-v1-", "tgp_v1_", "hf_", "nvapi-",
    };
    // Key shapes: skip the key, then redact the VALUE that follows.
    // Matched as a SUFFIX-tolerant substring, so "anthropic_api_key" and
    // "AWS_SECRET_ACCESS_KEY" hit via "api_key" / "secret" rather than
    // needing one entry per vendor spelling.
    static constexpr std::string_view kKeys[] = {
        "authorization", "x-api-key", "api_key", "apikey", "api-key",
        "access_token", "refresh_token", "id_token", "session_token",
        // NOTE: no bare "secret" / "private_key" entry. A generic word
        // matches ordinary prose ("/etc/secrets/config.yaml", a doc
        // reference to private_key) and eats the payload you are trying to
        // read — a redactor that corrupts real content is a worse failure
        // than one that misses an exotic token. Only key-shaped spellings
        // that are followed by a value get replaced.
        "client_secret", "secret_access_key", "secret_key",
        "password", "passphrase", "code_verifier",
    };

    std::size_t out = 0;
    std::size_t i = 0;
    unsigned hits = 0;

    const auto emit_mark = [&] {
        for (char c : kMark) if (out < n) buf[out++] = c;
        ++hits;
    };

    while (i < n) {
        bool matched = false;

        // — Key = value / "key": "value" —
        for (const auto& k : kKeys) {
            if (i + k.size() > n) continue;
            if (find_ci(buf + i, k.size(), k) != buf + i) continue;
            std::size_t j = i + k.size();
            // Copy the key itself plus the separator run (": ", "=", quotes).
            std::size_t sep = j;
            while (sep < n && (buf[sep] == '"' || buf[sep] == ':'
                               || buf[sep] == '=' || buf[sep] == ' ')) ++sep;
            // A "Bearer "/"Basic " scheme is structure, not secret — keep it.
            for (std::string_view scheme : {"Bearer ", "Basic ", "Token "}) {
                if (sep + scheme.size() <= n
                    && find_ci(buf + sep, scheme.size(), scheme) == buf + sep)
                    sep += scheme.size();
            }
            std::size_t end = sep;
            while (end < n && token_char(buf[end])) ++end;
            if (end == sep) continue;          // no value — not a secret
            for (std::size_t c = i; c < sep && out < n; ++c) buf[out++] = buf[c];
            emit_mark();
            i = end;
            matched = true;
            break;
        }
        if (matched) continue;

        // — Bare token prefixes (sk-…, ghu_…, a JWT) —
        // Only at a token boundary, so "task-" inside a word is untouched.
        const bool at_boundary = (i == 0) || !token_char(buf[i - 1]);
        if (at_boundary) {
            for (const auto& p : kPrefixes) {
                if (i + p.size() > n) continue;
                if (find_ci(buf + i, p.size(), p) != buf + i) continue;
                std::size_t end = i + p.size();
                while (end < n && token_char(buf[end])) ++end;
                // Require enough entropy to be a credential, not a word.
                if (end - i < p.size() + 12) continue;
                emit_mark();
                i = end;
                matched = true;
                break;
            }
        }
        if (matched) continue;

        if (out != i) buf[out] = buf[i];
        ++out; ++i;
    }

    if (hits) g_redactions.fetch_add(hits, std::memory_order_relaxed);
    return out;
}


// Fixed ring of preformatted lines. Writers claim a slot with a relaxed
// fetch_add, memcpy the line, then publish the length with a release
// store; the dumper acquires. A torn slot (claimed, not yet published)
// reads len==0 and is skipped — the recorder is best-effort by design,
// never blocking, never allocating.
//
// The ring slot is deliberately SMALL and fixed: it is written from a
// crash handler, so it must be a preallocated, async-signal-safe memcpy
// target. A long event is clipped HERE and only here — the file sink
// below writes the full line (see kStackLine / the heap fallback), so a
// 4 KB request body lands whole on disk while the crash ring keeps a
// readable summary of it.
constexpr std::size_t kRingSlots = 256;
constexpr std::size_t kSlotBytes = 384;

// Stack budget for formatting one line. Covers the overwhelming majority
// of events with no allocation; anything longer (raw wire bodies, big JSON)
// falls back to a heap buffer sized exactly once. Chosen so a typical SSE
// chunk (~1-2 KB) still formats on the stack.
constexpr std::size_t kStackLine = 4096;
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
    // Seed the rotation counter with the existing size so an appended-to
    // file still rotates at the same absolute threshold.
    if (g_fd >= 0) {
        struct stat st2{};
        g_bytes.store(::stat(path, &st2) == 0 ? st2.st_size : 0,
                      std::memory_order_relaxed);
    }
}

// Rotate the live sink once it exceeds kRotateBytes: rename to .old (the
// previous .old is dropped) and reopen fresh. Called from emit() on the
// writer that crosses the threshold; safe for concurrent writers — the
// mutex serializes, the re-check under the lock makes it idempotent, and
// writers racing the swap at worst land a line in the pre-rotation file
// (O_APPEND keeps every write intact either way). Not async-signal-safe;
// never called from the crash path.
void rotate_if_needed() noexcept {
    if (g_bytes.load(std::memory_order_relaxed) <= kRotateBytes) return;
    std::lock_guard<std::mutex> lk(g_rotate_mu);
    if (g_bytes.load(std::memory_order_relaxed) <= kRotateBytes) return;
    if (g_fd < 0 || g_path.empty()) return;
    const std::string old = g_path + ".old";
    std::remove(old.c_str());
    if (std::rename(g_path.c_str(), old.c_str()) != 0) {
        // Rename failed (permissions? cross-device?) — keep appending to
        // the big file rather than lose events; retry on the next cross.
        g_bytes.store(0, std::memory_order_relaxed);
        return;
    }
    const int oldfd = g_fd;
#if defined(_WIN32)
    const int nfd = AGT_OPEN(g_path.c_str(), _O_WRONLY | _O_APPEND | _O_CREAT | _O_BINARY,
                             _S_IREAD | _S_IWRITE);
#else
    const int nfd = AGT_OPEN(g_path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
#endif
    if (nfd >= 0) {
        g_fd = nfd;
#if defined(_WIN32)
        ::_close(oldfd);
#else
        ::close(oldfd);
#endif
    }
    g_bytes.store(0, std::memory_order_relaxed);
}

} // namespace

namespace detail {

bool init() {
    // ── Default policy ──────────────────────────────────────────────
    //
    // NON-RELEASE builds log EVERYTHING, to ONE file, with no env var to
    // discover. A developer or anyone running a debug build gets a complete
    // trace by default, because the alternative — what this codebase had — is
    // that a bug gets reported as a photo of a terminal and diagnosed by
    // guessing. (The Copilot tool-argument bug was exactly that: the fix was
    // one ignored SSE event, but the symptom read as a weak model, so the
    // first instinct was to try a different one.)
    //
    // RELEASE builds stay silent unless AGENTTY_LOG asks: logging costs I/O
    // and a release binary must not write to a user's disk uninvited.
    //
    // Either way there is exactly ONE knob (AGENTTY_LOG) and ONE file
    // (AGENTTY_LOG_FILE). Per-module trace vars are how you end up with eight
    // ways to ask the same question and no way to remember any of them.
    // RELEASE keeps WARN+ rather than going fully silent. That costs a few
    // lines per session and is the difference between a usable first bug
    // report and none at all: without it a user hits a bug, runs
    // `agentty diagnostics`, and gets a file that says "logging was off" —
    // the failure is over, and asking them to reproduce it under
    // AGENTTY_LOG is exactly the friction the command exists to remove.
    //
    // Warn+ is already always kept in the crash ring; this simply persists
    // the same events so they survive the process. Trace/Debug (raw wire
    // bytes, per-event chatter) still require AGENTTY_LOG.
#if defined(NDEBUG)
    constexpr Level kDefault = Level::Warn;
#else
    constexpr Level kDefault = Level::Trace;
#endif
    for (auto& lv : g_min_level)
        lv.store(static_cast<std::uint8_t>(kDefault), std::memory_order_relaxed);

    const char* spec = std::getenv("AGENTTY_LOG");
    const char* file = std::getenv("AGENTTY_LOG_FILE");
    // Legacy shim: AGENTTY_DEBUG_LOG=<path> = file + debug default.
    const char* legacy = std::getenv("AGENTTY_DEBUG_LOG");
    if ((!file || !*file) && legacy && *legacy) file = legacy;
    if ((!spec || !*spec) && legacy && *legacy)  spec = "debug";

    if (spec && *spec) parse_filter(spec);
    // A filter without a file logs to <user-root>/logs/agentty.log
    // ($AGENTTY_HOME/logs or ~/.agentty/logs — the single-root layout,
    // see util/user_root.hpp) so AGENTTY_LOG=debug alone just works.
    // In a non-release build the same is true with NO env var at all.
    const bool want_file = (spec && *spec) || kDefault != Level::Off;
    if (file && *file) open_sink(file);
    // Deliberately hand-rolled env reads (no <filesystem>, no user_root
    // dependency): logx must stay linkable from the narrow sanitizer
    // test TUs and be crash-handler-safe.
    if (g_fd < 0 && want_file) {
        std::string root;
        if (const char* ah = std::getenv("AGENTTY_HOME"); ah && *ah)
            root = ah;
        else if (const char* home = std::getenv("HOME"); home && *home)
            root = std::string{home} + "/.agentty";
#if defined(_WIN32)
        if (root.empty())
            if (const char* prof = std::getenv("USERPROFILE"); prof && *prof)
                root = std::string{prof} + "\\.agentty";
#endif
        if (!root.empty()) {
#if defined(_WIN32)
            ::_mkdir(root.c_str());
            const std::string dir = root + "\\logs";
            ::_mkdir(dir.c_str());
            open_sink((dir + "\\agentty.log").c_str());
#else
            ::mkdir(root.c_str(), 0700);
            const std::string dir = root + "/logs";
            ::mkdir(dir.c_str(), 0755);
            open_sink((dir + "/agentty.log").c_str());
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

unsigned long redaction_count() noexcept {
    return g_redactions.load(std::memory_order_relaxed);
}

void emit(Channel ch, Level lv, std::string_view site,
          std::string_view msg) noexcept {
    (void)detail::inited();

    const bool to_file = (g_fd >= 0) && enabled(ch, lv);
    const bool to_ring = lv >= Level::Warn
                      || detail::g_ring_all.load(std::memory_order_relaxed);
    if (!to_file && !to_ring) return;

    // ── Format the full line once ─────────────────────────────────
    // 2026-08-28T01:23:45.678 +0012345ms tid4 W wire    site: msg
    //
    // The line lives on the stack unless it doesn't fit. Raw wire bodies
    // routinely exceed a few KB, and the old fixed 384-byte buffer silently
    // clipped them — a "verbatim" wire trace that dropped 90% of every
    // request body, which is worse than no trace because it looks complete.
    // Heap fallback keeps long events whole; the common short event still
    // formats with zero allocation.
    char  stack_line[kStackLine];
    std::string heap_line;
    char* line = stack_line;
    std::size_t cap = sizeof(stack_line);

    // Header is bounded (timestamp + tid + level + channel + site ≤ ~120B);
    // only `msg` can be large, so one size check suffices.
    constexpr std::size_t kHeaderMax = 160;
    if (msg.size() + kHeaderMax + 2 > cap) {
        heap_line.resize(msg.size() + kHeaderMax + 2);
        line = heap_line.data();
        cap  = heap_line.size();
    }

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
            line, cap,
            "%04d-%02d-%02dT%02d:%02d:%02d.%03d +%07lldms %04x %c %-7.*s %.*s: ",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec, ms,
            mono_ms, tid_hash,
            kLevelChar[static_cast<std::size_t>(lv)],
            static_cast<int>(cn.size()), cn.data(),
            static_cast<int>(std::min(site.size(), std::size_t{48})),
            site.data());
        if (w < 0) return;
        n = std::min(static_cast<std::size_t>(w), cap - 2);
        const std::size_t room = cap - 2 - n;   // keep \n + NUL
        const std::size_t take = std::min(msg.size(), room);
        std::memcpy(line + n, msg.data(), take);
        n += take;

        // Strip secrets before ANY sink sees the line. One seam, so a new
        // channel or call site cannot leak by omission.
        n = redact(line, n);
        line[n++] = '\n';
    }

    // ── Flight recorder (always for Warn+, or when ring_all) ──────────
    // Clipped to the fixed slot: this buffer is read from a crash handler,
    // so it must stay preallocated and async-signal-safe.
    if (to_ring) {
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
    if (to_file) {
        // Best-effort; short writes / EINTR are dropped rather than looped —
        // diagnostics must never stall the caller. The `!` (not a bare
        // (void)) marks the result used: glibc declares write() __wur under
        // _FORTIFY_SOURCE, and GCC ignores a plain void-cast for
        // warn_unused_result functions.
        (void)!AGT_WRITE(g_fd, line, static_cast<unsigned>(n));
        g_bytes.fetch_add(static_cast<std::int64_t>(n),
                          std::memory_order_relaxed);
        rotate_if_needed();
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

void signal_mark() noexcept {
    // ASYNC-SIGNAL-SAFE by construction: static preformatted bytes, raw
    // write(2)s, atomic loads inside dump_flight_recorder — no allocation,
    // no locks, no formatting. Installed on SIGUSR1 so a dev can stamp the
    // live log from outside (`kill -USR1 $(pgrep agentty)`) the moment they
    // SEE a bug, without touching the TUI: the marker line plus a flight-
    // recorder snapshot land at the exact observation point.
    if (g_fd < 0) return;
    static const char m[] =
        "\n=== MARK (SIGUSR1) — user flagged this moment ===\n";
    (void)!AGT_WRITE(g_fd, m, sizeof(m) - 1);
    (void)dump_flight_recorder(g_fd);
    static const char e[] = "=== END MARK ===\n\n";
    (void)!AGT_WRITE(g_fd, e, sizeof(e) - 1);
}

void session_banner(std::string_view info) noexcept {
    (void)detail::inited();   // resolve the sink first — may open g_fd
    if (g_fd < 0) return;
    std::string line;
    line.reserve(info.size() + 32);
    line += "\n=== agentty session: ";
    line += info;
    line += " ===\n";
    (void)!AGT_WRITE(g_fd, line.data(), static_cast<unsigned>(line.size()));
    g_bytes.fetch_add(static_cast<std::int64_t>(line.size()),
                      std::memory_order_relaxed);
}

} // namespace agentty::logx
