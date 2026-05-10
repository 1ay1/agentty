#include "agentty/tool/spec.hpp"
#include "agentty/tool/tools.hpp"
#include "agentty/tool/util/arg_reader.hpp"
#include "agentty/tool/util/fs_helpers.hpp"
#include "agentty/tool/util/tool_args.hpp"

#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace agentty::tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

// ── Stale-Read suppression ──────────────────────────────────────────────
// Mirrors Claude Code 2.1.119's `tengu_noreread` mechanism (binary at
// offset 134969). When the model issues a Read against a (path, offset,
// limit) it has already read AND the file's mtime hasn't moved since,
// return a tiny sentinel string instead of re-shipping the bytes:
//
//   "File unchanged since last read. The content from the earlier
//    Read tool_result in this conversation is still current — refer to
//    that instead of re-reading."
//
// The earlier tool_result is still in the conversation prefix (and
// cached via cache_control on the last message's last block), so the
// model just refers to it. A 5K-token re-read collapses to ~25 tokens.
//
// Cache is process-global, mutex-protected. A fresh agentty process starts
// empty (the natural session boundary). Edit/Write that mutate a file
// also bump its mtime, so the cache self-invalidates without any
// invalidation hooks. Different (offset, limit) requests are tracked
// separately because the sentinel points to the *earlier exact range*
// — telling the model to refer to a 1-50 read when it just asked for
// 51-100 would be wrong.
struct ReadCacheKey {
    std::string canonical_path;
    int offset = 1;
    int limit  = 2000;
    bool operator==(const ReadCacheKey&) const noexcept = default;
};
struct ReadCacheKeyHash {
    [[nodiscard]] std::size_t operator()(const ReadCacheKey& k) const noexcept {
        std::size_t h = std::hash<std::string>{}(k.canonical_path);
        h = h * 31u + static_cast<std::size_t>(k.offset);
        h = h * 31u + static_cast<std::size_t>(k.limit);
        return h;
    }
};
struct ReadCache {
    std::mutex mu;
    std::unordered_map<ReadCacheKey, fs::file_time_type, ReadCacheKeyHash> seen;
};
[[nodiscard]] ReadCache& read_cache() {
    static ReadCache c;
    return c;
}

struct ReadArgs {
    util::NormalizedPath path;
    int offset;
    int limit;
    std::string display_description;
};

std::expected<ReadArgs, ToolError> parse_read_args(const json& j) {
    util::ArgReader ar(j);
    auto path_opt = ar.require_str("path");
    if (!path_opt)
        return std::unexpected(ToolError::invalid_args("path required"));
    auto wp = util::make_workspace_path(*path_opt, "read");
    if (!wp) return std::unexpected(std::move(wp.error()));
    int offset = ar.integer("offset", 1);
    if (offset < 1) offset = 1;
    // Zed-style `end_line` is inclusive (last line shown). Translate into our
    // limit = end_line - offset + 1. Only honored when the caller actually
    // passed end_line and didn't also pass an explicit limit.
    int limit = ar.integer("limit", 2000);
    if (ar.has("end_line") && !ar.has("limit")) {
        int end_line = ar.integer("end_line", 0);
        if (end_line >= offset) limit = end_line - offset + 1;
    }
    if (limit <= 0) limit = 2000;
    return ReadArgs{
        std::move(*wp),
        offset,
        limit,
        ar.str("display_description", ""),
    };
}

