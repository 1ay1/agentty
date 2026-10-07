#pragma once
// agentty::provider::wire — superseded-read collapse.
//
// A read-heavy coding loop reads the same files repeatedly: read foo.cpp,
// edit it, read it again; or re-read the same slice after a detour. Every
// earlier read whose bytes a LATER turn re-served or invalidated carries a
// stale full-fidelity body on the wire that the model no longer needs — it
// already has fresher state from the newer turn. Age-fading
// (wire::cap_tool_result_aged) eventually shrinks it, but only after
// kFullResultWindow turns; until then a 60 KiB read replays in full every
// turn. Collapsing those stale reads to a one-line pointer IMMEDIATELY is the
// single largest token reclaim in real coding sessions.
//
// "Stale" is the whole game, and it is narrower than "same path": a later
// read of a DIFFERENT slice does not carry the earlier slice's lines, so it
// cannot stand in for it. See the span logic below.
//
// This lives in its own header (not wire.hpp, which is deliberately
// dependency-light framing) because it needs the domain types. All four
// transports (Anthropic, OpenAI-compat, ChatGPT/Codex, Ollama) share it so
// the policy \u2014 like the fade policy \u2014 exists once.

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "agentty/domain/conversation.hpp"

namespace agentty::provider::wire {

// The deterministic pointer a superseded read collapses to. FIXED text (no
// byte counts / positions) so a given superseded read always serialises
// identically — the prompt cache never churns on it.
inline constexpr std::string_view kSupersededReadPointer =
    "(earlier read of this file \u2014 superseded by a later read or edit of the "
    "same file; refer to the more recent tool result for its current contents)";

// The file path a tool call targets, if any. Empty for non-file tools.
// read/edit/write/remove/move all carry the target in `path` (or an alias).
//
// Normalised to one absolute spelling, because the collapse compares paths as
// strings: without it a later `edit` on "/abs/src/x.cpp" does not supersede an
// earlier `read` of "src/x.cpp", and the wire keeps serving pre-edit bytes as
// if they were current.
[[nodiscard]] inline std::string tool_target_path(const ToolUse& tc) {
    if (!tc.args.is_object()) return {};
    for (const char* key : {"path", "file_path", "filepath", "filename"}) {
        auto it = tc.args.find(key);
        if (it == tc.args.end() || !it->is_string()) continue;
        std::string p = it->template get<std::string>();
        if (p.empty()) continue;
        std::filesystem::path fp{p};
        if (fp.is_relative()) {
            static const std::filesystem::path cwd = [] {
                std::error_code ec;
                auto c = std::filesystem::current_path(ec);
                return ec ? std::filesystem::path{} : c;
            }();
            if (!cwd.empty()) fp = cwd / fp;
        }
        return fp.lexically_normal().generic_string();
    }
    return {};
}

// ── what a read result actually put on the wire ──────────────────────────
//
// The collapse used to key on the PATH alone, which quietly assumed every
// read of a file is a substitute for every other read of it. It is not: a
// window read carries one slice, and `read` is explicitly built for paging a
// big file slice by slice. So reading a second region erased the first, and
// the model got a pointer to a result that never held those lines. That is
// unrecoverable mid-turn — it reads as "the read tool is broken" and the model
// either re-reads in a loop or falls back to `cat`. Measured over 40 recent
// threads: 121 of 3323 reads came back as a pointer this way.
//
// So a read is collapsed only when a later read's span CONTAINS it.

inline constexpr int kWholeFile = std::numeric_limits<int>::max();

// A 1-based inclusive line span. `known == false` means "can't tell": such a
// result neither supersedes nor is superseded by another read, because a
// missed collapse only costs tokens while a wrong one costs the bytes.
struct ReadSpan {
    bool known = false;
    int  lo    = 1;
    int  hi    = 0;
};

[[nodiscard]] inline bool covers(const ReadSpan& outer, const ReadSpan& inner) noexcept {
    return outer.known && inner.known
        && outer.lo <= inner.lo && outer.hi >= inner.hi;
}

// Ground truth, read off the body. `read` prints a `[showing lines A-B of N]`
// footer whenever it withheld anything, so its ABSENCE means the whole file
// is in this result.
//
// nullopt  = the body says nothing either way, ask the args.
// unknown  = the body says it carries no line range at all (an outline, or a
//            past-the-end offset) — nothing to compare, stop here.
[[nodiscard]] inline std::optional<ReadSpan>
read_span_from_output(std::string_view out) {
    constexpr auto npos = std::string_view::npos;
    // An outline is a symbol index, not file content: it cannot stand in for
    // any line range, not even the one it was asked for.
    if (out.find("# Outline of ") != npos) return ReadSpan{};
    if (out.find("is past the end of the file") != npos) return ReadSpan{};

    constexpr std::string_view kTag = "[showing lines ";
    const auto at = out.rfind(kTag);
    if (at == npos) {
        if (out.empty()) return ReadSpan{};
        return ReadSpan{true, 1, kWholeFile};
    }
    const char* p = out.data() + at + kTag.size();
    const char* e = out.data() + out.size();
    int lo = 0;
    const auto r1 = std::from_chars(p, e, lo);
    if (r1.ec != std::errc{} || r1.ptr == e || *r1.ptr != '-') return std::nullopt;
    int hi = 0;
    const auto r2 = std::from_chars(r1.ptr + 1, e, hi);
    if (r2.ec != std::errc{} || hi < lo) return std::nullopt;
    return ReadSpan{true, lo, hi};
}

// Fallback for a body we can't read (a thread saved by an older build, a
// reworded footer). Mirrors parse_read_args in mcp-cpp's fs.cpp. If that
// drifts, this only gets more conservative — the body parse above is what
// runs on a current build.
[[nodiscard]] inline ReadSpan read_span_from_args(const nlohmann::json& args) {
    if (!args.is_object()) return {};
    if (auto it = args.find("symbol");
        it != args.end() && it->is_string() && !it->template get<std::string>().empty())
        return {};   // a symbol's lines aren't knowable from the args

    auto num = [&](std::initializer_list<const char*> keys) -> std::optional<int> {
        for (const char* k : keys) {
            auto it = args.find(k);
            if (it != args.end() && it->is_number_integer())
                return it->template get<int>();
        }
        return std::nullopt;
    };
    const auto off = num({"offset", "start_line", "start", "from_line"});
    const auto lim = num({"limit", "num_lines", "max_lines", "count"});
    const auto end = num({"end_line", "to_line", "endLine"});

    if (off && *off < 0) return {};   // tail read: no absolute span
    const int lo = (off && *off > 0) ? *off : 1;
    if (end && *end >= lo) return ReadSpan{true, lo, *end};
    if (lim && *lim > 0)   return ReadSpan{true, lo, lo + *lim - 1};
    // No end: a start-anchored read gets a focused window, a bare read(path)
    // gets the whole-file default.
    if (off) return ReadSpan{true, lo, lo + 249};
    return ReadSpan{true, 1, 2000};
}

[[nodiscard]] inline ReadSpan read_span(const ToolUse& tc) {
    if (auto s = read_span_from_output(tc.output())) return *s;
    return read_span_from_args(tc.args);
}

// Did this tool result actually CARRY the file's bytes?
//
// Most read results do, including a CACHE HIT: the fs layer replays the exact
// text it served before under a `[cached — unchanged …]` tag, so that result
// is a full copy and may supersede an older one like any other.
//
// What cannot supersede is a POINTER: a result whose body only says "look at
// an earlier tool_result". The fs layer used to answer every repeat that way,
// and this collapse plus that sentinel broke each other -- read #1 was
// superseded the moment read #2 existed, and read #2 carried no bytes, so the
// content appeared in NO tool result on the wire. Measured; the model's own
// words were "the two sentinels point at each other, and there is no tool
// result anywhere containing the file's text", after which it fell back to
// `cat`. Claude Code has the same failure open as issues #53578 / #60684.
//
// The fs layer now serves content instead of a pointer, so this guard should
// never fire on a current build. It stays for two reasons: a thread SAVED by
// an older build rehydrates with the old sentinel text, and the invariant --
// a pointer cannot replace the thing it points at -- is worth enforcing
// structurally rather than trusting the layer below not to regress.
[[nodiscard]] inline bool read_result_has_body(const ToolUse& tc) {
    std::string_view out = tc.output();
    while (!out.empty() && (out.front() == '\n' || out.front() == ' '))
        out.remove_prefix(1);
    if (out.empty()) return false;
    // Anchored, not searched. These sentinels replace the WHOLE body, and
    // this repo's own sources quote them — a substring search called a
    // genuine read of this very header "bodyless", which then hid it from
    // the edit-invalidation path and served pre-edit bytes as current.
    // `[cached` is deliberately absent: that body holds the real text.
    static constexpr std::string_view kUnchanged =
        "File unchanged since last read";   // legacy, no longer emitted
    return !out.starts_with(kSupersededReadPointer)
        && !out.starts_with(kUnchanged);
}

// Identify earlier `read` results whose file was touched again LATER in the
// thread (by another read/edit/write/remove/move of the same path). Returns
// the set of ToolCallId strings to collapse. The MOST RECENT read of each
// file is never in the set (that's the live copy the model reasons over).
// Only terminal, successful reads participate; error reads are left untouched
// (they never fade either — the model needs the full failure text).
[[nodiscard]] inline std::unordered_set<std::string>
superseded_read_ids(const std::vector<Message>& msgs) {
    struct Live { std::string id; ReadSpan span; };
    std::unordered_map<std::string, std::vector<Live>> live;   // path -> live reads
    std::unordered_set<std::string> superseded;

    for (const auto& m : msgs) {
        if (m.role != Role::Assistant) continue;
        for (const auto& tc : m.tool_calls) {
            const std::string path = tool_target_path(tc);
            if (path.empty()) continue;

            const bool is_read = tc.name.value == "read";
            const bool ok_read = is_read
                && tc.is_terminal() && !tc.is_failed() && !tc.is_rejected();
            // A read that served only a sentinel is a pointer to an earlier
            // result, so it must neither supersede that result nor become the
            // live copy -- doing either is what stranded the bytes entirely.
            // A WRITE/EDIT/REMOVE still supersedes: it changed the file, so
            // the old body is genuinely stale whatever it contained.
            const bool carries_body = !is_read || read_result_has_body(tc);
            if (!carries_body) continue;

            auto& bucket = live[path];
            if (!is_read) {
                // The file changed: every earlier read of it is stale
                // whatever range it held, so range doesn't enter into it.
                for (const auto& l : bucket) superseded.insert(l.id);
                bucket.clear();
                continue;
            }
            const ReadSpan span = read_span(tc);
            std::erase_if(bucket, [&](const Live& l) {
                if (!covers(span, l.span)) return false;
                superseded.insert(l.id);
                return true;
            });
            // Unknown-span reads go in the bucket too: a read can't collapse
            // them (covers() is false), but a later EDIT must.
            if (ok_read) bucket.push_back({tc.id.value, span});
        }
    }
    return superseded;
}

[[nodiscard]] inline std::unordered_set<std::string>
superseded_read_ids(const Thread& t) {
    return superseded_read_ids(t.messages);
}

} // namespace agentty::provider::wire
