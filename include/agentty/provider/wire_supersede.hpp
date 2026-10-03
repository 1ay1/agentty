#pragma once
// agentty::provider::wire — superseded-read collapse.
//
// A read-heavy coding loop reads the same files repeatedly: read foo.cpp,
// edit it, read it again; or read a file, then read a different slice of it.
// Every earlier read of a file whose contents a LATER turn re-read or edited
// carries a stale full-fidelity body on the wire that the model no longer
// needs — it already has fresher state from the newer turn. Age-fading
// (wire::cap_tool_result_aged) eventually shrinks it, but only after
// kFullResultWindow turns; until then a 60 KiB read replays in full every
// turn. Collapsing those stale reads to a one-line pointer IMMEDIATELY is the
// single largest token reclaim in real coding sessions.
//
// This lives in its own header (not wire.hpp, which is deliberately
// dependency-light framing) because it needs the domain types. All four
// transports (Anthropic, OpenAI-compat, ChatGPT/Codex, Ollama) share it so
// the policy \u2014 like the fade policy \u2014 exists once.

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

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
[[nodiscard]] inline std::string tool_target_path(const ToolUse& tc) {
    if (!tc.args.is_object()) return {};
    for (const char* key : {"path", "file_path", "filepath", "filename"}) {
        auto it = tc.args.find(key);
        if (it != tc.args.end() && it->is_string()) {
            std::string p = it->template get<std::string>();
            if (!p.empty()) return p;
        }
    }
    return {};
}

// Did this tool result actually CARRY the file's bytes?
//
// `read` can succeed and still return no content: the fs-layer dedup answers a
// repeat of an already-served range with "File unchanged since last read",
// pointing at the earlier result. That is sound on its own -- but only while
// the earlier result is still on the wire.
//
// The collapse below is the other half of that bargain, and the two used to
// break it together: read #1 of a path was superseded the moment read #2
// existed, and read #2 was answered with the unchanged-sentinel. Net effect,
// measured: the bytes appeared in NO tool result on the wire. The model's own
// words for it were "the two sentinels point at each other, and there is no
// tool result anywhere containing the file's text" -- then it fell back to
// `cat`, which is the read-loop users report.
//
// So a sentinel-bearing result cannot supersede anything. It is a POINTER to
// content, not a copy of it, and a pointer cannot replace the thing it points
// at.
[[nodiscard]] inline bool read_result_has_body(const ToolUse& tc) {
    const std::string_view out = tc.output();
    if (out.empty()) return false;
    // The fs layer's two no-content answers. Matched on a stable prefix of
    // each rather than the whole string: both carry a trailing hint that has
    // been reworded before and will be again.
    static constexpr std::string_view kUnchanged =
        "File unchanged since last read";
    static constexpr std::string_view kSuperseded =
        "earlier read of this file";
    return out.find(kUnchanged) == std::string_view::npos
        && out.find(kSuperseded) == std::string_view::npos;
}

// Identify earlier `read` results whose file was touched again LATER in the
// thread (by another read/edit/write/remove/move of the same path). Returns
// the set of ToolCallId strings to collapse. The MOST RECENT read of each
// file is never in the set (that's the live copy the model reasons over).
// Only terminal, successful reads participate; error reads are left untouched
// (they never fade either — the model needs the full failure text).
[[nodiscard]] inline std::unordered_set<std::string>
superseded_read_ids(const std::vector<Message>& msgs) {
    std::unordered_map<std::string, std::string> newest_read_of;  // path -> id
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

            if (carries_body) {
                if (auto it = newest_read_of.find(path); it != newest_read_of.end()) {
                    superseded.insert(it->second);
                    newest_read_of.erase(it);
                }
            }
            if (ok_read && carries_body) newest_read_of[path] = tc.id.value;
        }
    }
    return superseded;
}

[[nodiscard]] inline std::unordered_set<std::string>
superseded_read_ids(const Thread& t) {
    return superseded_read_ids(t.messages);
}

} // namespace agentty::provider::wire
