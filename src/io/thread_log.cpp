// agentty::ThreadLog — append-only message log + byte-offset index.
// See thread_log.hpp for the design and the measurements behind it.

#include "agentty/io/thread_log.hpp"

#include "agentty/util/logx.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <system_error>

namespace agentty {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr std::size_t kOffsetWidth = 8;   // uint64 little-endian

// Little-endian by fiat so a threads/ directory copied between machines
// keeps working. Endianness is the kind of thing that is free to get
// right now and expensive to discover later.
void put_u64_le(char* dst, std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) dst[i] = static_cast<char>((v >> (i * 8)) & 0xFF);
}

[[nodiscard]] std::uint64_t get_u64_le(const char* src) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<std::uint64_t>(static_cast<unsigned char>(src[i])) << (i * 8);
    return v;
}

[[nodiscard]] std::uintmax_t file_size_or_zero(const fs::path& p) noexcept {
    std::error_code ec;
    auto n = fs::file_size(p, ec);
    return ec ? 0u : n;
}

// One message as it appears on a line: the shared codec, dumped compact.
//
// dump() can still throw on input the UTF-8 scrub inside message_to_json
// did not reach; a single unserialisable message must not take down the
// save path for the whole thread, so the failure is contained here and
// reported as an empty line the caller skips.
[[nodiscard]] std::string encode_line(const Message& m) {
    try {
        return persistence::message_to_json(m).dump();
    } catch (const std::exception& e) {
        AGT_LOG(General, Error, "thread_log.encode",
                "message {} failed to serialise: {}", m.id.value, e.what());
        return {};
    }
}

} // namespace

// ── open ─────────────────────────────────────────────────────────────

std::expected<ThreadLog, persistence::DeserializeError>
ThreadLog::open_path(fs::path log_path) {
    ThreadLog t;
    t.log_ = std::move(log_path);
    t.idx_ = t.log_;
    t.idx_.replace_extension(".ofs");
    t.meta_ = t.log_;
    t.meta_.replace_extension(".meta.json");

    std::error_code ec;
    fs::create_directories(t.log_.parent_path(), ec);

    // Metadata first: it is small, it is read exactly once, and a thread
    // with no messages yet still has a title and timestamps. A missing or
    // unreadable sidecar leaves a default-constructed Thread rather than
    // failing the open — the messages are the irreplaceable part, and a
    // thread that comes back untitled beats one that won't open.
    {
        std::ifstream in(t.meta_, std::ios::binary);
        if (in) {
            std::string buf{std::istreambuf_iterator<char>(in),
                            std::istreambuf_iterator<char>()};
            json mj = json::parse(buf, nullptr, /*allow_exceptions=*/false);
            if (!mj.is_discarded()) {
                if (auto m = persistence::thread_meta_from_json(mj)) {
                    t.meta_thread_ = std::move(*m);
                } else {
                    AGT_LOG(General, Warn, "thread_log.meta",
                            "unreadable metadata for {}: {}",
                            t.log_.string(), m.error().render());
                }
            }
        }
    }

    const auto log_bytes = file_size_or_zero(t.log_);
    if (log_bytes == 0) {
        // Absent or empty log is a legitimate state (a brand-new thread),
        // not an error. Any index lying beside it is stale by definition.
        t.offsets_.clear();
        return t;
    }

    // Read the index, then decide whether to believe it.
    const auto idx_bytes = file_size_or_zero(t.idx_);
    bool usable = idx_bytes > 0 && (idx_bytes % kOffsetWidth) == 0;
    if (usable) {
        std::ifstream in(t.idx_, std::ios::binary);
        std::string buf(static_cast<std::size_t>(idx_bytes), '\0');
        in.read(buf.data(), static_cast<std::streamsize>(idx_bytes));
        usable = in && static_cast<std::uintmax_t>(in.gcount()) == idx_bytes;
        if (usable) {
            const std::size_t n = buf.size() / kOffsetWidth;
            t.offsets_.resize(n);
            for (std::size_t i = 0; i < n; ++i)
                t.offsets_[i] = get_u64_le(buf.data() + i * kOffsetWidth);

            // Validate against the log. These are cheap checks that catch
            // every way the pair can fall out of step: a crash between the
            // two appends, a truncated index, an index from a different
            // (or rewritten) log, offsets that don't point into the file.
            //
            // Deliberately NOT validated: that each offset lands on a real
            // line boundary. That would cost a full scan, which is the
            // thing the index exists to avoid — and a wrong-but-in-range
            // offset is caught by the line failing to parse.
            if (t.offsets_.empty() || t.offsets_.front() != 0) usable = false;
            for (std::size_t i = 1; usable && i < n; ++i)
                if (t.offsets_[i] <= t.offsets_[i - 1]) usable = false;
            if (usable && t.offsets_.back() >= log_bytes) usable = false;
        }
    }

    if (!usable) {
        // The index is a cache: rebuild silently rather than surfacing an
        // error for a file the user isn't meant to think about. Costs one
        // scan (12 ms for a 29 MB thread) and self-heals permanently.
        t.rebuild_index_();
        if (!t.write_index_())
            AGT_LOG(General, Warn, "thread_log.index",
                    "could not persist rebuilt index for {}", t.log_.string());
    } else {
        // The index was trusted, so the full scan never ran — but a torn
        // final line is still possible (a crash writes the bytes, the
        // '\n', and the index in that order, so an index can legitimately
        // describe a log whose last line is incomplete).
        //
        // Detecting it costs ONE byte: if the log does not end with a
        // newline, the last recorded offset starts an unterminated line.
        // Skipping this check would let the next append glue itself onto
        // the partial line and lose both messages.
        std::ifstream in(t.log_, std::ios::binary);
        if (in) {
            in.seekg(-1, std::ios::end);
            char last = '\n';
            in.read(&last, 1);
            if (in && last != '\n' && !t.offsets_.empty())
                t.torn_tail_at_ = t.offsets_.back();
        }
    }
    return t;
}

