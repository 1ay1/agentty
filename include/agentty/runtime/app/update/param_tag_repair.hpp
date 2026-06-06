#pragma once
// ── XML-in-JSON tool-call recovery ────────────────────────────────────
// Some model rollouts intermittently mix Anthropic's internal XML
// tool-call syntax into the JSON tool input. Instead of the clean shape
//   {"path":"f","edits":[{"old_text":"A","new_text":"B"}]}
// the wire carries syntactically-VALID JSON whose string values smuggle
// the real arguments inside `<parameter name="…">…` markers, e.g.
//   {"path":"f","edit":"\n<parameter name=\"old_text\">A","new_text":"B"}
// (note `edit`, not `edits`, and old_text hidden inside it). Because the
// JSON parses, the salvage path never runs — but the required field
// (old_text / edits) is now buried in a stray key and the call dies in
// the required-field guard with a misleading "looks incomplete" message.
// Observed responsible for ~all edit failures in a session once the model
// latched into the XML-emitting state (~27% of edits in the worst case).
//
// Recovery lifts every `<parameter name="K">V` segment out of string
// values back into proper top-level keys and rebuilds the canonical arg
// shape. SAFETY: the only caller (guard_truncated_tool_args) invokes this
// solely on the already-failing path — a well-formed edit/write whose
// content legitimately contains the literal marker (e.g. editing this
// file) has its required fields present and is never rewritten.

#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "agentty/tool/spec.hpp"

namespace agentty::app::detail {

// Lift `<parameter name="K">V` segments out of every string value in
// `args`. Each value runs from just after the `>` to the next
// `<parameter name="` / `</parameter>` / end-of-string. First occurrence
// of a given name wins; later duplicate tags are treated as noise.
inline std::map<std::string, std::string>
extract_param_tags(const nlohmann::json& args) {
    constexpr std::string_view kOpen  = "<parameter name=\"";
    constexpr std::string_view kClose = "</parameter>";
    std::map<std::string, std::string> out;
    if (!args.is_object()) return out;
    for (const auto& [k, v] : args.items()) {
        if (!v.is_string()) continue;
        const std::string& s = v.template get_ref<const std::string&>();
        std::size_t pos = 0;
        while ((pos = s.find(kOpen, pos)) != std::string::npos) {
            std::size_t name_start = pos + kOpen.size();
            std::size_t name_end   = s.find('"', name_start);
            if (name_end == std::string::npos) break;
            std::size_t gt = s.find('>', name_end);
            if (gt == std::string::npos) break;
            std::size_t val_start = gt + 1;
            std::size_t next  = s.find(kOpen,  val_start);
            std::size_t close = s.find(kClose, val_start);
            std::size_t val_end = std::min(
                next  == std::string::npos ? s.size() : next,
                close == std::string::npos ? s.size() : close);
            out.emplace(s.substr(name_start, name_end - name_start),
                        s.substr(val_start, val_end - val_start));
            pos = val_end;
        }
    }
    return out;
}

// Rewrites `args` into the canonical shape and returns true when an XML
// parameter-tag leak is detected and recovered; returns false (leaving
// `args` untouched) otherwise. MUST be called only when the tool call
// would already fail its required-field check — see the header note.
inline bool repair_param_tag_leak(std::string_view tool_name,
                                  nlohmann::json& args) {
    using nlohmann::json;
    if (!args.is_object()) return false;
    auto kind = tools::spec::kind_of(tool_name);
    if (!kind) return false;

    constexpr std::string_view kOpen = "<parameter name=\"";
    bool marker = false;
    for (const auto& [k, v] : args.items())
        if (v.is_string()
            && v.template get_ref<const std::string&>().find(kOpen)
                   != std::string::npos) {
            marker = true; break;
        }
    if (!marker) return false;

    auto tags = extract_param_tags(args);
    if (tags.empty()) return false;

    // Prefer a clean top-level string field (one that isn't itself a leak);
    // fall back to the value recovered from a parameter tag.
    auto pick = [&](std::initializer_list<const char*> names)
        -> std::optional<std::string> {
        for (const char* n : names)
            if (auto it = args.find(n); it != args.end() && it->is_string()) {
                const std::string& s = it->template get_ref<const std::string&>();
                if (!s.empty() && s.find(kOpen) == std::string::npos) return s;
            }
        for (const char* n : names)
            if (auto it = tags.find(n); it != tags.end() && !it->second.empty())
                return it->second;
        return std::nullopt;
    };

    using K = tools::spec::Kind;
    if (*kind == K::Edit) {
        auto old_t = pick({"old_text", "old_string"});
        if (!old_t) return false;   // nothing recoverable
        auto new_t = pick({"new_text", "new_string"});
        auto path  = pick({"path", "file_path", "filepath", "filename"});

        json one = json::object();
        one["old_text"] = std::move(*old_t);
        one["new_text"] = new_t.value_or("");
        if (auto it = tags.find("line"); it != tags.end()) {
            try { one["line"] = std::stoi(it->second); } catch (...) {}
        } else if (auto it = args.find("line");
                   it != args.end() && it->is_number_integer()) {
            one["line"] = *it;
        }

        json rebuilt = json::object();
        if (path) rebuilt["path"] = std::move(*path);
        if (auto it = args.find("display_description");
            it != args.end() && it->is_string())
            rebuilt["display_description"] = *it;
        rebuilt["edits"] = json::array({std::move(one)});
        args = std::move(rebuilt);
        return true;
    }
    if (*kind == K::Write) {
        auto content = pick({"content", "file_text", "text",
                             "file_content", "contents", "body", "data"});
        if (!content) return false;
        auto path = pick({"path", "file_path", "filepath", "filename"});

        json rebuilt = json::object();
        if (path) rebuilt["path"] = std::move(*path);
        if (auto it = args.find("display_description");
            it != args.end() && it->is_string())
            rebuilt["display_description"] = *it;
        rebuilt["content"] = std::move(*content);
        args = std::move(rebuilt);
        return true;
    }
    return false;
}

} // namespace agentty::app::detail