ExecResult run_read(const ReadArgs& a) {
    const auto& p = a.path.path();
    std::error_code ec;
    if (!fs::exists(p, ec))
        return std::unexpected(ToolError::not_found("file not found: " + a.path.string()
            + ". Run `list_dir` on the parent directory or `glob` by name to verify."));
    if (!fs::is_regular_file(p, ec))
        return std::unexpected(ToolError::not_a_file("not a regular file: " + a.path.string()));

    // Stale-Read suppression. Compute the canonical path + current mtime
    // BEFORE the file_size / binary / read passes — we want the sentinel
    // to win even on a 1 MiB file that would otherwise blow the cap, so a
    // model that re-reads "same file again, oops" doesn't pay the read
    // cost AND get a too-large error. weakly_canonical handles symlinks
    // without requiring the path to exist beyond what `fs::exists` above
    // already proved. A failure to canonicalize (rare — typically only
    // hits on broken symlinks) just skips caching for this call.
    fs::file_time_type current_mtime{};
    {
        std::error_code mtime_ec;
        current_mtime = fs::last_write_time(p, mtime_ec);
        if (!mtime_ec) {
            std::error_code canon_ec;
            auto canon = fs::weakly_canonical(p, canon_ec);
            if (!canon_ec) {
                ReadCacheKey key{canon.string(), a.offset, a.limit};
                std::lock_guard lk{read_cache().mu};
                auto it = read_cache().seen.find(key);
                if (it != read_cache().seen.end()
                    && it->second == current_mtime) {
                    return ToolOutput{
                        "File unchanged since last read. The content from the "
                        "earlier Read tool_result in this conversation is still "
                        "current \xe2\x80\x94 refer to that instead of "
                        "re-reading.",
                        std::nullopt};
                }
            }
        }
    }
    // Size cap, enforced UNCONDITIONALLY — the previous gate was
    // `&& a.offset == 1`, which let any offset>1 read load a 10 GB file
    // into memory and scan it linearly to count lines. The cap is the
    // primary guard against accidentally feeding multi-GB files into the
    // model's context, and a wedged tool runs out the wall-clock watchdog.
    constexpr uintmax_t kMaxBytes = 1024u * 1024u;   // 1 MiB
    uintmax_t sz = fs::file_size(p, ec);
    if (!ec && sz > kMaxBytes) {
        return std::unexpected(ToolError::too_large(std::format(
            "file is {} KiB (> 1 MiB cap). "
            "Read in chunks via offset/limit (or start_line/end_line) — "
            "e.g. {{\"path\":\"{}\",\"offset\":1,\"limit\":500}}. "
            "For a structural overview, run `grep` for the symbols you need.",
            sz / 1024, a.path.string())));
    }
    // Bail on binary up-front — `is_binary_file` only opens and scans the
    // first 512 bytes for a NUL. The previous code detected binary inside
    // the line-counting loop AFTER reading the whole file, which on a
    // 1 MiB binary that's mostly valid bytes meant scanning 99% of it
    // before failing.
    if (util::is_binary_file(p)) {
        return std::unexpected(ToolError::binary(std::format(
            "cannot read binary file: {} ({} bytes). "
            "Use the bash tool with `file`, `hexdump`, or similar.",
            a.path.string(), static_cast<uintmax_t>(ec ? 0 : sz))));
    }
    // Single open, single read. Then one linear scan builds the slice
    // AND counts lines in one pass.
    auto content = util::read_file(p);
    std::string out;
    // Reserve the full content size on small files (one big alloc, no
    // resize) and cap at 1 MiB on larger files where the slice is
    // almost certainly a page of the whole thing — the realloc cascade
    // for a million-byte append is what we're avoiding, not the peak RSS.
    out.reserve(content.size() < 1024 * 1024 ? content.size() : 1024 * 1024);
    int total_lines = 0;
    int shown = 0;
    size_t line_start = 0;
    const size_t N = content.size();
    for (size_t i = 0; i < N; ++i) {
        char c = content[i];
        if (c == '\0') {
            return std::unexpected(ToolError::binary(std::format(
                "cannot read binary file: {} ({} bytes). "
                "Use the bash tool with `file`, `hexdump`, or similar "
                "if you need to inspect it.",
                a.path.string(), N)));
        }
        if (c == '\n') {
            ++total_lines;
            int n = total_lines; // 1-based index of the line just ended
            if (n >= a.offset && shown < a.limit) {
                // Normalize CRLF → LF on emit. Without this, Windows-
                // authored files come through to the model as `line\r\n`
                // and any string-matching the model does against the
                // read output silently fails (the embedded \r is invisible
                // in most TUIs but very real to a string compare).
                size_t end = i;
                if (end > line_start && content[end - 1] == '\r') --end;
                out.append(content.data() + line_start, end - line_start);
                out.push_back('\n');
                ++shown;
            }
            line_start = i + 1;
        }
    }
    // Trailing line without a final newline.
    if (line_start < N) {
        ++total_lines;
        int n = total_lines;
        if (n >= a.offset && shown < a.limit) {
            size_t end = N;
            if (end > line_start && content[end - 1] == '\r') --end;
            out.append(content.data() + line_start, end - line_start);
            out.push_back('\n');
            ++shown;
        }
    }
    if (a.offset > 1 || shown < total_lines) {
        std::string hint = std::format("\n[showing lines {}-{} of {}",
                                       a.offset, a.offset + shown - 1, total_lines);
        int remaining = total_lines - (a.offset + shown - 1);
        if (remaining > 0)
            hint += std::format("; {} more — pass offset={} (or start_line={}) "
                                "for the next chunk",
                                remaining, a.offset + shown, a.offset + shown);
        hint += "]";
        out += hint;
    }
    if (!a.display_description.empty())
        out = a.display_description + "\n\n" + out;

    // Record the (path, offset, limit) → mtime pair so the next Read of
    // the same range short-circuits to the sentinel. Computed up top so
    // we use the same canonicalisation here. A failure to canonicalise
    // (broken symlink edge case) just skips the bookkeeping; the next
    // call falls through to a real read again, which is fine.
    if (current_mtime.time_since_epoch().count() != 0) {
        std::error_code canon_ec;
        auto canon = fs::weakly_canonical(p, canon_ec);
        if (!canon_ec) {
            ReadCacheKey key{canon.string(), a.offset, a.limit};
            std::lock_guard lk{read_cache().mu};
            read_cache().seen[std::move(key)] = current_mtime;
        }
    }

    return ToolOutput{std::move(out), std::nullopt};
}

} // namespace

ToolDef tool_read() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"read">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description = "Read a file from the filesystem. Returns up to 2000 lines "
                    "starting at an optional offset. For large files, page via "
                    "offset/limit (or start_line/end_line) rather than reading "
                    "whole. Include a brief `display_description` so the user "
                    "sees why you're reading.";
    t.input_schema = json{
        {"type", "object"},
        {"required", {"path"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"path",       {{"type","string"}, {"description","Absolute or relative path"}}},
            {"offset",     {{"type","integer"}, {"description","Start line (1-based)"}}},
            {"limit",      {{"type","integer"}, {"description","Max lines"}}},
            {"start_line", {{"type","integer"}, {"description","Alias for offset (Zed-style)"}}},
            {"end_line",   {{"type","integer"}, {"description","Inclusive last line (Zed-style)"}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<ReadArgs>(parse_read_args, run_read);
    return t;
}

} // namespace agentty::tools