std::expected<ThreadLog, persistence::DeserializeError>
ThreadLog::open(const ThreadId& id) {
    if (id.value.empty())
        return std::unexpected(persistence::DeserializeError{
            persistence::DeserializeErrorKind::InvalidValue, "id", "empty thread id"});
    return open_path(persistence::threads_dir() / (id.value + ".jsonl"));
}

// ── index maintenance ────────────────────────────────────────────────

void ThreadLog::rebuild_index_() {
    offsets_.clear();
    torn_tail_at_ = kNoTear;
    std::ifstream in(log_, std::ios::binary);
    if (!in) return;

    // Stream in chunks: a 29 MB log must not become a 29 MB allocation
    // just to find its line starts.
    constexpr std::size_t kChunk = 1u << 16;
    std::string buf(kChunk, '\0');
    std::uint64_t pos = 0;
    bool at_line_start = true;
    std::uint64_t last_start = 0;
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(kChunk));
        const std::streamsize got = in.gcount();
        for (std::streamsize i = 0; i < got; ++i) {
            if (at_line_start) {
                last_start = pos + static_cast<std::uint64_t>(i);
                offsets_.push_back(last_start);
                at_line_start = false;
            }
            if (buf[static_cast<std::size_t>(i)] == '\n') at_line_start = true;
        }
        pos += static_cast<std::uint64_t>(got);
    }
    // A final line with no terminating newline is a TORN APPEND: the
    // process died between writing the bytes and writing the '\n'. It is
    // recorded as an offset (so range() sees it, fails to parse it, and
    // drops it) but also remembered here, because appending after it must
    // NOT concatenate onto it — that would merge the corpse of the old
    // message with the new one and lose BOTH.
    if (!at_line_start && !offsets_.empty()) torn_tail_at_ = last_start;
}

bool ThreadLog::write_index_() const {
    std::string buf(offsets_.size() * kOffsetWidth, '\0');
    for (std::size_t i = 0; i < offsets_.size(); ++i)
        put_u64_le(buf.data() + i * kOffsetWidth, offsets_[i]);
    return persistence::write_json_atomic(idx_, buf);
}

bool ThreadLog::set_meta(const Thread& t) {
    // Store the header only — the messages live in the log, and keeping a
    // second copy here is how the two would drift.
    meta_thread_ = t;
    meta_thread_.messages.clear();
    try {
        return persistence::write_json_atomic(
            meta_, persistence::thread_meta_to_json(meta_thread_).dump(2));
    } catch (const std::exception& e) {
        AGT_LOG(General, Error, "thread_log.meta",
                "could not serialise metadata for {}: {}",
                log_.string(), e.what());
        return false;
    }
}

// ── read ─────────────────────────────────────────────────────────────

std::vector<Message> ThreadLog::range(std::size_t from, std::size_t to) const {
    std::vector<Message> out;
    const std::size_t n = offsets_.size();
    if (from >= n || from >= to) return out;
    to = std::min(to, n);

    std::ifstream in(log_, std::ios::binary);
    if (!in) return out;

    // One seek, then a single contiguous read to the end of the window.
    // Reading line-by-line through the stream would issue a syscall per
    // message; the window is bounded by the caller, so slurping it is
    // both simpler and faster.
    const std::uint64_t begin = offsets_[from];
    const std::uint64_t end   = (to < n) ? offsets_[to]
                                         : static_cast<std::uint64_t>(
                                               file_size_or_zero(log_));
    if (end <= begin) return out;

    std::string buf(static_cast<std::size_t>(end - begin), '\0');
    in.seekg(static_cast<std::streamoff>(begin));
    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    buf.resize(static_cast<std::size_t>(in.gcount()));

    out.reserve(to - from);
    std::size_t pos = 0;
    while (pos < buf.size()) {
        std::size_t nl = buf.find('\n', pos);
        const bool last = (nl == std::string::npos);
        const std::string_view line{buf.data() + pos,
                                    (last ? buf.size() : nl) - pos};
        pos = last ? buf.size() : nl + 1;
        if (line.empty()) continue;

        // A bad line costs that message, not the thread. This is where a
        // torn final append lands: it parses as garbage and is dropped,
        // leaving every complete message intact.
        json j = json::parse(line, nullptr, /*allow_exceptions=*/false);
        if (j.is_discarded()) {
            AGT_LOG(General, Warn, "thread_log.read",
                    "skipping unparseable line in {}", log_.string());
            continue;
        }
        auto m = persistence::message_from_json(j);
        if (!m) {
            AGT_LOG(General, Warn, "thread_log.read",
                    "skipping malformed message in {}: {}",
                    log_.string(), m.error().render());
            continue;
        }
        out.push_back(std::move(*m));
    }
    return out;
}

