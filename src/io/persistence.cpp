#include "agentty/io/persistence.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>

#ifdef _WIN32
#  include <io.h>
#else
#  include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include "agentty/tool/util/utf8.hpp"
#include "agentty/util/base64.hpp"

namespace agentty::persistence {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
// Atomic + durable write: write to <target>.tmp, fsync, rename. A crash
// or ctrl-C mid-write leaves the previous version intact — the loader
// never sees a truncated file that its `catch (...)` would silently drop.
// Binary mode avoids CRLF translation so the on-disk bytes match dump(2).
bool write_json_atomic(const fs::path& target, const std::string& content) {
    fs::path tmp = target;
    tmp += ".tmp";
#ifdef _WIN32
    FILE* fp = ::_wfopen(tmp.wstring().c_str(), L"wb");
#else
    FILE* fp = std::fopen(tmp.c_str(), "wb");
#endif
    if (!fp) return false;
    if (std::fwrite(content.data(), 1, content.size(), fp) != content.size()) {
        std::fclose(fp);
        std::error_code ec; fs::remove(tmp, ec);
        return false;
    }
    std::fflush(fp);
#ifdef _WIN32
    (void)::_commit(::_fileno(fp));
#else
    (void)::fsync(::fileno(fp));
#endif
    if (std::fclose(fp) != 0) {
        std::error_code ec; fs::remove(tmp, ec);
        return false;
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) {
        std::error_code ec2; fs::remove(tmp, ec2);
        return false;
    }
    return true;
}
} // namespace

fs::path data_dir() {
    const char* home = std::getenv("USERPROFILE");
    if (!home) home = std::getenv("HOME");
    fs::path p = home ? fs::path(home) : fs::current_path();
    p /= ".agentty";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

fs::path threads_dir() {
    auto p = data_dir() / "threads";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

static std::string role_to_string(Role r) {
    switch (r) {
        case Role::User: return "user";
        case Role::Assistant: return "assistant";
        case Role::System: return "system";
    }
    return "user";
}
static Role role_from_string(const std::string& s) {
    if (s == "assistant") return Role::Assistant;
    if (s == "system")    return Role::System;
    return Role::User;
}

static json message_to_json(const Message& m) {
    // Belt-and-suspenders UTF-8 scrub. Tool output and freeform text can
    // contain raw bytes from arbitrary files (Latin-1 .htm, Shift-JIS logs)
    // that nlohmann::json::dump() refuses to serialise — it throws
    // type_error.316 and we used to terminate(). Scrub at the boundary so
    // bad bytes can never reach dump(). Tools that already scrub upstream
    // pay only the validate cost here.
    json j;
    // Round-trip the per-message stable id so on-disk → in-memory
    // reload preserves cache keys across sessions. Generated fresh
    // when missing on load (see parse_message) so older threads upgrade
    // transparently — no migration step needed.
    j["id"] = m.id.value;
    j["role"] = role_to_string(m.role);
    j["text"] = tools::util::to_valid_utf8(m.text);
    j["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
        m.timestamp.time_since_epoch()).count();
    json tcs = json::array();
    for (const auto& tc : m.tool_calls) {
        json t;
        t["id"] = tc.id;
        t["name"] = tc.name;
        t["args"] = tc.args;
        t["output"] = tools::util::to_valid_utf8(tc.output()); // empty unless terminal
        t["status"] = std::string{tc.status_name()};
        // Structured FileChange for edit/write tools. Stored on the
        // ToolUse so reopening a thread keeps the per-hunk review
        // state (Pending/Accepted/Rejected). The original/new content
        // blobs are the biggest fields here — a 5000-line file edit
        // doubles into the JSON — but they're needed to replay
        // `diff::apply_accepted` after a per-hunk reject decision on
        // a re-opened thread. Skipped entirely for tools that don't
        // mutate the filesystem.
        if (tc.file_change) {
            const auto& fc = *tc.file_change;
            json fcj;
            fcj["path"]              = fc.path;
            fcj["added"]             = fc.added;
            fcj["removed"]           = fc.removed;
            fcj["original_contents"] = tools::util::to_valid_utf8(fc.original_contents);
            fcj["new_contents"]      = tools::util::to_valid_utf8(fc.new_contents);
            json hs = json::array();
            for (const auto& h : fc.hunks) {
                json hj;
                hj["old_start"] = h.old_start;
                hj["old_len"]   = h.old_len;
                hj["new_start"] = h.new_start;
                hj["new_len"]   = h.new_len;
                hj["patch"]     = tools::util::to_valid_utf8(h.patch);
                hj["status"]    = h.status == Hunk::Status::Accepted ? "accepted"
                                 : h.status == Hunk::Status::Rejected ? "rejected"
                                                                      : "pending";
                hs.push_back(std::move(hj));
            }
            fcj["hunks"] = std::move(hs);
            t["file_change"] = std::move(fcj);
        }
        tcs.push_back(std::move(t));
    }
    j["tool_calls"] = std::move(tcs);
    // Image attachments on User messages — stored on disk as base64
    // so a thread reload can be re-sent on a follow-up turn without
    // having to re-paste the original. Adds ~33% to the message JSON
    // size; absent for messages that didn't carry images, so non-
    // image threads are unaffected.
    if (!m.images.empty()) {
        json imgs = json::array();
        for (const auto& img : m.images) {
            json e;
            e["media_type"] = img.media_type;
            e["data"]       = util::base64_encode(img.bytes);
            imgs.push_back(std::move(e));
        }
        j["images"] = std::move(imgs);
    }
    if (m.checkpoint_id) j["checkpoint_id"] = *m.checkpoint_id;
    // Persist the per-message error so reopening a thread shows which
    // turn died and why. UTF-8 scrubbed for the same reason as `text`.
    if (m.error) j["error"] = tools::util::to_valid_utf8(*m.error);
    if (m.is_compact_summary) j["is_compact_summary"] = true;
    // Non-image attachments (Paste / FileRef / Symbol). Persisted so a
    // reloaded thread can rebuild its wire payload — the user's `text`
    // carries chip placeholders, and the model only sees real content
    // after `attachment::expand(...)` splices the bodies back in at
    // request-build time. Body bytes are base64-encoded since pasted
    // text can contain anything (NULs, lone surrogates, control bytes
    // that the UTF-8 scrub would otherwise mangle).
    if (!m.attachments.empty()) {
        json atts = json::array();
        for (const auto& a : m.attachments) {
            json e;
            switch (a.kind) {
                case Attachment::Kind::Paste:   e["kind"] = "paste";   break;
                case Attachment::Kind::FileRef: e["kind"] = "fileref"; break;
                case Attachment::Kind::Symbol:  e["kind"] = "symbol";  break;
                case Attachment::Kind::Image:   e["kind"] = "image";   break;
            }
            e["body"]        = util::base64_encode(a.body);
            if (!a.path.empty())       e["path"]        = a.path;
            if (!a.media_type.empty()) e["media_type"] = a.media_type;
            if (!a.name.empty())       e["name"]        = a.name;
            if (a.line_number > 0)     e["line_number"] = a.line_number;
            e["line_count"] = a.line_count;
            e["byte_count"] = a.byte_count;
            atts.push_back(std::move(e));
        }
        j["attachments"] = std::move(atts);
    }
    return j;
}

// ── Typed deserializers ──────────────────────────────────────────────────
// One source of truth for "what does a valid Thread JSON look like."
// Required fields fail with `MissingField`; wrong-type fields fail with
// `InvalidValue`; unrecognised discriminators fail with `InvalidVariantTag`.
// Optional fields fall back to defaults silently (timestamps, error strings)
// — those are recoverable; missing them shouldn't kill the whole thread.

std::string DeserializeError::render() const {
    static constexpr std::string_view kind_str[] = {
        "json_parse", "missing_field", "invalid_value",
        "invalid_variant_tag", "io",
    };
    std::string out = "[";
    out += kind_str[static_cast<std::size_t>(kind)];
    out += "] ";
    if (!field.empty()) { out += field; out += ": "; }
    out += detail;
    return out;
}

static std::expected<ToolUse::Status, DeserializeError>
parse_tool_status(std::string_view status_tag, std::string&& output) {
    // Reconstruct the variant. Persisted threads only ever land in
    // terminal states (in-flight tools are never serialized), so the
    // intermediate states reset to a no-arg-time-stamp default.
    if (status_tag == "done")
        return ToolUse::Status{ToolUse::Done{{}, {}, std::move(output)}};
    if (status_tag == "failed" || status_tag == "error")
        return ToolUse::Status{ToolUse::Failed{{}, {}, std::move(output)}};
    if (status_tag == "rejected") return ToolUse::Status{ToolUse::Rejected{{}}};
    if (status_tag == "running")  return ToolUse::Status{ToolUse::Running{{}, {}}};
    if (status_tag == "approved") return ToolUse::Status{ToolUse::Approved{{}}};
    if (status_tag == "pending")  return ToolUse::Status{ToolUse::Pending{{}}};
    return std::unexpected(DeserializeError{
        DeserializeErrorKind::InvalidVariantTag, "tool_calls[*].status",
        std::string{"unknown status tag: "} + std::string{status_tag}});
}

static std::expected<Message, DeserializeError> parse_message(const json& j) {
    if (!j.is_object())
        return std::unexpected(DeserializeError{
            DeserializeErrorKind::InvalidValue, "messages[*]",
            "expected object"});
    Message m;
    // `id` was added in 2026-05; older thread files don't carry it,
    // so the default-constructed `m.id` (already-fresh from
    // new_message_id()) stands in for them. New writes will persist
    // this fresh id, so a save-after-load completes the migration.
    if (auto it = j.find("id"); it != j.end() && it->is_string()
        && !it->get<std::string>().empty())
        m.id = MessageId{it->get<std::string>()};
    m.role = role_from_string(j.value("role", "user"));
    m.text = j.value("text", "");
    if (auto it = j.find("error"); it != j.end() && it->is_string()
        && !it->get<std::string>().empty())
        m.error = it->get<std::string>();
    if (j.contains("timestamp")) {
        const auto& ts = j["timestamp"];
        if (!ts.is_number_integer())
            return std::unexpected(DeserializeError{
                DeserializeErrorKind::InvalidValue, "messages[*].timestamp",
                "expected integer seconds-since-epoch"});
        m.timestamp = std::chrono::system_clock::time_point{
            std::chrono::seconds{ts.get<long long>()}};
    }
    if (j.contains("tool_calls")) {
        const auto& arr = j["tool_calls"];
        if (!arr.is_array())
            return std::unexpected(DeserializeError{
                DeserializeErrorKind::InvalidValue, "messages[*].tool_calls",
                "expected array"});
        for (const auto& t : arr) {
            ToolUse tc;
            tc.id = ToolCallId{t.value("id", "")};
            tc.name = ToolName{t.value("name", "")};
            tc.args = t.value("args", json::object());
            // Old persisted threads stored status as an int enum; new ones
            // use the string tag returned by ToolUse::status_name(). Accept
            // both so existing on-disk threads keep loading.
            std::string status_tag = "pending";
            std::string output = t.value("output", "");
            if (auto it = t.find("status"); it != t.end()) {
                if (it->is_string()) {
                    status_tag = it->get<std::string>();
                } else if (it->is_number()) {
                    static constexpr std::string_view legacy[] = {
                        "pending","approved","running","done","failed","rejected"};
                    int idx = it->get<int>();
                    status_tag = idx >= 0 && idx < static_cast<int>(std::size(legacy))
                        ? std::string{legacy[idx]} : std::string{"pending"};
                }
            }
            auto status = parse_tool_status(status_tag, std::move(output));
            if (!status) return std::unexpected(std::move(status).error());
            tc.status = std::move(*status);
            // file_change — structured diff data, present only for
            // edit/write tool calls in threads saved by builds that
            // had this field. Older threads silently skip; the view
            // falls back to parsing the ```diff fence out of
            // tc.output() the way it always did. Per-field tolerance
            // is intentional — a malformed hunk is just skipped, not
            // failure-of-thread-load — because losing diff-review
            // state on one tool call is better than losing the
            // entire thread.
            if (auto fcit = t.find("file_change");
                fcit != t.end() && fcit->is_object())
            {
                FileChange fc;
                fc.path              = fcit->value("path", std::string{});
                fc.added             = fcit->value("added", 0);
                fc.removed           = fcit->value("removed", 0);
                fc.original_contents = fcit->value("original_contents", std::string{});
                fc.new_contents      = fcit->value("new_contents", std::string{});
                if (auto hit = fcit->find("hunks");
                    hit != fcit->end() && hit->is_array())
                {
                    for (const auto& hj : *hit) {
                        if (!hj.is_object()) continue;
                        Hunk h;
                        h.old_start = hj.value("old_start", 0);
                        h.old_len   = hj.value("old_len",   0);
                        h.new_start = hj.value("new_start", 0);
                        h.new_len   = hj.value("new_len",   0);
                        h.patch     = hj.value("patch", std::string{});
                        auto st = hj.value("status", std::string{"pending"});
                        h.status = st == "accepted" ? Hunk::Status::Accepted
                                 : st == "rejected" ? Hunk::Status::Rejected
                                                    : Hunk::Status::Pending;
                        fc.hunks.push_back(std::move(h));
                    }
                }
                // Defer pushing to m.d.pending_changes — that's a
                // session-scope decision the reducer makes (a re-
                // opened thread shouldn't auto-resurrect a review
                // pane the user dismissed before close). The struct
                // stays on tc.file_change for the body renderer; the
                // user can re-open the pane to revisit if they want.
                tc.file_change = std::move(fc);
            }
            m.tool_calls.push_back(std::move(tc));
        }
    }
    if (j.contains("checkpoint_id")) {
        const auto& cp = j["checkpoint_id"];
        if (!cp.is_string())
            return std::unexpected(DeserializeError{
                DeserializeErrorKind::InvalidValue, "messages[*].checkpoint_id",
                "expected string"});
        m.checkpoint_id = CheckpointId{cp.get<std::string>()};
    }
    if (j.contains("is_compact_summary")) {
        const auto& v = j["is_compact_summary"];
        if (v.is_boolean()) m.is_compact_summary = v.get<bool>();
    }
    if (j.contains("images")) {
        const auto& arr = j["images"];
        if (!arr.is_array())
            return std::unexpected(DeserializeError{
                DeserializeErrorKind::InvalidValue, "messages[*].images",
                "expected array"});
        for (const auto& e : arr) {
            if (!e.is_object()) continue;
            ImageContent img;
            img.media_type = e.value("media_type", "image/png");
            auto data_b64 = e.value("data", std::string{});
            img.bytes = util::base64_decode(data_b64);
            // Drop entries that decode to nothing — corrupted base64
            // shouldn't kill the whole thread load.
            if (!img.bytes.empty()) m.images.push_back(std::move(img));
        }
    }
    if (j.contains("attachments")) {
        const auto& arr = j["attachments"];
        if (!arr.is_array())
            return std::unexpected(DeserializeError{
                DeserializeErrorKind::InvalidValue, "messages[*].attachments",
                "expected array"});
        for (const auto& e : arr) {
            if (!e.is_object()) continue;
            Attachment a;
            auto kind = e.value("kind", std::string{"paste"});
            if      (kind == "paste")   a.kind = Attachment::Kind::Paste;
            else if (kind == "fileref") a.kind = Attachment::Kind::FileRef;
            else if (kind == "symbol")  a.kind = Attachment::Kind::Symbol;
            else if (kind == "image")   a.kind = Attachment::Kind::Image;
            else                        a.kind = Attachment::Kind::Paste;
            a.body        = util::base64_decode(e.value("body", std::string{}));
            a.path        = e.value("path", std::string{});
            a.media_type  = e.value("media_type", std::string{});
            a.name        = e.value("name", std::string{});
            a.line_number = e.value("line_number", 0);
            a.line_count  = e.value("line_count", std::size_t{0});
            a.byte_count  = e.value("byte_count", std::size_t{0});
            m.attachments.push_back(std::move(a));
        }
    }
    return m;
}

// Populates id/title/timestamps only; leaves messages empty. Used by the
// directory-walking metadata load that backs the thread picker.
static std::expected<Thread, DeserializeError>
parse_thread_meta_only(const json& j) {
    if (!j.is_object())
        return std::unexpected(DeserializeError{
            DeserializeErrorKind::InvalidValue, "", "expected top-level object"});
    Thread t;
    auto id_str = j.value("id", "");
    if (id_str.empty())
        return std::unexpected(DeserializeError{
            DeserializeErrorKind::MissingField, "id",
            "thread JSON has no `id` field"});
    t.id = ThreadId{std::move(id_str)};
    t.title = j.value("title", "");
    if (j.contains("created_at"))
        t.created_at = std::chrono::system_clock::time_point{
            std::chrono::seconds{j["created_at"].get<long long>()}};
    if (j.contains("updated_at"))
        t.updated_at = std::chrono::system_clock::time_point{
            std::chrono::seconds{j["updated_at"].get<long long>()}};
    return t;
}

static std::expected<Thread, DeserializeError> parse_thread(const json& j) {
    auto meta = parse_thread_meta_only(j);
    if (!meta) return meta;
    Thread t = std::move(*meta);
    for (const auto& mj : j.value("messages", json::array())) {
        auto msg = parse_message(mj);
        if (!msg) return std::unexpected(std::move(msg).error());
        t.messages.push_back(std::move(*msg));
    }
    // Compactions: optional. Missing on threads from before the feature
    // existed and on threads that simply haven't been compacted yet —
    // both indistinguishable from on-disk and both correctly default to
    // an empty vector. Per-field tolerance is intentional: a malformed
    // record (e.g. negative `up_to_index`) is skipped rather than
    // failing the whole load, because the wire-substitution helper
    // already validates `up_to_index <= messages.size()` and gracefully
    // falls back to "no compaction" when it doesn't — worst case the
    // user re-compacts manually, vs. losing the entire thread.
    if (j.contains("compactions") && j["compactions"].is_array()) {
        for (const auto& cj : j["compactions"]) {
            if (!cj.is_object()) continue;
            Thread::CompactionRecord rec;
            if (cj.contains("up_to_index") && cj["up_to_index"].is_number_integer()) {
                auto v = cj["up_to_index"].get<long long>();
                if (v < 0) continue;
                rec.up_to_index = static_cast<std::size_t>(v);
            } else {
                continue;
            }
            if (cj.contains("summary") && cj["summary"].is_string()) {
                rec.summary = cj["summary"].get<std::string>();
            }
            if (cj.contains("created_at") && cj["created_at"].is_number_integer()) {
                rec.created_at = std::chrono::system_clock::time_point{
                    std::chrono::seconds{cj["created_at"].get<int64_t>()}};
            }
            // Discard records whose boundary doesn't fit the loaded
            // transcript — typically only happens when a save was
            // interrupted mid-compaction.
            if (rec.up_to_index <= t.messages.size()) {
                t.compactions.push_back(std::move(rec));
            }
        }
    }
    return t;
}

// SAX handler that pulls the four top-level metadata fields out of a thread
// JSON file without ever materialising the messages array. The directory
// walk runs this once per file at startup; with 649 files at ~580 KB each
// the previous tree-build path peaked at hundreds of MB of intermediate
// json::value_t allocations *and* left the converted Thread::messages live
// forever. SAX gives us O(file_bytes) parse cost with O(1) live state per
// file: a few dozen bytes for the metadata fields plus depth tracking.
//
// Note the on-disk key order is alphabetical (json::dump(2) sorts keys),
// so the layout is `created_at, id, messages, title, updated_at` — i.e.
// `title` and `updated_at` arrive *after* the messages array. The skip
// state must therefore unwind cleanly when the array closes, otherwise
// those two fields are silently lost.
struct ThreadMetaSax {
    Thread out;
    std::string last_key;
    int depth = 0;        // current nesting depth inside the JSON
    int skip_target = -1; // -1 == not skipping; otherwise resume when depth <= skip_target
    bool got_id = false;

    bool skipping() const noexcept { return skip_target >= 0; }

    bool key(std::string& v) {
        last_key = std::move(v);
        return true;
    }
    bool string(std::string& v) {
        if (!skipping() && depth == 1) {
            if (last_key == "id") {
                if (v.empty()) return false; // hard fail — caller treats as parse error
                out.id = ThreadId{std::move(v)};
                got_id = true;
            } else if (last_key == "title") {
                out.title = std::move(v);
            }
        }
        return true;
    }
    bool number_integer(std::int64_t v)            { return num(static_cast<long long>(v)); }
    bool number_unsigned(std::uint64_t v)          { return num(static_cast<long long>(v)); }
    bool number_float(double, const std::string&)  { return true; }
    bool num(long long v) {
        if (!skipping() && depth == 1) {
            if (last_key == "created_at")
                out.created_at = std::chrono::system_clock::time_point{
                    std::chrono::seconds{v}};
            else if (last_key == "updated_at")
                out.updated_at = std::chrono::system_clock::time_point{
                    std::chrono::seconds{v}};
        }
        return true;
    }
    bool boolean(bool)             { return true; }
    bool null()                    { return true; }
    bool start_object(std::size_t) { return enter_container(); }
    bool end_object()              { return leave_container(); }
    bool start_array(std::size_t)  { return enter_container(); }
    bool end_array()               { return leave_container(); }
    bool binary(json::binary_t&)   { return true; }
    bool parse_error(std::size_t, const std::string&,
                     const json::exception&) { return false; }

    bool enter_container() {
        // If we're at the top-level object (depth==1) and the latest key
        // was "messages", arm the skip: we'll resume when depth comes
        // back down to 1. The ++depth must come AFTER this check so that
        // nested containers inside the messages array don't re-arm it.
        if (!skipping() && depth == 1 && last_key == "messages")
            skip_target = 1;
        ++depth;
        return true;
    }
    bool leave_container() {
        --depth;
        if (skipping() && depth == skip_target)
            skip_target = -1;
        return true;
    }
};

[[nodiscard]] static std::expected<Thread, DeserializeError>
load_thread_meta_file(const fs::path& p) {
    std::ifstream ifs(p);
    if (!ifs) return std::unexpected(DeserializeError{
        DeserializeErrorKind::Io, "", "open failed: " + p.string()});
    ThreadMetaSax sax;
    bool ok = json::sax_parse(ifs, &sax);
    if (!ok || !sax.got_id) {
        return std::unexpected(DeserializeError{
            DeserializeErrorKind::JsonParse, "",
            "metadata sax parse failed: " + p.string()});
    }
    return std::move(sax.out);
}

std::expected<Thread, DeserializeError>
load_thread_file(const std::filesystem::path& p) {
    std::ifstream ifs(p);
    if (!ifs) return std::unexpected(DeserializeError{
        DeserializeErrorKind::Io, "", "open failed: " + p.string()});
    json j;
    try { ifs >> j; }
    catch (const std::exception& e) {
        return std::unexpected(DeserializeError{
            DeserializeErrorKind::JsonParse, "",
            std::string{"json parse failed: "} + e.what()});
    }
    return parse_thread(j);
}

std::vector<Thread> load_all_threads() {
    // Metadata-only directory walk — leaves Thread::messages empty. The
    // previous full-parse loaded all 649 files into Message/ToolUse trees
    // (376 MB on disk → ~1.2 GB live) at startup, even though the picker
    // only ever reads title + updated_at. Bodies are now lazy-loaded on
    // ThreadListSelect via load_thread_file.
    std::vector<Thread> out;
    std::error_code ec;
    if (!fs::exists(threads_dir(), ec)) return out;
    for (const auto& e : fs::directory_iterator(threads_dir(), ec)) {
        if (e.path().extension() != ".json") continue;
        auto loaded = load_thread_meta_file(e.path());
        if (loaded) {
            out.push_back(std::move(*loaded));
        } else {
            // Log and skip — corrupt or schema-incompatible files don't
            // kill the rest of the directory walk. The typed kind is
            // visible to anyone watching stderr; programmatic callers
            // who want a strict load can use load_thread_file directly.
            std::fprintf(stderr,
                "agentty: skipping %s — %s\n",
                e.path().string().c_str(),
                loaded.error().render().c_str());
        }
    }
    std::sort(out.begin(), out.end(), [](const Thread& a, const Thread& b){
        return a.updated_at > b.updated_at;
    });
    return out;
}

void save_thread(const Thread& t) {
    if (t.id.empty() || t.messages.empty()) return;
    json j;
    j["id"] = t.id;
    j["title"] = t.title;
    j["created_at"] = std::chrono::duration_cast<std::chrono::seconds>(
        t.created_at.time_since_epoch()).count();
    j["updated_at"] = std::chrono::duration_cast<std::chrono::seconds>(
        t.updated_at.time_since_epoch()).count();
    json msgs = json::array();
    for (const auto& m : t.messages) msgs.push_back(message_to_json(m));
    j["messages"] = std::move(msgs);
    // Wire-only compaction records. Persisting these lets a reloaded
    // thread keep sending the SAME wire payload it was sending before
    // shutdown — if the user compacted at turn 40 then closed the app,
    // the next request after reload still summarises the [0, 40) prefix
    // instead of resending all 40 raw turns and blowing context. Empty
    // for threads that have never been compacted (the common case);
    // older on-disk threads predate the field and parse_thread defaults
    // it to empty, so upgrade is transparent.
    if (!t.compactions.empty()) {
        json comps = json::array();
        for (const auto& c : t.compactions) {
            json cj;
            cj["up_to_index"] = c.up_to_index;
            cj["summary"]     = tools::util::to_valid_utf8(c.summary);
            cj["created_at"]  = std::chrono::duration_cast<std::chrono::seconds>(
                                    c.created_at.time_since_epoch()).count();
            comps.push_back(std::move(cj));
        }
        j["compactions"] = std::move(comps);
    }
    // dump() throws type_error.316 on non-UTF-8 bytes. Scrubbing in
    // message_to_json should have caught everything, but swallow the
    // exception as a belt-and-suspenders guard against future regressions —
    // a silently-skipped save beats a process-terminating uncaught throw.
    try {
        (void)write_json_atomic(threads_dir() / (t.id.value + ".json"), j.dump(2));
    } catch (const nlohmann::json::exception&) {
        // caller can't react; best-effort persistence is acceptable here.
    }
}

void delete_thread(const ThreadId& id) {
    std::error_code ec;
    fs::remove(threads_dir() / (id.value + ".json"), ec);
}

store::Settings load_settings() {
    store::Settings s;
    std::ifstream ifs(data_dir() / "settings.json");
    if (!ifs) return s;
    try {
        json j; ifs >> j;
        s.model_id = ModelId{j.value("model_id", "")};
        s.profile = static_cast<Profile>(j.value("profile", 0));
        auto favs = j.value("favorite_models", std::vector<std::string>{});
        for (auto& f : favs) s.favorite_models.push_back(ModelId{std::move(f)});
    } catch (...) {}
    return s;
}

void save_settings(const store::Settings& s) {
    json j;
    j["model_id"] = s.model_id;
    j["profile"] = static_cast<int>(s.profile);
    json favs = json::array();
    for (const auto& mid : s.favorite_models) favs.push_back(mid);
    j["favorite_models"] = std::move(favs);
    (void)write_json_atomic(data_dir() / "settings.json", j.dump(2));
}

ThreadId new_id() {
    static std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream oss;
    oss << std::hex << dist(rng);
    return ThreadId{oss.str()};
}

std::string title_from_first_message(std::string_view text) {
    std::string t{text};
    for (auto& c : t) if (c == '\n' || c == '\r') c = ' ';
    if (t.size() > 60) {
        // UTF-8-safe cut: a naive resize(57) can split a multi-byte sequence,
        // and the partial bytes propagate into json::dump() which throws
        // type_error.316 on any invalid UTF-8. Scrub afterward as belt-and-
        // suspenders in case `text` itself arrived malformed.
        t.resize(tools::util::safe_utf8_cut(t, 57));
        t = tools::util::to_valid_utf8(std::move(t));
        t += "...";
    }
    if (t.empty()) t = "New thread";
    return t;
}

} // namespace agentty::persistence

namespace agentty {

// Per-Message stable identity. Generated at Message default-construction
// (see Message::id in conversation.hpp). The cache key (thread_id,
// message_id) is stable across vector index shifts (compaction,
// deletion, reordering) so a render-cache lookup never returns a
// stale Element for a now-different message at the same position.
//
// Implementation mirrors persistence::new_id() — 64-bit random hex,
// thread-safe via static mt19937 + std::random_device. 16 hex digits
// is more than enough for within-process uniqueness; the chance of two
// IDs colliding within a session is ~2⁻³² even at a million messages,
// well below any realistic load.
MessageId new_message_id() {
    static std::mt19937_64 rng{std::random_device{}()};
    static std::mutex      mu;
    std::lock_guard<std::mutex> lk(mu);
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream oss;
    oss << std::hex << dist(rng);
    return MessageId{oss.str()};
}

} // namespace agentty