// ── write ────────────────────────────────────────────────────────────

bool ThreadLog::append(const Message& m) {
    const std::string line = encode_line(m);
    if (line.empty()) return false;

    // HEAL A TORN TAIL FIRST.
    //
    // If the previous run died mid-append, the log ends with a partial
    // line and no newline. Appending onto that would CONCATENATE the new
    // message onto the corpse of the old one, producing a single
    // unparseable line — losing the new message as well as the torn one,
    // silently, forever. The bytes are already unrecoverable; what must
    // not happen is taking a good message down with them.
    //
    // Truncating to the last complete line is the honest repair: it is
    // exactly what range() already reports, so the file now matches what
    // every reader has been seeing.
    if (torn_tail_at_ != kNoTear) {
        std::error_code ec;
        fs::resize_file(log_, static_cast<std::uintmax_t>(torn_tail_at_), ec);
        if (ec) {
            AGT_LOG(General, Error, "thread_log.append",
                    "cannot truncate torn tail of {}: {}",
                    log_.string(), ec.message());
            return false;
        }
        AGT_LOG(General, Warn, "thread_log.append",
                "dropped a torn final line from {} before appending",
                log_.string());
        if (!offsets_.empty() && offsets_.back() == torn_tail_at_)
            offsets_.pop_back();
        torn_tail_at_ = kNoTear;
    }

    // Order is load-bearing: the log is made durable BEFORE the index
    // grows to describe it. The reverse would let a crash leave an offset
    // pointing past the end of the log — the one inconsistency that isn't
    // self-correcting, since open() would have to distrust every index.
    const std::uint64_t at = static_cast<std::uint64_t>(file_size_or_zero(log_));
    {
        std::ofstream out(log_, std::ios::binary | std::ios::app);
        if (!out) return false;
        out.write(line.data(), static_cast<std::streamsize>(line.size()));
        out.put('\n');
        out.flush();
        if (!out) return false;
    }

    offsets_.push_back(at);

    // The index is rewritten whole rather than appended to. It is 8 bytes
    // per message — 20 KB for a 2519-message thread — so a full atomic
    // rewrite is microseconds, and it keeps the crash story trivial: the
    // index is either the old one or the new one, never a torn mix.
    if (!write_index_()) {
        AGT_LOG(General, Warn, "thread_log.append",
                "index write failed for {} (will rebuild on next open)",
                log_.string());
    }
    return true;
}

bool ThreadLog::rewrite(const std::vector<Message>& msgs) {
    std::string body;
    std::vector<std::uint64_t> offs;
    offs.reserve(msgs.size());
    for (const auto& m : msgs) {
        const std::string line = encode_line(m);
        if (line.empty()) continue;      // see encode_line: logged, skipped
        offs.push_back(static_cast<std::uint64_t>(body.size()));
        body += line;
        body += '\n';
    }

    // Both files land atomically. write_json_atomic is temp+fsync+rename,
    // so an interrupted rewrite leaves the previous log AND its matching
    // index in place — never a new log with an old index.
    if (!persistence::write_json_atomic(log_, body)) return false;
    offsets_ = std::move(offs);
    torn_tail_at_ = kNoTear;   // a full rewrite always ends cleanly
    if (!write_index_()) {
        AGT_LOG(General, Warn, "thread_log.rewrite",
                "index write failed for {} (will rebuild on next open)",
                log_.string());
    }
    return true;
}

// ── whole-thread convenience ────────────────────────────────────

bool ThreadLog::exists() const noexcept {
    std::error_code ec;
    return fs::exists(log_, ec);
}

Thread ThreadLog::load_thread() const {
    Thread t = meta_thread_;
    t.messages = all();
    // The compaction records were read before the message count was
    // known (metadata is a separate file), so apply the same bound the
    // whole-document loader applies: a record pointing past the end of
    // the transcript is from an interrupted save and must not survive.
    persistence::clamp_compactions(t);
    return t;
}

bool ThreadLog::store_thread(const Thread& t) {
    // Messages first. If the log write fails the metadata is left alone,
    // so the pair on disk stays the one that was last written whole —
    // rather than new metadata describing old history.
    if (!rewrite(t.messages)) return false;
    return set_meta(t);
}

void ThreadLog::remove() {
    std::error_code ec;
    fs::remove(log_, ec);
    fs::remove(idx_, ec);
    fs::remove(meta_, ec);
    offsets_.clear();
    torn_tail_at_ = kNoTear;
    meta_thread_ = Thread{};
}

} // namespace agentty
